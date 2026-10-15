# OpenColorIO Windows SDK

This directory vendors OpenColorIO **2.5.2** for MSVC x64, with separate Release
(`/MD`) and Debug (`/MDd`) runtimes. It is consumed through the repository-owned
`OpenColorIO::OpenColorIO` CMake target; configuration and deployment never search
an external vcpkg installation.

```
windows/include/OpenColorIO/   public SDK headers
windows/lib/                  Release import library
windows/bin/                  Release DLL and its seven runtime dependencies
windows/debug/lib/            Debug import library
windows/debug/bin/            Debug DLLs (same names can have different contents)
licenses/                     component notices, ACES notices and source evidence
manifest.json                 source/version identity and file SHA-256 inventory
```

The binary source is the x64-windows shared package produced by vcpkg. vcpkg is a
provenance/build tool for this snapshot, not an application build prerequisite.
Runtime dependencies are expat, Imath, yaml-cpp, zlib, bzip2, liblzma and zstd.
Static embedded components include pystring, minizip-ng, xxHash and SampleICC; their
notices are also retained. See `licenses/SOURCE.md` for license/source details.

The Studio ACES configuration is embedded in the library:
`studio-config-v4.0.0_aces-v2.0_ocio-v2.5`. No separate `.ocio` download is needed.

Binary files use the existing `Dependence/**` Git LFS rules. After cloning, run
`git lfs pull` before building. A matching official MSVC Redistributable is needed
for Release distribution; Debug requires the developer Debug CRT and is not a
redistributable product package. Do not mix Debug and Release DLLs.

Updating this SDK means replacing a complete matching snapshot, updating the
manifest/notices, then verifying hashes, both configurations and isolated runtime
deployment. Do not substitute only the main DLL or rewrite the bundled headers.
