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
    in
    {
      devShells = forAllSystems (
        pkgs:
        let
          # Everything in here is built *for* Windows, *on* the build system.
          # UCRT rather than msvcrt: build-freerdp.sh links MSYS2's ucrt64
          # OpenSSL, and the Arch toolchain in the Dockerfile defaults to UCRT.
          pkgsCross = pkgs.pkgsCross.ucrt64;
          prefix = pkgsCross.stdenv.cc.targetPrefix;

          # build-freerdp.sh does `source mingw-env x86_64-w64-mingw32`, which is
          # an Arch mingw-w64-environment helper. Here the cross wrappers are
          # already on PATH; all the shim has to do is point the usual
          # variables at them.
          mingw-env = pkgs.writeTextFile {
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
