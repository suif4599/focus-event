# focus-event

React to [niri](https://github.com/YaLTeR/niri) window focus changes by running
configured commands. Built as a split daemon:

- **`focus-event-executor`** — a privileged system service that listens on a
  Unix domain socket and fork+execs commands. Validates each connecting peer
  via `SO_PEERCRED` against an optional uid/username allowlist.
- **`focus-event-trigger`** — a user-space process spawned by niri (via niri's
  `spawn-at-startup`). Reads the niri event stream, matches windows against
  the rule table, and forwards SPAWN commands to the executor over the socket.

The split lets focus changes drive privileged actions (e.g. `keyd`, system
`pactl`) without ever running the rule engine as root. The trigger inherits
the user's environment naturally — `XDG_RUNTIME_DIR`, `NIRI_SOCKET`, `PATH` —
so there's no uid-resolution dance or PATH munging like there'd be with a
single system service.

## Architecture

```
 User session (niri)                                System (root)
 ┌────────────────────────────────┐                 ┌────────────────────────────┐
 │  niri                          │                 │  focus-event-executor      │
 │   └─ spawn-at-startup          │                 │   listens on               │
 │       └─ focus-event-trigger   │                 │   /run/focus-event/sock    │
 │            │                   │                 │   ↓ per conn: SO_PEERCRED  │
 │            │ event-stream      │                 │   ↓ allowlist check        │
 │            ▼                   │   UDS frames    │                            │
 │     rule engine + cache  ──────┼─────────────────┼─► SPAWN argv → fork+setsid │
 │            │                   │                 │   ↓                        │
 │            ▼ config (KDL)      │                 │   privileged command runs │
 └────────────────────────────────┘                 └────────────────────────────┘
```

The protocol is length-prefixed binary frames over a Unix domain socket. See
[src/lib/protocol.hpp](src/lib/protocol.hpp). Two tags exist: `h` (hello, so
the executor's logs read nicer) and `s` (spawn — argc + length-prefixed argv).

## Quick start (NixOS)

Add the flake and enable the module:

```nix
{
  inputs.focus-event.url = "github:your-name/focus-event";

  outputs = { self, nixpkgs, focus-event, ... }: {
    nixosConfigurations.myhost = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        focus-event.nixosModules.default
        {
          services.focus-event = {
            enable = true;
            allowedUsers = [ "alice" ];
            rules = [
              {
                trigger = "focus";
                app-id = "codium";
                title = ".*focus-event.*";
                spawn = [ [ "keyd" "bind" "control.j = C-S-tab" ] ];
              }
              {
                trigger = "blur";
                app-id = "codium";
                spawn = [ [ "keyd" "unbind" ] ];
              }
            ];
          };
        }
      ];
    };
  };
}
```

Then in your niri config:

```kdl
spawn-at-startup "focus-event"
```

That's the entire user-facing surface. The module:

1. Renders `services.focus-event.rules` into a KDL file in the nix store.
2. Installs a `focus-event` wrapper script (in `environment.systemPackages`)
   that invokes `focus-event-trigger --config <rendered.kdl> --socket
   /run/focus-event/sock`.
3. Starts `systemd.services.focus-event` (the executor) at boot, with
   `RuntimeDirectory=focus-event` so `/run/focus-event/` exists and is cleaned
   up across restarts.

## Options

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `enable` | bool | `false` | Enable the executor service and install the trigger wrapper. |
| `rules` | list of submodule | `[]` | Structured rules → rendered into the KDL config the trigger reads. |
| `extraConfig` | lines | `""` | Raw KDL appended after the generated rules (escape hatch). |
| `allowedUsers` | list of str | `[]` | Usernames allowed to connect to the executor. Resolved at runtime via `getpwnam`, so lazy-uid NixOS accounts work. Empty = allow any local uid (audit-logged via `SO_PEERCRED` only). |

Each rule submodule:

| Field | Type | Default | Description |
| --- | --- | --- | --- |
| `trigger` | `"focus"` or `"blur"` | — | When to fire. |
| `app-id` | nullOr str | `null` | Regex matched (substring) against `app_id`. |
| `title` | nullOr str | `null` | Regex matched (substring) against `title`. |
| `id` | nullOr uint | `null` | Exact numeric window id. |
| `workspace-id` | nullOr uint | `null` | Match windows on this workspace. |
| `is-floating` | nullOr bool | `null` | Match floating (true) or tiled (false). |
| `is-urgent` | nullOr bool | `null` | Match urgent (true) or non-urgent (false). |
| `spawn` | list of (list of str) | `[]` | Spawn actions; each element is an argv. Multiple actions fire in order. |

Within a rule, selectors are AND-combined. `on-blur` fires against the window
losing focus; `on-focus` fires against the window gaining focus.

## Security model

- **Socket perms.** The executor creates the socket with mode `0660` and
  group `root` (configurable via `--mode`). Only users with write access can
  even attempt a connection.
- **`SO_PEERCRED`.** On each accepted connection, the executor reads the
  kernel-verified `{pid, uid, gid}` of the peer. If `--allow-uid N` or
  `--allow-user NAME` was given (repeatable), connections whose uid isn't in
  the resolved set are dropped before any frame is read. Usernames are
  re-resolved via `getpwnam` on each connection so newly-created users work
  without restarting the executor.
- **Defense in depth.** The NixOS module wires `allowedUsers` into
  `--allow-user`, but the underlying socket file mode (`0660`) is the first
  gate. Set `allowedUsers = [ "alice" ]` for a single-user workstation; leave
  it empty for a fully-trust-the-socket-perms setup.

If a peer is rejected, the executor logs `executor: rejecting uid N pid P
(not in allowlist)` and closes the connection without reading any frames.

## Why the split?

A single daemon trying to be both "user-space focus watcher" and
"privileged spawn runner" runs into a mess of environment issues:

- `NIRI_SOCKET` lives in the user's environment, but the daemon needs to be
  privileged to spawn commands.
- Resolving the user's uid at runtime requires a wrapper script and gives
  systemd ordering head-aches (graphical.target cycles).
- The daemon must duplicate the user's `PATH` to find `niri`, and any
  spawn actions inherit the wrong environment.

The split eliminates all of that:

- The **trigger** is just a user process — niri spawned it, so its env is
  complete and correct by construction.
- The **executor** is a dumb, privileged pipe-cleaner: it doesn't know about
  niri, windows, or rules. It just spawn()s what an authenticated peer sends.
- The boundary is the Unix domain socket, with `SO_PEERCRED` as the
  authentication mechanism (kernel-trusted, no spoofing possible).

## How the trigger works

```
                ┌──────────────────┐
                │ niri msg -j      │
                │ event-stream     │  (long-running subprocess)
                └────────┬─────────┘
                         │ stdout pipe, one JSON line per event
                         ▼
              ┌──────────────────────┐
              │  epoll_wait (loop)   │  no polling, no threads
              └────────┬─────────────┘
                       │
                       ▼
              ┌──────────────────────┐    on cache miss for an id
              │  line buffer + JSON  │ ──────────────────────────► ┐
              │  parse (nlohmann)    │                            │
              └────────┬─────────────┘                            │
                       │                                            │
                       ▼                                            ▼
              ┌──────────────────────┐               ┌────────────────────────┐
              │  window cache        │ ◄──────────── │ niri msg -j windows    │
              │  (unordered_map)     │   full rebuild│ (one-shot subprocess)  │
              └────────┬─────────────┘               └────────────────────────┘
                       │
                       ▼
              ┌──────────────────────┐
              │  rule engine         │  selector match (regex on app-id/title)
              └────────┬─────────────┘
                       │
                       ▼
              ┌──────────────────────┐
              │  SPAWN frame → UDS   │  → executor (privileged)
              └──────────────────────┘
```

Highlights:

- **No polling.** A single `epoll_wait` blocks until the niri event-stream
  emits a line.
- **Cheap focus changes.** The window cache is maintained incrementally from
  `WindowOpenedOrChanged` / `WindowClosed`. A full `niri msg -j windows`
  refresh happens only on a cache miss (typically never after bootstrap).
- **Debouncing.** Repeated `WindowFocusChanged` events with the same id
  (common around overview open/close) are coalesced.
- **Reconnect.** If the executor restarts, the trigger transparently
  reconnects on the next spawn attempt.

## Bare binaries (non-NixOS)

```sh
nix build                              # or: cmake -B build && cmake --build build
./result/bin/focus-event-executor --socket /tmp/fe.sock --mode 0660 --allow-user "$USER" &
./result/bin/focus-event-trigger --socket /tmp/fe.sock --config config.example.kdl
```

For local development without an executor, pass `--socket ""` to the trigger
and it will spawn commands directly (via fork+setsid+execvp) instead of
forwarding them. Useful for iterating on the rule engine without systemd.

## Configuration file format

The NixOS module renders a structured `rules` option into KDL. If you'd
rather write KDL directly, here's the grammar:

```kdl
// Comments use // or /* */.

on-focus app-id="codium" title=".*focus-event.*" is-floating=false id=42 {
    spawn "pactl" "set-sink-mute" "@DEFAULT_SINK@" "1"
    spawn "notify-send" "focused"
}

on-blur app-id="codium" {
    spawn "pactl" "set-sink-mute" "@DEFAULT_SINK@" "0"
}
```

Default config path for the trigger (when not invoked via the wrapper):
`$XDG_CONFIG_HOME/focus-event/config.kdl`, falling back to
`~/.config/focus-event/config.kdl`. Override with `--config PATH`.

## Development

```sh
nix develop                            # cmake, ninja, gcc, niri
cmake -B build -G Ninja
ninja -C build

# Live test against your running niri session:
./build/focus-event-executor --socket /tmp/fe.sock --mode 0660 --allow-user "$USER" &
./build/focus-event-trigger --socket /tmp/fe.sock --config config.example.kdl
```

To validate the NixOS module evals cleanly inside a minimal `nixosSystem`:

```sh
nix eval --impure --expr 'let cfg = (import ./nix/test/eval.nix) {}; in cfg.config.systemd.services.focus-event'
```

## Project layout

```
.
├── CMakeLists.txt             # Builds lib + executor + trigger
├── flake.nix                  # packages, overlay, nixosModules.default, lib
├── config.example.kdl         # hand-written example
├── example-events.txt         # sample event-stream lines (offline testing)
├── example-windows.txt        # sample niri msg -j windows output
├── nix/
│   ├── lib.nix                # renderConfig: rules → KDL (pure Nix)
│   ├── default.nix            # the NixOS module (service + package)
│   └── test/eval.nix          # smoke-test NixOS config
├── src/
│   ├── lib/                   # shared static lib
│   │   ├── kdl.{cpp,hpp}      # KDL-like parser
│   │   ├── config.{cpp,hpp}   # rule structs + selector matcher (regex)
│   │   ├── niri.{cpp,hpp}     # event-stream + windows JSON codec
│   │   ├── epoll.{cpp,hpp}    # thin epoll wrapper
│   │   ├── subprocess.{cpp,hpp} # fork+exec helpers (pipe, capture, double-fork)
│   │   ├── uds.{cpp,hpp}      # Unix domain socket listen/connect/read/write
│   │   ├── protocol.hpp       # length-prefixed SPAWN/HELLO framing
│   │   └── engine.{cpp,hpp}   # window cache + rule dispatch (Spawner interface)
│   ├── executor_main.cpp      # privileged socket server
│   └── trigger_main.cpp       # event-stream watcher + UDS client
└── third_party/
    └── nlohmann/json.hpp      # vendored single-header JSON parser
```

## License

MIT.
