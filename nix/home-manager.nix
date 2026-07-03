{ config, lib, pkgs, ... }:

let
  cfg = config.services.focus-event;

  focusEventBin = "/run/current-system/sw/bin/focus-event";

  startScript = pkgs.writeShellScript "focus-event-start" ''
    set -euo pipefail

    if [ -n "''${NIRI_SOCKET:-}" ] && [ -e "$NIRI_SOCKET" ]; then
      sleep 2
      exec ${focusEventBin}
    fi

    for i in $(seq 1 100); do
      SOCKET=$(ls -t "$XDG_RUNTIME_DIR"/niri.wayland-1.*.sock 2>/dev/null | head -1 || true)
      if [ -n "$SOCKET" ] && [ -e "$SOCKET" ]; then
        export NIRI_SOCKET="$SOCKET"
        sleep 2
        exec ${focusEventBin}
      fi
      sleep 0.1
    done

    echo "focus-event: cannot find niri socket after 10s" >&2
    exit 1
  '';
in
{
  options.services.focus-event = {
    enable = lib.mkEnableOption "focus-event user service (niri socket discovery + trigger)";

    niriSpawnAtStartup = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = ''
        Whether to add `focus-event` to niri's `spawn-at-startup` config.
        Disable this if you prefer the systemd user service approach only.
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    systemd.user.services.focus-event = {
      Unit = {
        Description = "focus-event trigger (niri focus event handler)";
        PartOf = [ "graphical-session.target" ];
        After = [ "graphical-session.target" ];
      };

      Service = {
        ExecStart = startScript;
        Restart = "on-failure";
        RestartSec = "2s";
        StandardOutput = "journal";
        StandardError = "journal";
      };

      Install = {
        WantedBy = [ "graphical-session.target" ];
      };
    };
  };
}