{
  description = "focus-event: react to niri window focus changes by spawning commands";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      # The systems we ship packages and dev shells for. Both modules work on
      # any linux; we only build the binary for the two common ones.
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f system);

      nixpkgsFor = system: nixpkgs.legacyPackages.${system};

      # Build the bare focus-event package against the given pkgs.
      mkPackage = pkgs: pkgs.stdenv.mkDerivation {
        pname = "focus-event";
        version = "0.1.0";

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

        postInstall = ''
          install -Dm644 ${./config.example.kdl} $out/share/focus-event/config.example.kdl
        '';

        cmakeFlags = [ "-DCMAKE_BUILD_TYPE=Release" ];

        meta = with pkgs.lib; {
          description = "React to niri window focus changes by spawning commands";
          mainProgram = "focus-event";
          platforms = platforms.linux;
          license = licenses.mit;
        };
      };

      # Overlay injects `pkgs.focus-event` so the NixOS modules can resolve the
      # package without specialArgs.
      overlay = final: prev: {
        focus-event = mkPackage prev;
      };

    in
    {
      # Pure Nix KDL renderer, exposed for downstream consumers.
      lib = import ./nix/lib.nix { lib = nixpkgs.lib; };

      overlays.default = overlay;

      # Bare binary packages, one per system.
      packages = forAllSystems (system:
        let pkgs = nixpkgsFor system;
        in { default = mkPackage pkgs; });

      # Two NixOS modules. Each injects the overlay so pkgs.focus-event resolves.
      nixosModules = {
        # System-level service: runs as root, talks to a specific user's niri
        # via runtime uid lookup + NIRI_SOCKET globbing.
        system = { ... }: {
          nixpkgs.overlays = [ overlay ];
          imports = [ ./nix/modules/system.nix ];
        };

        # User-level service: installed under /etc/systemd/user/, user enables
        # it via `systemctl --user`. Inherits the user's environment.
        user = { ... }: {
          nixpkgs.overlays = [ overlay ];
          imports = [ ./nix/modules/user.nix ];
        };
      };

      devShells = forAllSystems (system:
        let pkgs = nixpkgsFor system;
        in {
          default = pkgs.mkShell {
            packages = with pkgs; [ cmake ninja gcc gdb niri ];
          };
        });
    };
}
