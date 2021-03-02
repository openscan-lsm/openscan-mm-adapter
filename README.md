Micro-Manager device adapter for OpenScan
=========================================

This is a Micro-Manager device adapter that presents an OpenScan-based laser
scanning microscope to Micro-Manager as a camera.


Building
--------

This device adapter is currently Windows-only and requires Visual Studio (2015
or later) to build.

Place the OpenScanLib and micro-manager repos at the same directory level as
this repository (openscan-mm-adapter). Headers and build products from those
projects are referenced using relative paths.

OpenScanLib should be built using the same Platform and Configuration.

Micro-Manager's `MMDevice-SharedRuntime` project should be built using the same
Platform, and Configuration, and compiler version (this may require temporarily
switching the Platform Toolset of the `MMDevice-SharedRuntime` project).
Compiler versions must match because MMDevice is a static library.

This version assumes that the Micro-Manager device interface is at
`../micro-manager/MMDevice`.

Finally, build openscan-mm-adapter. The build should produce
`mmgr_dal_OpenScan.dll`.


Historical
----------

This code (including the git history prior to this README being added) was
extracted from the LOCI internal 'mm-openscan' repository.
