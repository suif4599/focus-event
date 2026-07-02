# NixOS module: focus-event as a SYSTEM-level systemd service.
#
# The service runs as root but talks to a specific user's niri IPC socket by
# resolving the user's uid at start time and pointing NIRI_SOCKET at
# /run/user/<uid>/niri.wayland-*.sock. This is useful when you want focus-event
# to start at boot before any graphical login, or to centralize it outside any
# particular user session.
#
# The username is REQUIRED — there is no sensible default. The uid is resolved
# at runtime (not at nixos-rebuild time) so this works even when NixOS assigns
# uids lazily.
#
# This module expects pkgs.focus-event to exist (the flake wires that up via an
# overlay in nixosModules.system).

{ config, lib, pkgs, ... }:

let
  focus-event-lib = import ../lib.nix { inherit lib; };
  cfg = config.services.focus-event-system;

  launcher = pkgs.writeShellScriptBin "focus-event-system-launch"
    (builtins.readFile ../wrapper-system.sh);

  kdlConfig = pkgs.writeText "focus-event-system.kdl"
    (focus-event-lib.renderConfig cfg);
in
{
  options.services.focus-event-system = {
    enable = lib.mkEnableOption "focus-event as a system-level service";

    user = lib.mkOption {
      type = lib.types.str;
      description = ''
        Username whose niri session the daemon should talk to. The uid is
        resolved at service start time via `id -u <user>`, so the user account
        does not need to exist at nixos-rebuild time.
      '';
      example = "alice";
    };

    rules = lib.mkOption {
      type = lib.types.listOf (lib.types.submodule (focus-event-lib.ruleSubmodule { inherit lib; }));
      default = [ ];
      description = ''
        Rules to render into the KDL config file. Each rule fires one or more
        spawn actions when a window matching all the given selectors gains or
        loses keyboard focus.
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
      description = "Extra raw KDL to append after the generated rules.";
    };
  };

  config = lib.mkIf cfg.enable {
    systemd.services.focus-event = {
      description = "focus-event: react to niri window focus changes (system-level, talks to a user's niri)";
      after = [ "graphical.target" ];
      wants = [ "graphical.target" ];
      wantedBy = [ "multi-user.target" ];

      # The unit may come up before the user has logged in or before niri has
      # created its socket. Restart gently so we attach as soon as possible.
      serviceConfig = {
        Type = "simple";
        ExecStart = "${launcher}/bin/focus-event-system-launch ${lib.escapeShellArg cfg.user} ${pkgs.focus-event}/bin/focus-event --config ${kdlConfig}";
        Restart = "on-failure";
        RestartSec = "5s";
      };
    };
  };
}
