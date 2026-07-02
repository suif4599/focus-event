# Test harness: import the system module into a minimal NixOS config and
# verify the systemd unit evals without error. Used by the test script.
{ system ? builtins.currentSystem }:

let
  flake = builtins.getFlake (toString ./../..);
  nixpkgsSrc = flake.inputs.nixpkgs;
in
nixpkgsSrc.lib.nixosSystem {
  inherit system;
  modules = [
    "${nixpkgsSrc}/nixos/modules/virtualisation/qemu-vm.nix"
    flake.nixosModules.system
    {
      users.users.alice = { isNormalUser = true; password = "x"; };

      services.focus-event-system = {
        enable = true;
        user = "alice";
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
