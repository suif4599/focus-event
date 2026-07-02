# NixOS module for focus-event.
#
# Architecture:
#   - The privileged half (focus-event-executor) runs as a system service. It
#     listens on a Unix domain socket and fork+execs commands sent to it by the
#     trigger. Authentication is binary-identity-based: the executor reads
#     SO_PEERCRED to get the peer pid, resolves /proc/<pid>/exe, and only
#     accepts the connection if the resolved path matches the trigger binary
#     built into this Nix generation.
#   - The user half (focus-event-trigger) is spawned by niri via
#     `spawn-at-startup`. It inherits the user's NIRI_SOCKET / XDG_RUNTIME_DIR
#     / PATH so it can talk to niri without any env munging.
#   - The module renders the structured `rules` option into a KDL config file
#     and ships a wrapper script (`focus-event`) that invokes the trigger with
#     the right --config and --socket, so niri's spawn-at-startup just needs:
#
#         spawn-at-startup "focus-event"
#
# Usage:
#   services.focus-event = {
#     enable = true;
#     rules = [
#       { trigger = "focus"; app-id = "codium";
#         spawn = [ [ "keyd" "bind" "control.j = C-S-tab" ] ]; }
#     ];
#   };

{ config, lib, pkgs, ... }:

let
  focus-event-lib = import ./lib.nix { inherit lib; };
  cfg = config.services.focus-event;

  # The C++ package: contains focus-event-executor and focus-event-trigger
  # binaries. Resolved via the overlay the flake installs alongside this
  # module.
  pkg = pkgs.focus-event;

  # KDL config rendered from the structured `rules` option. Lives in the
  # nix store; the trigger reads it read-only.
  kdlConfig = pkgs.writeText "focus-event.kdl" (focus-event-lib.renderConfig cfg);

  # The socket lives in /run/focus-event/, which systemd's RuntimeDirectory
  # creates and cleans up for us.
  socketPath = "/run/focus-event/sock";

  # Path of the trusted trigger binary, baked into the executor at service
  # start. The store path already encodes a content hash, so comparing
  # /proc/<pid>/exe against this is equivalent to verifying a SHA256 of the
  # binary without needing a hashing dependency.
  expectedTrigger = "${pkg}/bin/focus-event-trigger";

  # Wrapper script that the user adds to niri's spawn-at-startup. Knows the
  # --config and --socket paths so the user just types "focus-event".
  triggerWrapper = pkgs.writeShellScriptBin "focus-event" ''
    exec ${pkg}/bin/focus-event-trigger \
      --config ${kdlConfig} \
      --socket ${socketPath} \
      "$@"
  '';
in
{
  options.services.focus-event = {
    enable = lib.mkEnableOption "focus-event daemon (executor system service + trigger wrapper)";

    rules = lib.mkOption {
      type = lib.types.listOf (lib.types.submodule (focus-event-lib.ruleSubmodule { inherit lib; }));
      default = [ ];
      description = ''
        Structured rules; rendered into the KDL config that focus-event-trigger
        reads. Each rule fires its spawn actions when a window matching all
        the given selectors gains (trigger="focus") or loses (trigger="blur")
        keyboard focus.
      '';
      example = lib.literalExpression ''
        [
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
      '';
    };

    extraConfig = lib.mkOption {
      type = lib.types.lines;
      default = "";
      description = "Extra raw KDL appended after the generated rules.";
    };
  };

  config = lib.mkIf cfg.enable {
    environment.systemPackages = [
      pkg
      triggerWrapper
    ];

    systemd.services.focus-event = {
      description = "focus-event executor: privileged spawn-on-demand service";
      wantedBy = [ "multi-user.target" ];

      # /run/focus-event/ is created and torn down by systemd.
      serviceConfig = {
        Type = "simple";
        RuntimeDirectory = "focus-event";
        RuntimeDirectoryMode = "0755";
        ExecStart = lib.concatStringsSep " " [
          "${pkg}/bin/focus-event-executor"
          "--socket ${socketPath}"
          "--mode 0666"
          "--expected-trigger ${expectedTrigger}"
        ];
        Restart = "on-failure";
        RestartSec = "5s";
        # No rate limit: we want to keep retrying forever across reboots /
        # upgrades without systemd giving up.
        StartLimitIntervalSec = 0;
      };
    };
  };
}
