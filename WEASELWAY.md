# Building FreeRDP for weaselway (Nix)

This builds the Windows SDL3 client (`sdl-freerdp.exe`) that
`weaselway/ww-start-viewer.sh` runs. It cross-compiles with the dev shell in
[flake.nix](flake.nix) (nixpkgs `nixos-26.05`, `pkgsCross.ucrt64`). Nothing
is installed.

```sh
nix develop -c ./build-freerdp.sh
```

The same build as a package, which is what CI and the weaselway image use:

```sh
nix build .#sdl-freerdp      # result/bin/sdl-freerdp.exe, SDL3.dll, SDL3_ttf.dll
```

Output, all in `build/`:

- `sdl-freerdp.exe`: statically linked; imports only Windows system DLLs
  (UCRT `api-ms-win-crt-*`) plus SDL
- `SDL3.dll`, `SDL3_ttf.dll`: copy these next to the exe

The cmake configuration lives in [build-freerdp.sh](build-freerdp.sh) and is
the only place the flags are kept. It runs `cmake` only when
`build/freerdp/okay` is missing, so to re-apply changed flags run
`rm -rf build/freerdp`. Incremental rebuilds after that are just the script
again, or `cmake --build build/freerdp`.

## How the Nix shell fits the script

- **Toolchain:** `x86_64-w64-mingw32-gcc` 15.x targeting **UCRT**. It has to
  be UCRT and not msvcrt (`pkgsCross.mingwW64`), because the script links
  MSYS2's `ucrt64` OpenSSL. See the comment in the script.
- **`source mingw-env`:** that is an Arch helper. The shell provides a small
  `mingw-env` on `PATH` that exports `CC`/`CXX`/`AR`/`RANLIB`/`RC` for the
  cross tools.
- **Dependencies:** the script downloads SDL3, SDL3_ttf and OpenSSL itself
  into `build/`, so the shell only adds curl/tar/zstd/cacert. No nixpkgs
  Windows libraries are used.
- **`MINGW_SYSROOT`:** it doesn't exist on NixOS, so the script skips its
  `CMAKE_FIND_ROOT_PATH` confinement. The cross wrappers find the right
  libraries anyway.
- **wine:** only added on x86_64 hosts, since nixpkgs has no aarch64 wine64.

## Notes

- On aarch64 hosts the mingw gcc isn't in the binary cache, so the first
  `nix develop` compiles it (two gcc builds). It is cached in the store after
  that.
- cache.nixos.org has no mingw/UCRT cross gcc for x86_64 either, so a machine
  that only uses it compiles the toolchain once. weaselway.cachix.org has it:

  ```sh
  nix develop \
    --extra-substituters https://weaselway.cachix.org \
    --extra-trusted-public-keys weaselway.cachix.org-1:aN6jpdbl2M5QNsR3U8zx1G/R0jHIkYkvX15G9jxPiHU= \
    -c ./build-freerdp.sh
  ```

  That needs a trusted nix user; otherwise add both to `nix.conf`.

## CI

[build.yml](.github/workflows/build.yml) builds the package on pushes to
`main` and on pull requests and uploads the exe and DLLs as an artifact.
[release.yml](.github/workflows/release.yml) does the same for a tag and
attaches `freerdp-<tag>.zip` to a release, for running the client without
the weaselway image. Both use weaselway.cachix.org and push what
they build to it (token in the `CACHIX_AUTH_TOKEN` secret), so the toolchain
is not compiled on every run and the weaselway image build finds the client
there. Pull requests from forks get no secrets and only read the cache.
