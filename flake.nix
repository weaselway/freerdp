{
  description = "Cross-compilation devshell targeting Windows (x86_64-w64-mingw32) for FreeRDP";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs { inherit system; };

        # everything in here is built *for* Windows, *on* ${system}
        pkgsCross = pkgs.pkgsCross.mingwW64;
      in
      {
        devShells.default = pkgsCross.mkShell {
          # tools that run on the build platform, but target the host platform.
          # taking them as arguments (rather than from `pkgs`) lets splicing pick
          # the x86_64-w64-mingw32-prefixed wrappers.
          nativeBuildInputs = [
            pkgs.pkg-config
            pkgs.file
            pkgs.cmake
            pkgs.ninja
            pkgs.wine64

            # build-freerdp.sh fetches the SDL mingw tarballs itself; nixpkgs'
            # cross SDL3 is not wired up here (see the commented-out entries
            # below), so curl/tar and a CA bundle have to be in the shell.
            pkgs.curl
            pkgs.cacert
            pkgs.gnutar
            pkgs.gzip
          ];

          # libraries built for Windows
          buildInputs = [
            pkgsCross.zlib
            pkgsCross.windows.mcfgthreads
            pkgsCross.windows.pthreads
            pkgsCross.windows.mingw_w64
            # pkgsCross.sdl3.dev
            # pkgsCross.sdl3-ttf
            pkgsCross.openssl
            pkgsCross.icu
          ];

          shellHook = ''
            echo "cross toolchain: $(${pkgsCross.stdenv.cc.targetPrefix}cc --version | head -n1)"
            echo "build with:      ./build-freerdp.sh"
            echo "run binaries:    wine64 ./your.exe"
          '';
        };
      }
    );
}
