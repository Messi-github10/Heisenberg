# Shaderc license and version provenance

Scope: license texts, embedded notices and passive version evidence only. This new licenses directory is the only write target for this component. No binaries, public headers, CMake or active manifests are changed; no build, installation or commit is performed. Original upstream terms take precedence over these summaries.

## SDK identity is not shaderc version

Local SDK root: `D:/Environment/VulkanSDK` (`VULKAN_SDK`). Its `components.xml` identifies **Vulkan SDK 1.4.357.0** and installed core/debug packages. Those package Version values are SDK package versions, not shaderc/glslang/SPIR-V source versions. `HKLM/SOFTWARE/LunarG/VulkanSDK` records core/debug selected but has no source commit mapping. Local `Licenses/LICENSE.txt` explicitly says the SDK itself is not licensed as one work and directs users to the component registry. Both local files are copied under `evidence/`.

Official pages queried:

- Version-specific licensing registry: https://vulkan.lunarg.com/license/release/1.4.357.0.dhtml
- Windows component licenses: https://vulkan.lunarg.com/doc/view/1.4.357.0/windows/licenses.html
- SDK release/component list: https://vulkan.lunarg.com/doc/view/1.4.357.0/windows/release_notes.html

The `/doc/view/` endpoint returns the actual document; `/doc/sdk/` alone returns the application shell. The registry's shaderc license link uses mutable `main`; the files here instead use actual fixed source commits resolved below.

## Fixed source chain

1. SDK release notes name `google/shaderc@ef2c68b4871a3c399a0808321b51379847a54673`. That is an assembly/known-good commit containing only manifests/update scripts, not the actual libshaderc source. Requesting its root LICENSE returns 404.
2. Its fixed `known_good.json`, retained as `evidence/sdk-shaderc-known_good.json.txt`, selects the actual source commits below. Its alternate `known_good_khr.json` references a different/internal source set; it is not selected here because no evidence says this public SDK uses it.
3. Actual shaderc source commit `2c8cae778eec0283b44acbe7ed1a386865d78799` has CHANGES whose newest heading is `v2026.3 2026-07-15`. This is release-line context, not an assertion this commit is exactly a release tag or that SDK version equals shaderc version.
4. That shaderc commit's DEPS and selected glslang known_good.json independently corroborate the shaderc assembly dependency revisions.

| Component | Source commit selected by shaderc assembly | Available version evidence |
| --- | --- | --- |
| shaderc | 2c8cae778eec0283b44acbe7ed1a386865d78799 | CHANGES latest heading v2026.3; all five shipped public headers match this source commit byte-for-byte |
| glslang | 168d452a4f460d24b588fed08477a81c44ee27a1 | SDK tag vulkan-sdk-1.4.357.0 resolves here; local config metadata says 16.4.0 |
| SPIRV-Tools in shaderc source set | b707790a898e44038547df54580022fc1cf89c3d | Selected by shaderc and glslang assembly manifests; CHANGES latest heading v2026.3 |
| SPIRV-Headers | 29981f65241605e08b0ede4cfeb999fe3b723c6a | SDK tag vulkan-sdk-1.4.357.0 resolves here; local CMake metadata says 1.5.5, which is not a source commit |

Evidence links:

- https://raw.githubusercontent.com/google/shaderc/ef2c68b4871a3c399a0808321b51379847a54673/known_good.json
- https://raw.githubusercontent.com/google/shaderc/2c8cae778eec0283b44acbe7ed1a386865d78799/DEPS
- https://raw.githubusercontent.com/google/shaderc/2c8cae778eec0283b44acbe7ed1a386865d78799/CHANGES
- https://raw.githubusercontent.com/KhronosGroup/glslang/168d452a4f460d24b588fed08477a81c44ee27a1/known_good.json
- https://raw.githubusercontent.com/KhronosGroup/SPIRV-Tools/b707790a898e44038547df54580022fc1cf89c3d/CHANGES
- https://github.com/KhronosGroup/glslang/tree/vulkan-sdk-1.4.357.0
- https://github.com/KhronosGroup/SPIRV-Headers/tree/vulkan-sdk-1.4.357.0
- Local `D:/Environment/VulkanSDK/Lib/cmake/glslang/glslang/glslang-config-version.cmake`: `set(PACKAGE_VERSION "16.4.0")`.
- Local `D:/Environment/VulkanSDK/Lib/cmake/SPIRV-Headers/SPIRV-HeadersConfigVersion.cmake`: `set(PACKAGE_VERSION "1.5.5")`.

## Unresolved standalone-SPIRV-Tools / shaderc-assembly distinction

