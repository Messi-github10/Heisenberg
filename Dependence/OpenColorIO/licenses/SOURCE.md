# OpenColorIO license and version provenance

Scope: the centrally vendored OpenColorIO runtime/dependency notices and requested builtinConfigACES v4.0.0 / ACES 2 terms. Only this new licenses directory is written. Original full license texts take precedence over the descriptive labels here. This is not a repository-wide license audit or a reproducible-build attestation.

## Local package versions

Version evidence: `D:/Code/Github/vcpkg/installed/vcpkg/status`, installed architecture `x64-windows`. The `#` suffix denotes a vcpkg port revision, not an upstream release. Repository/ref/license-file mappings were read from the corresponding `D:/Code/Github/vcpkg/ports/<package>/portfile.cmake` and `vcpkg.json`.

| Package | Installed version | License context |
| --- | --- | --- |
| opencolorio | 2.5.2 | BSD-3-Clause |
| expat | 2.8.2 | MIT |
| imath | 3.2.2 | BSD-3-Clause |
| yaml-cpp | 0.9.0#1 | MIT |
| pystring | 1.2.0 | BSD-3-Clause |
| minizip-ng | 4.1.0 | Zlib |
| zlib | 1.3.2#1 | Zlib |
| bzip2 | 1.0.8#6 | bzip2-1.0.6 license family |
| liblzma | 5.8.3 | 0BSD library; installed COPYING is a package licensing inventory |
| zstd | 1.5.7 | BSD-3-Clause OR GPL-2.0-only; BSD-3-Clause alternative available |

Each `<package>/copyright` is an unchanged byte copy of `D:/Code/Github/vcpkg/installed/x64-windows/share/<package>/copyright`. These installed notices describe the local packages; upstream URLs below are additional source references, not an assertion that every local vcpkg patch is absent.

OpenColorIO tag `v2.5.2` resolves to commit `c52966a6677723d5bd2dbef0ccec3fed9cbc3790`, observed at https://github.com/AcademySoftwareFoundation/OpenColorIO/tree/v2.5.2 . Local source root is `D:/Code/Github/vcpkg/buildtrees/opencolorio/src/v2.5.2-4011011b88.clean`. Its directory suffix is not claimed to be a Git commit.

## Embedded notices and supplemental liblzma terms

- `xxHash/LICENSE.embedded.txt` contains the entire leading license comment of `ext/xxHash/src/include/xxhash.h`, verbatim through its closing comment. BSD-2-Clause; copyright Yann Collet 2012-2020. Local header macros report 0.8.1; no unrelated current xxHash license is substituted.
- `sampleicc/iccProfileReader.LICENSE.embedded.txt` retains the entire comment area before the include guard, including complete ICC Software License 0.2 and OpenColorIO modification notice.
- `sampleicc/icProfileHeader.LICENSE.embedded.txt` retains the entire leading comment area before `/* Header file guard bands */`, preserving ALL ICC 2003-2006, Heidelberger Druckmaschinen 2002 and SunSoft 1994 copyright, permissions, disclaimers and name-use terms. It preserves the original bytes, including the legacy single-byte copyright symbol. The local complete source header was also verified byte-identical to the fixed OpenColorIO upstream header. No implementation header was changed or copied into an include directory.
- `liblzma/COPYING.0BSD` supplies the actual full 0BSD terms missing from the installed copyright inventory. The inventory's references to GPL/LGPL scripts, tools and build files do not change the stated license of the liblzma library.
- `zstd/copyright` preserves the installed concatenation of LICENSE and COPYING without truncation. Retaining both offered texts does not select the GPL alternative; the library can use BSD-3-Clause.

## builtinConfigACES v4.0.0 / ACES 2 fixed source chain

The local OpenColorIO `src/OpenColorIO/builtinconfigs/BuiltinConfigRegistry.cpp` identifies default `ocio://cg-config-v4.0.0_aces-v2.0_ocio-v2.5` and matching studio config; `.ocio` files under `src/OpenColorIO/builtinconfigs/configs/` identify `[COLORSPACES v4.0.0] [ACES v2.0] [OCIO v2.5]`.

