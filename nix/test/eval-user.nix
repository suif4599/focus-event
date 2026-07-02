# Test harness: import the user module into a minimal NixOS config and
# verify the systemd user unit + systemd.packages evals without error.
{ system ? builtins.currentSystem }:

let
  flake = builtins.getFlake (toString ./../..);
  nixpkgsSrc = flake.inputs.nixpkgs;
in
nixpkgsSrc.lib.nixosSystem {
  inherit system;
  modules = [
    "${nixpkgsSrc}/nixos/modules/virtualisation/qemu-vm.nix"
    flake.nixosModules.user
    {
      users.users.alice = { isNormalUser = true; password = "x"; };

      services.focus-event-user = {
        enable = true;
        rules = [
          { trigger = "focus"; app-id = "codium";
            spawn = [ [ "pactl" "set-sink-mute" "@DEFAULT_SINK@" "1" ] ]; }
        ];
      };

      virtualisation.diskSize = 512;
      documentation.enable = false;
    }
  ];
}