The SDK release list names standalone SPIRV-Tools tag `vulkan-sdk-1.4.357.0`, which resolves to **9a49b0883b9b635689a85b5647dbfcb223268151**, while the official shaderc/glslang assembly manifests select **b707790a898e44038547df54580022fc1cf89c3d**. Evidence: https://github.com/KhronosGroup/SPIRV-Tools/tree/vulkan-sdk-1.4.357.0 . Both are official selectors but they differ. These are not silently collapsed into one claim about the exact object code linked into the DLL.

The assembly-selected full LICENSE is retained in `SPIRV-Tools/LICENSE`; the standalone SDK-tag LICENSE is separately retained in `evidence/SPIRV-Tools-standalone-SDK-LICENSE.txt`. They are byte-identical full Apache-2.0 texts. Exact compiled-in SPIRV-Tools revision still needs SDK build records for proof stronger than these official manifests and the local SDK file identity checks below.

## Complete license and third-party notices

- `shaderc/LICENSE` is full Apache-2.0. All five original public header copyright/Apache comments are separately preserved; the public headers are left unchanged.
- Both actual shaderc source `third_party/LICENSE.glslang` and `third_party/LICENSE.spirv-tools` are copied in full, including historical upstream notices. They do not substitute for current selected dependency license texts.
- The entire glslang root LICENSE.txt is retained (54,615 bytes), not only its BSD opening. It contains BSD/MIT/Apache, NVIDIA preprocessing terms, and GPL parser-skeleton terms plus the Bison exception. `LICENSES/AML-glslang.txt` separately preserves full NVIDIA terms; `REUSE.toml.txt` preserves the upstream per-file attribution mapping. In particular, that mapping identifies generated `glslang_tab.cpp` / `.h` as `GPL-3.0-or-later WITH Bison-exception-2.2`. This is retention of actual dependency terms, not a claim that the whole SDK/application is GPL. No unrelated FFmpeg/GPL repository audit is implied.
- SPIRV-Headers full combined LICENSE retains Khronos header/XML wording, JsonCpp public-domain/MIT terms and CC-BY-4.0 documentation terms, rather than flattening them to a single shorthand license.
- SPIRV-Tools DEPS pins optional `microsoft/mimalloc@fef6b0dd70f9d7fa0750b0d0b9fbb471203b94cd`. Its MIT license is retained conservatively. Actual inclusion in these SDK DLLs is **unverified**. Windows defaults depend on allocator checkout/build options; no exact build flags are available locally.
- A dependency list is not a runtime SBOM. Actual shaderc `third_party/CMakeLists.txt` treats abseil/re2/effcee/googletest as test dependencies; protobuf can be tool-only. Optional Tint/WGSL support is not evidenced by this SDK assembly manifest. No claim is made that all source DEPS entries are shipped in shaderc_shared.

## Local file identity and limits

Read-only byte comparisons verified that all nine currently vendored shaderc files (five public headers, release/debug DLLs and import libraries) equal their local SDK counterparts. All five headers were additionally compared with fixed upstream raw URLs under `google/shaderc@2c8cae778eec0283b44acbe7ed1a386865d78799/libshaderc/include/shaderc/` and match exactly. No library was loaded or built for these comparisons. The table appended below records exact file SHA-256; file digests are not source commits.

## NOTICE checks

Fixed-ref root NOTICE requests returned HTTP 404 for actual shaderc source, selected glslang, selected SPIRV-Tools, selected SPIRV-Headers and optional mimalloc. The checked URLs are the same fixed raw repository/commit directories as their license source URLs below with filename `NOTICE`. No NOTICE was invented. Actual embedded and bundled third-party attribution files are retained instead.

## Honest closure status

Requested full license texts and fixed public source/version evidence are present. The actual shaderc source has been resolved through the SDK assembly manifest and the vendored files verified against the SDK. Exact binary reconstruction, exact compiled-in SPIRV-Tools revision across the documented official-selector mismatch, and optional allocator/build flags are not proven. No guessed hashes, SDK-to-shaderc semantic-version substitution, complete-SBOM claim or legal compliance certification is made.

## File-by-file sources

Paths below are relative to this licenses directory. `.json.txt`, `.xml.txt` and `.toml.txt` files are passive evidence/attribution copies, not active dependency manifests or build configuration. `SOURCE.md` itself is the author-created provenance record based on the evidence above.

### `shaderc/LICENSE`

- Source URL: https://raw.githubusercontent.com/google/shaderc/2c8cae778eec0283b44acbe7ed1a386865d78799/LICENSE
- Commit: `2c8cae778eec0283b44acbe7ed1a386865d78799`
- License / scope: Full Apache-2.0.
- Bytes: 11357; SHA-256: `c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4`