1. OpenColorIO-Config-ACES tag `v4.0.0` resolves to `97a47181b52a051518163e2a7a8c29f6c7ab8da7`. Evidence: https://github.com/AcademySoftwareFoundation/OpenColorIO-Config-ACES/tree/v4.0.0 . Its full BSD-3-Clause LICENSE is retained, separately from Academy terms.
2. That tag's `opencolorio_config_aces/config/reference/aces` gitlink resolves to `ampas/aces@c2a37a08f898d28ba589a7835028c1442b95f02e`. Evidence: https://github.com/AcademySoftwareFoundation/OpenColorIO-Config-ACES/tree/v4.0.0/opencolorio_config_aces/config/reference . Repository URL is corroborated by https://raw.githubusercontent.com/AcademySoftwareFoundation/OpenColorIO-Config-ACES/97a47181b52a051518163e2a7a8c29f6c7ab8da7/.gitmodules .
3. The Academy umbrella tree fixes transform component revisions below. Evidence: https://github.com/ampas/aces/tree/c2a37a08f898d28ba589a7835028c1442b95f02e . GitHub currently resolves those component links into the `aces-aswf` organization; the fixed hashes are retained.

| Transform component | Fixed gitlink commit |
| --- | --- |
| aces-core | 2d7af39344725aaa8ac3bf1746693c9a1d6c4792 |
| aces-input-and-colorspaces | cc60becf11534f2a40df00a281f47e95412d0599 |
| aces-look | e0e70ecf2ce57d503318dde1fa4aa1bd7287fcdd |
| aces-output | aab74723f76728c37345ed01e51ebb24fb1f2f1f |

The Academy umbrella and all four transform component LICENSE.md files are included in full. Their binary notice retention, no-endorsement and no-implied-trademark/patent conditions remain intact; these terms are not relabeled BSD-3-Clause. The aces-amf submodule is not asserted to ship in these built-in configs and is outside the requested transform license bundle.

## NOTICE availability and closure limits

Root `NOTICE` requests at the fixed OpenColorIO-Config-ACES v4.0.0 tag and the exact fixed Academy umbrella/component revisions above returned HTTP 404. No NOTICE was invented. For each saved upstream LICENSE, the checked NOTICE URL is the same fixed raw directory with filename `NOTICE` instead of `LICENSE` or `LICENSE.md`.

The requested package copies, complete embedded xxHash/SampleICC attribution, supplemental 0BSD text, config-generator v4.0.0 and ACES 2 source-chain licenses are present. No independent SampleICC release is known: the source identity is the embedded snapshot in OpenColorIO v2.5.2. This task does not establish exact source reconstruction of the DLLs, audit every historical built-in ACES config, or certify legal compliance. No FFmpeg or unrelated repository license files were changed or audited.

## File-by-file sources

Paths in this section are relative to this directory. Local paths name the exact byte-copy/extraction source; fixed upstream references identify public source context. `SOURCE.md` itself is the author-created provenance record based on the evidence above.

### `opencolorio/copyright`

- Local source: `D:/Code/Github/vcpkg/installed/x64-windows/share/opencolorio/copyright`
- Upstream URL: https://raw.githubusercontent.com/AcademySoftwareFoundation/OpenColorIO/v2.5.2/LICENSE
- Version / commit: `v2.5.2`
- License / scope: BSD-3-Clause; copied bytes unchanged.
- Bytes: 1485; SHA-256: `82a40c52065ee968aa62015735e84378ac11425db7a233e6a2dcd2c83fc24276`

### `expat/copyright`

- Local source: `D:/Code/Github/vcpkg/installed/x64-windows/share/expat/copyright`
- Upstream URL: https://raw.githubusercontent.com/libexpat/libexpat/R_2_8_2/expat/COPYING
- Version / commit: `R_2_8_2`
- License / scope: MIT; copied bytes unchanged.
- Bytes: 1144; SHA-256: `31b15de82aa19a845156169a17a5488bf597e561b2c318d159ed583139b25e87`

### `imath/copyright`

- Local source: `D:/Code/Github/vcpkg/installed/x64-windows/share/imath/copyright`
- Upstream URL: https://raw.githubusercontent.com/AcademySoftwareFoundation/Imath/v3.2.2/LICENSE.md
- Version / commit: `v3.2.2`
- License / scope: BSD-3-Clause; copied bytes unchanged.
- Bytes: 1497; SHA-256: `c20236d3b39fd20eba8e3d1fb3b892a5483df2e7d8d61bf43f165d3fac22f601`

### `yaml-cpp/copyright`

- Local source: `D:/Code/Github/vcpkg/installed/x64-windows/share/yaml-cpp/copyright`
- Upstream URL: https://raw.githubusercontent.com/jbeder/yaml-cpp/yaml-cpp-0.9.0/LICENSE
- Version / commit: `yaml-cpp-0.9.0`
- License / scope: MIT; copied bytes unchanged.
- Bytes: 1061; SHA-256: `aa6fcc27be034e41e21dd832f9175bfe694a48491d9e14ff0fa278e19ad14f1b`

### `pystring/copyright`

