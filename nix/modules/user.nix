# NixOS module: focus-event as a USER-level systemd service.
#
# The unit is installed system-wide under /etc/systemd/user/ (via
# systemd.packages) so any user on the machine can enable it with
# `systemctl --user enable --now focus-event`. Once running, it inherits the
# user's environment — NIRI_SOCKET, XDG_RUNTIME_DIR, DBUS_SESSION_BUS_ADDRESS,
# WAYLAND_DISPLAY — so no uid lookup or socket globbing is required on this
# path.
#
# This module expects pkgs.focus-event to exist (the flake wires that up via an
# overlay in nixosModules.user).

{ config, lib, pkgs, ... }:

let
  focus-event-lib = import ../lib.nix { inherit lib; };
  cfg = config.services.focus-event-user;

  kdlConfig = pkgs.writeText "focus-event-user.kdl"
    (focus-event-lib.renderConfig cfg);

  # Package whose sole purpose is to carry the systemd user unit. We deliver it
  # via systemd.packages so NixOS installs it under the system-wide user unit
  # search path automatically.
  userUnitPkg = pkgs.writeTextFile {
    name = "focus-event-user-unit";
    destination = "/lib/systemd/user/focus-event.service";
    text = ''
      [Unit]
      Description=focus-event: react to niri window focus changes
      After=graphical-session.target
      PartOf=graphical-session.target

      [Service]
      Type=simple
      ExecStart=${pkgs.focus-event}/bin/focus-event --config ${kdlConfig}
      Restart=on-failure
      RestartSec=5s

      [Install]
      WantedBy=default.target
    '';
  };
in
{
  options.services.focus-event-user = {
    enable = lib.mkEnableOption "focus-event as a user-level service";

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
    systemd.packages = [ userUnitPkg ];
  };
}