### `shaderc/third_party/LICENSE.glslang`

- Source URL: https://raw.githubusercontent.com/google/shaderc/2c8cae778eec0283b44acbe7ed1a386865d78799/third_party/LICENSE.glslang
- Commit: `2c8cae778eec0283b44acbe7ed1a386865d78799`
- License / scope: Complete bundled historical third-party notices.
- Bytes: 8280; SHA-256: `b5a00e94f058edc87e05978329b55730d8689abe61205d9018443d03de4f07da`

### `shaderc/third_party/LICENSE.spirv-tools`

- Source URL: https://raw.githubusercontent.com/google/shaderc/2c8cae778eec0283b44acbe7ed1a386865d78799/third_party/LICENSE.spirv-tools
- Commit: `2c8cae778eec0283b44acbe7ed1a386865d78799`
- License / scope: Complete bundled historical third-party notices.
- Bytes: 11650; SHA-256: `47e20ce182bc68fab1a0cfb129b03c326aaf4f2b6b7905aa9d376d31018bd29f`

### `glslang/LICENSE.txt`

- Source URL: https://raw.githubusercontent.com/KhronosGroup/glslang/168d452a4f460d24b588fed08477a81c44ee27a1/LICENSE.txt
- Commit: `168d452a4f460d24b588fed08477a81c44ee27a1`
- License / scope: Complete combined license, including NVIDIA and parser/Bison exception.
- Bytes: 54615; SHA-256: `17e70c676e1521ff3e4686f04a2053d93a7e28a33be8de7ec37ab0ff72feb677`

### `glslang/REUSE.toml.txt`

- Source URL: https://raw.githubusercontent.com/KhronosGroup/glslang/168d452a4f460d24b588fed08477a81c44ee27a1/REUSE.toml
- Commit: `168d452a4f460d24b588fed08477a81c44ee27a1`
- License / scope: Complete per-file copyright and license attribution evidence.
- Bytes: 16712; SHA-256: `fd3556a55016a73ff351a79a4d87600e4e67ed511299e0809dce018638ae7424`

### `glslang/LICENSES/AML-glslang.txt`

- Source URL: https://raw.githubusercontent.com/KhronosGroup/glslang/168d452a4f460d24b588fed08477a81c44ee27a1/LICENSES/AML-glslang.txt
- Commit: `168d452a4f460d24b588fed08477a81c44ee27a1`
- License / scope: Full NVIDIA preprocessing terms.
- Bytes: 2373; SHA-256: `cc8ea571129dbee2997e52a54d1f715f8b0159a22f28f985a9b2b31708a48620`

### `SPIRV-Tools/LICENSE`

- Source URL: https://raw.githubusercontent.com/KhronosGroup/SPIRV-Tools/b707790a898e44038547df54580022fc1cf89c3d/LICENSE
- Commit: `b707790a898e44038547df54580022fc1cf89c3d`
- License / scope: Full Apache-2.0 at assembly-selected commit.
- Bytes: 11358; SHA-256: `cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30`

### `SPIRV-Headers/LICENSE`

- Source URL: https://raw.githubusercontent.com/KhronosGroup/SPIRV-Headers/29981f65241605e08b0ede4cfeb999fe3b723c6a/LICENSE
- Commit: `29981f65241605e08b0ede4cfeb999fe3b723c6a`
- License / scope: Complete Khronos, XML, JsonCpp, documentation licensing text.
- Bytes: 23503; SHA-256: `ea43b1de38a6f90c488800d66dec1ed671e68cda530266bc96951fb5b6307613`

### `mimalloc/LICENSE`

- Source URL: https://raw.githubusercontent.com/microsoft/mimalloc/fef6b0dd70f9d7fa0750b0d0b9fbb471203b94cd/LICENSE
- Commit: `fef6b0dd70f9d7fa0750b0d0b9fbb471203b94cd`
- License / scope: Optional allocator full MIT notice; inclusion in SDK DLL not verified.
- Bytes: 1096; SHA-256: `82ade5d7d9b029044b5fbbc326207fc47c2d44ee4dd71e8a559c36d728217de9`

### `evidence/sdk-shaderc-known_good.json.txt`

- Source URL: https://raw.githubusercontent.com/google/shaderc/ef2c68b4871a3c399a0808321b51379847a54673/known_good.json
- Commit: `ef2c68b4871a3c399a0808321b51379847a54673`
- License / scope: Exact SDK assembly commit-selection evidence.
- Bytes: 1655; SHA-256: `b3d6f8052c3739cfe124f1a8def7b3c0344e00f77f4452ebea21a5df47735c11`