- Local source: `D:/Code/Github/vcpkg/installed/x64-windows/share/pystring/copyright`
- Upstream URL: https://raw.githubusercontent.com/imageworks/pystring/v1.2.0/LICENSE
- Version / commit: `v1.2.0`
- License / scope: BSD-3-Clause; copied bytes unchanged.
- Bytes: 1530; SHA-256: `f7f060301402ea0b5c507b865b1a9de037d4d7b24f37422a93b70fe9c7a9b78a`

### `minizip-ng/copyright`

- Local source: `D:/Code/Github/vcpkg/installed/x64-windows/share/minizip-ng/copyright`
- Upstream URL: https://raw.githubusercontent.com/zlib-ng/minizip-ng/4.1.0/LICENSE
- Version / commit: `4.1.0`
- License / scope: Zlib; copied bytes unchanged.
- Bytes: 877; SHA-256: `675181c03fc1302a1c8554c00f7be9bb420c5dbc9dcc2013433cec144413de03`

### `zlib/copyright`

- Local source: `D:/Code/Github/vcpkg/installed/x64-windows/share/zlib/copyright`
- Upstream URL: https://raw.githubusercontent.com/madler/zlib/v1.3.2/LICENSE
- Version / commit: `v1.3.2`
- License / scope: Zlib; copied bytes unchanged.
- Bytes: 1002; SHA-256: `e32ff4e00d9d94930537635291da39e7e612703334bf6fde8c7f1686fe8a45a2`

### `bzip2/copyright`

- Local source: `D:/Code/Github/vcpkg/installed/x64-windows/share/bzip2/copyright`
- Upstream URL: https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz (LICENSE member)
- Version / commit: `1.0.8`
- License / scope: bzip2 full notice; copied bytes unchanged.
- Bytes: 1896; SHA-256: `c6dbbf828498be844a89eaa3b84adbab3199e342eb5cb2ed2f0d4ba7ec0f38a3`

### `liblzma/copyright`

- Local source: `D:/Code/Github/vcpkg/installed/x64-windows/share/liblzma/copyright`
- Upstream URL: https://raw.githubusercontent.com/tukaani-project/xz/v5.8.3/COPYING
- Version / commit: `v5.8.3`
- License / scope: XZ package license inventory; copied bytes unchanged.
- Bytes: 3119; SHA-256: `616a3ad264ce29b8f1cb97e53037b139d406899ca8d1f799651e17bfa09830b8`

### `zstd/copyright`

- Local source: `D:/Code/Github/vcpkg/installed/x64-windows/share/zstd/copyright`
- Upstream URL: https://raw.githubusercontent.com/facebook/zstd/v1.5.7/LICENSE and https://raw.githubusercontent.com/facebook/zstd/v1.5.7/COPYING
- Version / commit: `v1.5.7`
- License / scope: Full installed dual-license concatenation; BSD alternative available; copied bytes unchanged.
- Bytes: 20086; SHA-256: `434dca949c6da7c500413aef694539fe37f867dd1a94d83d4ed1d260194e2660`

### `liblzma/COPYING.0BSD`

- Local source: `D:/Code/Github/vcpkg/buildtrees/liblzma/src/v5.8.3-6de1f0384d.clean/COPYING.0BSD`
- Upstream URL: https://raw.githubusercontent.com/tukaani-project/xz/v5.8.3/COPYING.0BSD
- Version / commit: `v5.8.3`
- License / scope: Full library 0BSD terms, unchanged.
- Bytes: 607; SHA-256: `0b01625d853911cd0e2e088dcfb743261034a091bb379246cb25a14cc4c74bf1`

### `xxHash/LICENSE.embedded.txt`

- Local source: `D:/Code/Github/vcpkg/buildtrees/opencolorio/src/v2.5.2-4011011b88.clean/ext/xxHash/src/include/xxhash.h`
- Upstream URL: https://raw.githubusercontent.com/AcademySoftwareFoundation/OpenColorIO/c52966a6677723d5bd2dbef0ccec3fed9cbc3790/ext/xxHash/src/include/xxhash.h
- Version / commit: `c52966a6677723d5bd2dbef0ccec3fed9cbc3790 (v2.5.2)`
- License / scope: Entire opening BSD-2-Clause comment; header macros 0.8.1.
- Bytes: 1661; SHA-256: `20bf2c9b132fe78c4256abb204159403d987af7a45345f1bdde07ecc094bed7f`

### `sampleicc/iccProfileReader.LICENSE.embedded.txt`

