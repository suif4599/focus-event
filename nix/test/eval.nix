# Test harness: import the focus-event module into a minimal NixOS config and
# verify both the systemd service and the wrapper eval without error.
{ system ? builtins.currentSystem }:

let
  flake = builtins.getFlake (toString ./../..);
  nixpkgsSrc = flake.inputs.nixpkgs;
in
nixpkgsSrc.lib.nixosSystem {
  inherit system;
  modules = [
    "${nixpkgsSrc}/nixos/modules/virtualisation/qemu-vm.nix"
    flake.nixosModules.default
    {
      users.users.alice = { isNormalUser = true; password = "x"; };

      services.focus-event = {
        enable = true;
        allowedUsers = [ "alice" ];
        rules = [
          { trigger = "focus"; app-id = "codium"; title = ".*focus-event.*";
            spawn = [ [ "pactl" "set-sink-mute" "@DEFAULT_SINK@" "1" ] ]; }
          { trigger = "blur"; app-id = "codium";
            spawn = [ [ "pactl" "set-sink-mute" "@DEFAULT_SINK@" "0" ] ]; }
        ];
      };

      virtualisation.diskSize = 512;
      documentation.enable = false;
    }
  ];
}