### `evidence/shaderc-DEPS.txt`

- Source URL: https://raw.githubusercontent.com/google/shaderc/2c8cae778eec0283b44acbe7ed1a386865d78799/DEPS
- Commit: `2c8cae778eec0283b44acbe7ed1a386865d78799`
- License / scope: Actual shaderc dependency revision evidence, including tests.
- Bytes: 1359; SHA-256: `f629401c15ad76ce96405ec97257db08df112f68cf1557f49f50f14bc8c0ac33`

### `evidence/glslang-known_good.json.txt`

- Source URL: https://raw.githubusercontent.com/KhronosGroup/glslang/168d452a4f460d24b588fed08477a81c44ee27a1/known_good.json
- Commit: `168d452a4f460d24b588fed08477a81c44ee27a1`
- License / scope: Independent selected SPIR-V source revision evidence.
- Bytes: 667; SHA-256: `5324c6dc4ff43e9f185a828f3c76b1626c80aef4e0707b17dfb67dc0d803c307`

### `evidence/SPIRV-Tools-DEPS.txt`

- Source URL: https://raw.githubusercontent.com/KhronosGroup/SPIRV-Tools/b707790a898e44038547df54580022fc1cf89c3d/DEPS
- Commit: `b707790a898e44038547df54580022fc1cf89c3d`
- License / scope: Selected SPIRV-Headers and optional allocator evidence.
- Bytes: 1329; SHA-256: `d5ae06c13341f45d763ae20f11b3b5202ecbea83e39a1eb67c098feb7d117737`

### `evidence/SPIRV-Tools-standalone-SDK-LICENSE.txt`

- Source URL: https://raw.githubusercontent.com/KhronosGroup/SPIRV-Tools/9a49b0883b9b635689a85b5647dbfcb223268151/LICENSE
- Commit: `9a49b0883b9b635689a85b5647dbfcb223268151`
- License / scope: Full standalone SDK-tag Apache-2.0; byte-identical to assembly-selected LICENSE.
- Bytes: 11358; SHA-256: `cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30`

### `evidence/SDK-components.xml.txt`

- Local source: `D:/Environment/VulkanSDK/components.xml`
- Version: Vulkan SDK 1.4.357.0.
- Scope: SDK installer version/package evidence; not upstream component versions.
- Bytes: 3699; SHA-256: `72d48475b34b69238d3917eca626f4360896ed1dd85f2a945dc39ad600e3b8ea`

### `evidence/SDK-LICENSE.txt`

- Local source: `D:/Environment/VulkanSDK/Licenses/LICENSE.txt`
- Version: Vulkan SDK 1.4.357.0.
- Scope: LunarG license-registry explanation, not a replacement for upstream terms.
- Bytes: 944; SHA-256: `03b9dd9957f288bc26ef39d188672d5a761f2f35c972d959b65c0f32cd4d576f`

### `shaderc/headers/env.h.LICENSE.embedded.txt`

- Local source: `D:/Code/Project/Heisenberg/Dependence/Shaderc/windows/include/shaderc/env.h` (entire opening comment, stopping before include guard).
- Upstream URL: https://raw.githubusercontent.com/google/shaderc/2c8cae778eec0283b44acbe7ed1a386865d78799/libshaderc/include/shaderc/env.h
- Commit: `2c8cae778eec0283b44acbe7ed1a386865d78799`; full original header byte-matched.
- License / scope: Existing Shaderc Authors copyright and Apache-2.0 notice; original header unchanged.
- Bytes: 619; SHA-256: `aea950f224c850bf353c6207b528407ddf27f4a1971f48be554104613f924e7c`

### `shaderc/headers/shaderc.h.LICENSE.embedded.txt`

- Local source: `D:/Code/Project/Heisenberg/Dependence/Shaderc/windows/include/shaderc/shaderc.h` (entire opening comment, stopping before include guard).
- Upstream URL: https://raw.githubusercontent.com/google/shaderc/2c8cae778eec0283b44acbe7ed1a386865d78799/libshaderc/include/shaderc/shaderc.h
- Commit: `2c8cae778eec0283b44acbe7ed1a386865d78799`; full original header byte-matched.
- License / scope: Existing Shaderc Authors copyright and Apache-2.0 notice; original header unchanged.
- Bytes: 619; SHA-256: `f1615c27810bf717756ad7f9d2122d0135480fca3c7e1faea3335a0a24152183`

### `shaderc/headers/shaderc.hpp.LICENSE.embedded.txt`