- Local source: `D:/Code/Github/vcpkg/buildtrees/opencolorio/src/v2.5.2-4011011b88.clean/ext/sampleicc/src/include/iccProfileReader.h`
- Upstream URL: https://raw.githubusercontent.com/AcademySoftwareFoundation/OpenColorIO/c52966a6677723d5bd2dbef0ccec3fed9cbc3790/ext/sampleicc/src/include/iccProfileReader.h
- Version / commit: `c52966a6677723d5bd2dbef0ccec3fed9cbc3790 (v2.5.2)`
- License / scope: Entire leading comments: ICC License 0.2 and OCIO modification notice.
- Bytes: 2174; SHA-256: `d53a48311c133d250af89b05e67ea551a37ac91e986fb612f030447967f950f9`

### `sampleicc/icProfileHeader.LICENSE.embedded.txt`

- Local source: `D:/Code/Github/vcpkg/buildtrees/opencolorio/src/v2.5.2-4011011b88.clean/ext/sampleicc/src/include/icProfileHeader.h`
- Upstream URL: https://raw.githubusercontent.com/AcademySoftwareFoundation/OpenColorIO/c52966a6677723d5bd2dbef0ccec3fed9cbc3790/ext/sampleicc/src/include/icProfileHeader.h
- Version / commit: `c52966a6677723d5bd2dbef0ccec3fed9cbc3790 (v2.5.2)`
- License / scope: Entire leading comments; ICC, Heidelberger and SunSoft terms, original byte encoding.
- Bytes: 6315; SHA-256: `6442b7fd7a885bb902745f2d3128107ffd060decc1e6a68a759ab15822442711`

### `builtinConfigACES/LICENSE`

- Upstream URL: https://raw.githubusercontent.com/AcademySoftwareFoundation/OpenColorIO-Config-ACES/97a47181b52a051518163e2a7a8c29f6c7ab8da7/LICENSE
- Version / commit: `97a47181b52a051518163e2a7a8c29f6c7ab8da7`
- License / scope: Config v4.0.0 BSD-3-Clause full text.
- Bytes: 1485; SHA-256: `82a40c52065ee968aa62015735e84378ac11425db7a233e6a2dcd2c83fc24276`

### `builtinConfigACES/aces/LICENSE.md`

- Upstream URL: https://raw.githubusercontent.com/ampas/aces/c2a37a08f898d28ba589a7835028c1442b95f02e/LICENSE.md
- Version / commit: `c2a37a08f898d28ba589a7835028c1442b95f02e`
- License / scope: Academy umbrella full terms.
- Bytes: 2759; SHA-256: `0ff2aaf84aa20b394be4d92414074a526e857a5c6b96e05a9e68452cf0427ed5`

### `builtinConfigACES/aces-core/LICENSE.md`

- Upstream URL: https://raw.githubusercontent.com/aces-aswf/aces-core/2d7af39344725aaa8ac3bf1746693c9a1d6c4792/LICENSE.md
- Version / commit: `2d7af39344725aaa8ac3bf1746693c9a1d6c4792`
- License / scope: Academy core full terms.
- Bytes: 2761; SHA-256: `cf614fceabe08ebc5618bb79ab056ced3e68967cc32f60245bd64fabd6731833`

### `builtinConfigACES/aces-input-and-colorspaces/LICENSE.md`

- Upstream URL: https://raw.githubusercontent.com/aces-aswf/aces-input-and-colorspaces/cc60becf11534f2a40df00a281f47e95412d0599/LICENSE.md
- Version / commit: `cc60becf11534f2a40df00a281f47e95412d0599`
- License / scope: Academy transform full terms.
- Bytes: 2759; SHA-256: `5b7e8d2d0f26d01b9739be0a706a106e3b9616ba19875eb577f340fbb1701b7f`

### `builtinConfigACES/aces-look/LICENSE.md`

- Upstream URL: https://raw.githubusercontent.com/aces-aswf/aces-look/e0e70ecf2ce57d503318dde1fa4aa1bd7287fcdd/LICENSE.md
- Version / commit: `e0e70ecf2ce57d503318dde1fa4aa1bd7287fcdd`
- License / scope: Academy look full terms.
- Bytes: 2759; SHA-256: `5b7e8d2d0f26d01b9739be0a706a106e3b9616ba19875eb577f340fbb1701b7f`

### `builtinConfigACES/aces-output/LICENSE.md`

- Upstream URL: https://raw.githubusercontent.com/aces-aswf/aces-output/aab74723f76728c37345ed01e51ebb24fb1f2f1f/LICENSE.md
- Version / commit: `aab74723f76728c37345ed01e51ebb24fb1f2f1f`
- License / scope: Academy output full terms.
- Bytes: 2748; SHA-256: `ef0987b0b98ab18ef0f11a0ecf184c94adf65d69002d5941affe4ac2bcdc58f9`

