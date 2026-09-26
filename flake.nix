{
  description = "Cross-compilation devshell targeting Windows (x86_64-w64-mingw32, UCRT) for FreeRDP";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
  };

  outputs =
    { self, nixpkgs }:
    let
      forAllSystems =
        f:
        nixpkgs.lib.genAttrs [ "x86_64-linux" "aarch64-linux" ] (
          system: f nixpkgs.legacyPackages.${system}
        );

      # build-freerdp.sh does `source mingw-env x86_64-w64-mingw32`, which is
      # an Arch mingw-w64-environment helper. Here the cross wrappers are
      # already on PATH; all the shim has to do is point the usual variables
      # at them.
      mkMingwEnv =
        pkgs:
        let
          prefix = pkgs.pkgsCross.ucrt64.stdenv.cc.targetPrefix;
        in
        pkgs.writeTextFile {
          name = "mingw-env";
          destination = "/bin/mingw-env";
          executable = true;
          text = ''
            export CC=${prefix}cc
            export CXX=${prefix}c++
            export AR=${prefix}ar
            export RANLIB=${prefix}ranlib
            export RC=${prefix}windres
          '';
        };
    in
    {
      # The Windows client, sdl-freerdp.exe with SDL3.dll and SDL3_ttf.dll next
      # to it, as a package. Built by build-freerdp.sh itself, so the cmake
      # flags and the dependency pins stay in one place: the SDL3, SDL3_ttf and
      # OpenSSL archives it would download are fetched here instead, at the
      # versions and hashes read out of the script, and unpacked where it looks
      # for them.
      packages = forAllSystems (
        pkgs:
        let
          pkgsCross = pkgs.pkgsCross.ucrt64;
          script = builtins.readFile ./build-freerdp.sh;
          pin = name: builtins.head (builtins.match ".*\n${name}=([^\n]*)\n.*" script);

          sdl3 = pkgs.fetchurl {
            url = "https://github.com/libsdl-org/SDL/releases/download/release-${pin "SDL3_VERSION"}/SDL3-devel-${pin "SDL3_VERSION"}-mingw.tar.gz";
            sha256 = pin "SDL3_SHA256";
          };
          sdl3-ttf = pkgs.fetchurl {
            url = "https://github.com/libsdl-org/SDL_ttf/releases/download/release-${pin "SDL3_TTF_VERSION"}/SDL3_ttf-devel-${pin "SDL3_TTF_VERSION"}-mingw.tar.gz";
            sha256 = pin "SDL3_TTF_SHA256";
          };
          openssl = pkgs.fetchurl {
            url = "https://mirror.msys2.org/mingw/ucrt64/mingw-w64-ucrt-x86_64-openssl-${pin "OPENSSL_VERSION"}-any.pkg.tar.zst";
            sha256 = pin "OPENSSL_SHA256";
          };
        in
        rec {
          sdl-freerdp = pkgsCross.stdenv.mkDerivation {
            pname = "sdl-freerdp-windows";
            version = "3-weaselway";
            src = self;

            nativeBuildInputs = [
              (mkMingwEnv pkgs)
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
              pkgs.zstd
            ];

            # build-freerdp.sh runs cmake itself.
            dontUseCmakeConfigure = true;
            dontUseNinjaBuild = true;
            dontUseNinjaInstall = true;
            dontStrip = true;

            buildPhase = ''
              runHook preBuild

              mkdir -p build/openssl-ucrt64-${pin "OPENSSL_VERSION"}
              tar xf ${sdl3} -C build
              tar xf ${sdl3-ttf} -C build
              tar xf ${openssl} -C build/openssl-ucrt64-${pin "OPENSSL_VERSION"}

              bash ./build-freerdp.sh

              runHook postBuild
            '';

            installPhase = ''
              runHook preInstall
              install -Dm644 -t $out/bin build/sdl-freerdp.exe build/SDL3.dll build/SDL3_ttf.dll
              runHook postInstall
            '';
          };
          default = sdl-freerdp;
        }
      );

      devShells = forAllSystems (
        pkgs:
        let
          # Everything in here is built *for* Windows, *on* the build system.
          # UCRT rather than msvcrt: build-freerdp.sh links MSYS2's ucrt64
          # OpenSSL, and the Arch toolchain in the Dockerfile defaults to UCRT.
          pkgsCross = pkgs.pkgsCross.ucrt64;
          prefix = pkgsCross.stdenv.cc.targetPrefix;

          mingw-env = mkMingwEnv pkgs;
        in
        {
          default = pkgsCross.mkShell {
            # Tools that run on the build platform. Taking them from `pkgs`
            # rather than `pkgsCross` keeps them native.
            nativeBuildInputs = [
              mingw-env
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
              pkgs.file
              pkgs.git

              # build-freerdp.sh fetches the SDL3/SDL3_ttf mingw tarballs and
              # the MSYS2 OpenSSL package itself.
              pkgs.curl
              pkgs.cacert
              pkgs.gnutar
              pkgs.gzip
              pkgs.zstd
            ]
            # wine only exists for x86 hosts.
            ++ pkgs.lib.optional pkgs.stdenv.hostPlatform.isx86_64 pkgs.wine64;

            shellHook = ''
              echo "cross toolchain: $(${prefix}cc --version | head -n1)"
              echo "build with:      ./build-freerdp.sh"
            '';
          };
        }
      );
    };
}
