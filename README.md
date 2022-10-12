# Micro-Manager device adapter for OpenScan

This is a Micro-Manager device adapter that presents an OpenScan-based laser
scanning microscope to Micro-Manager as a camera.

## How to build

This device adapter is currently Windows-only and requires Visual Studio (2019)
or later to build.

[Meson](https://github.com/mesonbuild/meson/releases) is used for build; make
sure `meson` and `ninja` are on the `PATH`.

Dependencies (OpenScanLib and MMDevice) are automatically fetched and built by
Meson.

It is best to build in the Developer PowerShell for VS 2019 (or later), which
can be started from the Start Menu (hint: type 'developer powershell' into the
Start Menu to search).

```pwsh
cd path\to\openscan-mm-adapter
meson setup builddir --buildtype release
meson compile -C builddir
```

The build should produce `mmgr_dal_OpenScan.dll` in `builddir`.

## Code of Conduct

[![Contributor Covenant](https://img.shields.io/badge/Contributor%20Covenant-2.0-4baaaa.svg)](https://github.com/openscan-lsm/OpenScan/blob/main/CODE_OF_CONDUCT.md)

## Historical

This code (including the git history prior to this README being added) was
extracted from the LOCI internal 'mm-openscan' repository.