- Local source: `D:/Code/Project/Heisenberg/Dependence/Shaderc/windows/include/shaderc/shaderc.hpp` (entire opening comment, stopping before include guard).
- Upstream URL: https://raw.githubusercontent.com/google/shaderc/2c8cae778eec0283b44acbe7ed1a386865d78799/libshaderc/include/shaderc/shaderc.hpp
- Commit: `2c8cae778eec0283b44acbe7ed1a386865d78799`; full original header byte-matched.
- License / scope: Existing Shaderc Authors copyright and Apache-2.0 notice; original header unchanged.
- Bytes: 619; SHA-256: `f1615c27810bf717756ad7f9d2122d0135480fca3c7e1faea3335a0a24152183`

### `shaderc/headers/status.h.LICENSE.embedded.txt`

- Local source: `D:/Code/Project/Heisenberg/Dependence/Shaderc/windows/include/shaderc/status.h` (entire opening comment, stopping before include guard).
- Upstream URL: https://raw.githubusercontent.com/google/shaderc/2c8cae778eec0283b44acbe7ed1a386865d78799/libshaderc/include/shaderc/status.h
- Commit: `2c8cae778eec0283b44acbe7ed1a386865d78799`; full original header byte-matched.
- License / scope: Existing Shaderc Authors copyright and Apache-2.0 notice; original header unchanged.
- Bytes: 619; SHA-256: `aea950f224c850bf353c6207b528407ddf27f4a1971f48be554104613f924e7c`

### `shaderc/headers/visibility.h.LICENSE.embedded.txt`

- Local source: `D:/Code/Project/Heisenberg/Dependence/Shaderc/windows/include/shaderc/visibility.h` (entire opening comment, stopping before include guard).
- Upstream URL: https://raw.githubusercontent.com/google/shaderc/2c8cae778eec0283b44acbe7ed1a386865d78799/libshaderc/include/shaderc/visibility.h
- Commit: `2c8cae778eec0283b44acbe7ed1a386865d78799`; full original header byte-matched.
- License / scope: Existing Shaderc Authors copyright and Apache-2.0 notice; original header unchanged.
- Bytes: 619; SHA-256: `aea950f224c850bf353c6207b528407ddf27f4a1971f48be554104613f924e7c`

## Verified SDK file identities

| Vendored path relative to ../windows | SHA-256 | SDK byte-copy source | Result |
| --- | --- | --- | --- |
| `bin/shaderc_shared.dll` | `258c9ccc80c31c20904c64a80af964504bdb29c1475aa21b15eb540d5c93c860` | `D:/Environment/VulkanSDK/Bin/shaderc_shared.dll` | identical |
| `debug/bin/shaderc_sharedd.dll` | `736dace353ae19767a74918488f56b635ab5317e0e1c7c50718464454e925727` | `D:/Environment/VulkanSDK/Bin/shaderc_sharedd.dll` | identical |
| `debug/lib/shaderc_sharedd.lib` | `54b31e366e04d5f28bb616c3c98bdcf73e8cb8d3520319ef7bdd3a801d58b7da` | `D:/Environment/VulkanSDK/Lib/shaderc_sharedd.lib` | identical |
| `include/shaderc/env.h` | `645d5d8c0bbc874e42be727d8bb3cb887408b39a7335e99f33bf1f923142dac8` | `D:/Environment/VulkanSDK/Include/shaderc/env.h` | identical |
| `include/shaderc/shaderc.h` | `78a8195db6fa67b94e95014fe08f63063c36d2b581d182bae0f7cd4e86d59588` | `D:/Environment/VulkanSDK/Include/shaderc/shaderc.h` | identical |
| `include/shaderc/shaderc.hpp` | `1230c3d7e07bf431555b820ff957b46f7f3e3609ba0839d8c85ed09f809a6405` | `D:/Environment/VulkanSDK/Include/shaderc/shaderc.hpp` | identical |
| `include/shaderc/status.h` | `c379f95263f2a319de6805a68749b5bdf78ec885075bc504d6d2c1173c032941` | `D:/Environment/VulkanSDK/Include/shaderc/status.h` | identical |
| `include/shaderc/visibility.h` | `9bb1c9db00ab812ced4b22bb813a8950ecb7c9cdf7a92135db0c71f0804b3d62` | `D:/Environment/VulkanSDK/Include/shaderc/visibility.h` | identical |
| `lib/shaderc_shared.lib` | `697a51fe3193f41bf1cd71d462aca037102b88f1f1d114f36db27d58ae771ebb` | `D:/Environment/VulkanSDK/Lib/shaderc_shared.lib` | identical |
