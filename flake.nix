{
  description = "Bloodborne PC native execution layer and launcher";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    nix-filter.url = "github:numtide/nix-filter";
  };

  outputs = { self, nixpkgs, flake-utils, nix-filter }:
    flake-utils.lib.eachSystem [ "x86_64-linux" "aarch64-linux" ] (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

        # libdecor (window frames on GNOME's Wayland) without its GTK 3 plugin (GTK 3, tinysparql, ICU:
        # ~100 MB); its cairo plugin draws the frames.
        libdecor = pkgs.libdecor.overrideAttrs (old: {
          buildInputs = pkgs.lib.remove pkgs.gtk3 old.buildInputs;
          mesonFlags = (old.mesonFlags or [ ]) ++ [ "-Dgtk=disabled" ];
        });

        # SDL3 names zenity (its message boxes) by store path, and zenity brings a whole GTK4 with
        # GStreamer into the AppImage (~200 MB). This one runs the host's zenity when there is one.
        sdl3 = pkgs.sdl3.override {
          inherit libdecor;
          zenity = pkgs.writeShellScriptBin "zenity" ''
            host=$(command -v zenity) && exec "$host" "$@"
            exit 1
          '';
        };

        pkgDef = {
          pname = "bloodborne-pc";
          version = "1.0.9";

          src = nix-filter.lib.filter {
            root = ./.;
            exclude = [
              ./.git
              ./.gitignore
              ./flake.nix
              ./flake.lock
              ./out
              ./pgo
            ];
          };

          nativeBuildInputs = with pkgs; [
            cmake
            ninja
            pkg-config
            gcc
            gnumake
            python3
            binutils
            just
            git
          ];

          buildInputs = with pkgs; [
            vulkan-headers
            vulkan-loader
            sdl3
            freetype
            # GPU library (gpu/): shadPS4 video core dependencies
            ffmpeg-headless
            boost
            fmt
            magic-enum
            robin-map
            xxhash
            vulkan-memory-allocator
            glslang
            spirv-cross
            xbyak
            zydis
            spirv-headers
            miniz
            xorg.libX11
            xorg.libXcb
            xorg.libXau
            xorg.xorgproto
            wayland
            libxkbcommon
            alsa-lib
            libGL
            fontconfig
            # Online module (gpu/bbnet): shadNet client, HTTPS, UPnP
            protobuf
            nlohmann_json
            openssl
            zlib
            miniupnpc
          ];

          runtimeDependencies = with pkgs; [
            vulkan-loader
            wayland
            libxkbcommon
            alsa-lib
            sdl3
            libGL
            openssl
            xorg.libX11
            xorg.libXcb
          ];

          meta = with pkgs.lib; {
            description = "Bloodborne PC native execution layer and launcher";
            license = licenses.mit;
            platforms = [ "x86_64-linux" "aarch64-linux" ];
            mainProgram = "bb-launcher";
          };
        };

        bloodborne-pc = pkgs.stdenv.mkDerivation (pkgDef // {
          passthru = {
            inherit (pkgDef) runtimeDependencies;
          };

          buildPhase = ''
            runHook preBuild
            bash ./scripts/build.sh
            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall
            mkdir -p $out/bin $out/lib
            install -m755 out/bb-probe $out/bin/bb-probe
            install -m755 out/bb-launcher $out/bin/bb-launcher
            install -m755 out/bb-runner $out/bin/bb-runner
            install -m755 out/bb-gpu-capabilities $out/bin/bb-gpu-capabilities
            if [ -d out/gpu ]; then
              mkdir -p $out/bin/gpu
              cp -r out/gpu/* $out/bin/gpu/
            fi
            runHook postInstall
          '';
        });
      in {
        checks = {
          inherit bloodborne-pc;
        };

        packages.default = bloodborne-pc;

        apps.default = flake-utils.lib.mkApp {
          drv = bloodborne-pc;
        };

        devShells.default = pkgs.mkShell rec {
          inputsFrom = builtins.attrValues self.checks.${system};
          LD_LIBRARY_PATH = pkgs.lib.strings.makeLibraryPath (builtins.concatMap (d: d.runtimeDependencies or [ ]) inputsFrom);
        };
      });
}
