# focus-event

A tiny daemon that watches the running [niri](https://github.com/YaLTeR/niri)
compositor's event stream and runs configured commands when windows gain or
lose keyboard focus. Built in C++20 on top of `epoll`, with zero polling and a
self-maintained window-info cache.

The flake ships:

- `packages.<system>.default` — the bare binary, for non-NixOS use
- `nixosModules.system` — system-level systemd service (runs as root, talks to
  a chosen user's niri via runtime uid lookup + `NIRI_SOCKET` globbing)
- `nixosModules.user` — user-level systemd service (runs as the logged-in user,
  inherits their env)
- `lib.renderConfig` — pure-Nix KDL renderer for the structured `rules` options
- `overlays.default` — adds `pkgs.focus-event` to your pkgs

## Quick start (NixOS)

Add the flake and pick a module:

```nix
{
  inputs.focus-event.url = "github:your-name/focus-event";

  outputs = { self, nixpkgs, focus-event, ... }: {
    nixosConfigurations.myhost = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        focus-event.nixosModules.user   # or .system
        {
          services.focus-event-user = {
            enable = true;
            rules = [
              {
                trigger = "focus";
                app-id = "codium";
                title = ".*focus-event.*";
                spawn = [ [ "pactl" "set-sink-mute" "@DEFAULT_SINK@" "1" ] ];
              }
              {
                trigger = "blur";
                app-id = "codium";
                spawn = [ [ "pactl" "set-sink-mute" "@DEFAULT_SINK@" "0" ] ];
              }
            ];
          };
        }
      ];
    };
  };
}
```

For the **system** module, you must also specify which user's niri to talk to:

```nix
services.focus-event-system = {
  enable = true;
  user = "alice";
  rules = [ /* ... */ ];
};
```

## Two flavours

### `nixosModules.user` — per-user service

Installs a systemd **user** unit at `/etc/systemd/user/focus-event.service`
(via `systemd.packages`). The unit inherits the user's environment —
`XDG_RUNTIME_DIR`, `NIRI_SOCKET`, `WAYLAND_DISPLAY`, etc. — so no env munging
is required. After `nixos-rebuild switch`, enable it once per user:

```sh
systemctl --user enable --now focus-event.service
```

The unit binds into `graphical-session.target` so it starts with the user's
Wayland session.

### `nixosModules.system` — system service

Installs a regular `systemd.services.focus-event` that runs as root at boot.
Because root doesn't have the user's env, the unit's `ExecStart` is wrapped by
[`nix/wrapper-system.sh`](nix/wrapper-system.sh), which:

1. Resolves the runtime uid via `id -u <user>` (NixOS may assign uids lazily —
   the wrapper tolerates this by deferring lookup to start time)
2. Globs `/run/user/<uid>/niri.wayland-*.sock` to find the running niri's IPC
   socket (the PID-suffixed name is not known at nixos-rebuild time)
3. Exports `XDG_RUNTIME_DIR` and `NIRI_SOCKET`
4. Execs the focus-event binary

If the user isn't logged in yet or niri isn't up, the wrapper exits non-zero
and systemd retries with `Restart=on-failure`, `RestartSec=5s`.

## Options

Both modules share the same rule schema; only the system module additionally
requires `user`.

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `enable` | bool | `false` | Enable the module. |
| `user` | string | *(system: required)* | Username whose niri to talk to (system module only). |
| `rules` | list of submodule | `[]` | Structured rules; rendered to KDL. |
| `extraConfig` | lines | `""` | Raw KDL appended after generated rules (escape hatch). |

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
| `spawn` | list of (list of str) | `[]` | Spawn actions; each element is an argv. |

Selectors within a rule are combined with logical AND. A rule with no
selectors matches every window. Multiple spawn actions fire in declaration
order. `on-focus` and `on-blur` both fire against the window gaining / losing
focus — `on-blur` against the previously focused window, `on-focus` against the
newly focused one.

## How it works

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
              │  spawn (setsid+exec) │  fire-and-forget, no wait
              └──────────────────────┘
```

Highlights:

- **No polling.** A single `epoll_wait` blocks until the niri event-stream
  emits a line. No timers, no sleep loops, no wakeups on idle.
- **Cheap focus changes.** The daemon maintains an in-memory window cache
  (`WindowOpenedOrChanged` and `WindowClosed` update it incrementally). A
  `WindowFocusChanged` only triggers `niri msg -j windows` if its id is missing
  from the cache — typically never after the initial bootstrap.
- **Fire-and-forget spawns.** Action commands `fork`/`setsid`/`exec` so they
  outlive the daemon and don't block it.
- **Debouncing.** Repeated `WindowFocusChanged` events with the same id (niri
  emits these around `OverviewOpenedOrClosed`) are coalesced into a single
  focus event.
- **KDL config.** The format mirrors niri's own config style — `on-focus` /
  `on-blur` blocks with selector properties and `spawn` children.

## Configuration file format

The NixOS modules render a structured `rules` option into KDL. If you'd rather
write KDL directly (e.g. when running the bare binary), here's the grammar:

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

Selectors on a rule are AND-combined; absent selectors match anything.

Default config path: `$XDG_CONFIG_HOME/focus-event/config.kdl`, falling back to
`~/.config/focus-event/config.kdl`. Override with `--config PATH`.

## Bare binary (non-NixOS)

```sh
nix build                              # or: cmake -B build && cmake --build build
./result/bin/focus-event --config path/to/config.kdl
```

Useful when iterating on the C++ side without going through a full NixOS
rebuild.

## Development

```sh
nix develop                            # cmake, ninja, gcc, niri
cmake -B build -G Ninja
ninja -C build
./build/focus-event --config config.example.kdl
```

For fast iteration against a live niri session, run the binary directly with a
scratch config:

```kdl
on-focus { spawn "sh" "-c" "echo focus $(date) >> /tmp/fe.log" }
on-blur  { spawn "sh" "-c" "echo blur  $(date) >> /tmp/fe.log" }
```

Switch windows a few times; `/tmp/fe.log` should accumulate one line per
transition.

## Testing the modules

The flake ships two test harnesses that just check whether the modules eval
inside a minimal `nixosSystem`:

```sh
nix eval --impure --expr 'let cfg = (import ./nix/test/eval-system.nix) {}; in cfg.config.systemd.services.focus-event'
nix eval --impure --expr 'let cfg = (import ./nix/test/eval-user.nix)   {}; in cfg.config.systemd.packages'
```

To manually verify the system module's wrapper can talk to your running niri
(without going through systemd):

```sh
env -u NIRI_SOCKET -u XDG_RUNTIME_DIR \
  ./nix/wrapper-system.sh $(whoami) ./build/focus-event --config config.example.kdl
```

## Project layout

```
.
├── CMakeLists.txt             # C++20, gcc, no external deps
├── flake.nix                  # packages, modules, lib, overlay, dev shell
├── config.example.kdl         # hand-written example
├── example-events.txt         # sample event-stream lines (for offline testing)
├── example-windows.txt        # sample niri msg -j windows output
├── nix/
│   ├── lib.nix                # renderConfig: rules → KDL (pure Nix)
│   ├── wrapper-system.sh      # uid resolution + NIRI_SOCKET globbing for system module
│   ├── modules/
│   │   ├── system.nix         # systemd.services.focus-event
│   │   └── user.nix           # systemd.packages with user unit
│   └── test/
│       ├── eval-system.nix
│       └── eval-user.nix
├── src/
│   ├── main.cpp               # entry, SIGCHLD reaper, epoll loop, line buffer
│   ├── kdl.{cpp,hpp}          # minimal KDL-like parser (lexer + AST)
│   ├── config.{cpp,hpp}       # rule structs, selector matcher (regex)
│   ├── niri.{cpp,hpp}         # event-stream + windows JSON parsing
│   ├── epoll.{cpp,hpp}        # thin epoll wrapper
│   ├── subprocess.{cpp,hpp}   # fork+exec helpers (pipe, capture, setsid)
│   └── engine.{cpp,hpp}       # window cache + rule dispatch
└── third_party/
    └── nlohmann/json.hpp      # vendored single-header JSON parser
```

## License

MIT.
