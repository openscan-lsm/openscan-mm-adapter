Micro-Manager device adapter for OpenScan
=========================================

This is a Micro-Manager device adapter that presents an OpenScan-based laser
scanning microscope to Micro-Manager as a camera.


Building
--------

This device adapter is currently Windows-only and requires Visual Studio (2019)
to build.

This project depends on Micro-Manager's
[mmCoreAndDevices](https://github.com/micro-manager/mmCoreAndDevices) and
OpenScanLib. Clone these two repositories and place them under the same parent
directory as this repository. Headers and build products from those projects
are referenced using relative paths.

OpenScanLib (in particular, OpenScanDeviceLib) should be built using the same
Platform and Configuration (and compiler version).

Of the projects in mmCoreAndDevices, only `MMDevice-SharedRuntime` is needed.
First set its Platform Toolset (in Configuration Properties > General) to
"Visual Studio 2019 (v142)". Make sure to do this with "All Configurations" and
"All Platforms" selected in the Property Pages. Then build
`MMDevice-SharedRuntime` for the desired platforms and configurations (I
usually build for x64 Debug and x64 Release). Configuration and compiler
version must match with openscan-mm-adapter because MMDevice is a static
library.

This version assumes that the Micro-Manager device interface is at
`../mmCoreAndDevices/MMDevice`.

Finally, build openscan-mm-adapter. The build should produce
`mmgr_dal_OpenScan.dll`.


Historical
----------

This code (including the git history prior to this README being added) was
extracted from the LOCI internal 'mm-openscan' repository.
