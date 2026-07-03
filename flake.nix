{
  description = "focus-event: react to niri window focus changes by spawning commands";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f system);
      nixpkgsFor = system: nixpkgs.legacyPackages.${system};

      mkPackage = pkgs: pkgs.stdenv.mkDerivation {
        pname = "focus-event";
        version = "0.2.0";

        src = pkgs.lib.cleanSourceWith {
          src = ./.;
          filter = path: type:
            let base = baseNameOf path;
            in !(type == "directory" && base == "build")
            && !(type == "directory" && base == ".cache")
            && !(base == "flake.lock");
        };

        nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
        buildInputs = [ ];

        cmakeFlags = [ "-DCMAKE_BUILD_TYPE=Release" ];

        meta = with pkgs.lib; {
          description = "React to niri window focus changes by spawning commands (executor + trigger)";
          mainProgram = "focus-event-trigger";
          platforms = platforms.linux;
          license = licenses.mit;
        };
      };

      overlay = final: prev: {
        focus-event = mkPackage prev;
      };

    in
    {
      # Pure Nix KDL renderer for downstream consumers.
      lib = import ./nix/lib.nix { lib = nixpkgs.lib; };

      overlays.default = overlay;

      # The package: contains focus-event-executor and focus-event-trigger.
      # The NixOS module adds a `focus-event` wrapper that bakes in --config
      # and --socket paths so the user just types "focus-event" in niri's
      # spawn-at-startup.
      packages = forAllSystems (system:
        let pkgs = nixpkgsFor system;
        in { default = mkPackage pkgs; });

      # Single NixOS module. Sets up:
      #   - systemd.services.focus-event  (privileged executor)
      #   - environment.systemPackages    (executor + trigger binaries + wrapper)
      nixosModules.default = { ... }: {
        nixpkgs.overlays = [ overlay ];
        imports = [ ./nix/default.nix ];
      };
      nixosModules.focus-event = self.nixosModules.default;

      homeManagerModules.default = import ./nix/home-manager.nix;
      homeManagerModules.focus-event-user = self.homeManagerModules.default;

      devShells = forAllSystems (system:
        let pkgs = nixpkgsFor system;
        in {
          default = pkgs.mkShell {
            packages = with pkgs; [ cmake ninja gcc gdb niri ];
          };
        });
    };
}
