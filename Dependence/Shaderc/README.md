# shaderc shared Windows SDK

This directory vendors the x64 shared shader compiler from Vulkan SDK **1.4.357.0**.
The SDK version is not the shaderc semantic version. Its official assembly manifest
selects shaderc source commit `2c8cae778eec0283b44acbe7ed1a386865d78799`; the five public
headers match that commit. See `licenses/SOURCE.md` for exact source evidence and
limits on reconstructing the DLL's embedded dependency revisions.

```
windows/include/shaderc/       public API headers
windows/lib/                  shaderc_shared.lib
windows/bin/                  shaderc_shared.dll
windows/debug/lib/            shaderc_sharedd.lib
windows/debug/bin/            shaderc_sharedd.dll
licenses/                     full license texts, notices and version evidence
manifest.json                 binary/source identity and SHA-256 inventory
```

The target `heisenberg_shaderc` (alias `shaderc::shaderc_shared`) provides the
configuration-specific import library/runtime and `SHADERC_SHAREDLIB`. It does not
search the installed Vulkan SDK. glslang and SPIR-V compiler components are embedded
in these DLLs; no separate glslang/SPIRV-Tools runtime DLLs are required. Matching
MSVC Release/Debug CRT rules apply.

The application still needs Vulkan SDK **build tools** (`glslangValidator`,
`spirv-val`, `spirv-cross`) to compile and validate its static shaders. Those tools
are distinct from this vendored runtime compiler dependency.

DLL/LIB files use the existing Git LFS rules. Update the SDK as one matched
Debug/Release snapshot and regenerate the manifest after updating notices.
