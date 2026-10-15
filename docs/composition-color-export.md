# Shared composition, ACES and export

The Framework composition path is independent of Qt and windows. `CompositionBuilder`
freezes the project into a value `CompositionPlan`. Each preview or export job owns
its own `CompositionExecutor` and renderer; neither shares a mutable decode cursor.

## Ownership

- `VideoProject` owns the editor; `ProjectSnapshot` owns a document/manifest capture.
- `CompositionExecutor` decodes and mixes synchronously. It has no clock, device or
  worker. Original source timestamps remain separate from target positions.
- `GradeCompiler` samples clip-local effect parameters and produces graph stages.
  It does not run GPU work or select the OCIO configuration.
- `Renderer/Color` owns OCIO processors, generated shaders and LUTs.
- `CompositionRenderer` creates ACEScg frames without a window, applies the chosen
  output transform, and provides CPU readback. Its GPU frame leases keep resources
  alive until consumers complete. Its context must outlive the leases. The former
  `FrameRenderer`/`VideoNormalizer` API remains for compatibility; preview and
  export both use the new renderer rather than feeding legacy converted images
  into ACES as if they were original source pixels.
- `VideoPresenter` only presents prepared, output-transformed images.
- `VideoEngine` retains real-time queues and audio-device/wall-clock scheduling.
- `ExportEngine` schedules offline work and commits output. Video/Audio encoders
  handle codec state; `Muxer` handles container state.

Preview keeps its UI, unified decode, and render/clock application threads. The
sound-device callback only consumes PCM. An export task has a separate worker and
independent execution/render resources.

## Color contract

The fixed built-in configuration is
`studio-config-v4.0.0_aces-v2.0_ocio-v2.5`. No external `.ocio` file is required.
Working images are ACEScg (AP1, D60, linear); ACEScct exists only within appropriate
Grade stages. Linear exposure and blur run in ACEScg; log-domain contrast and
saturation run in ACEScct. Empty grade stages do not force a log round trip.

FFmpeg decodes compressed streams. libplacebo reconstructs RGB from YUV and handles
placement/scaling; OCIO performs the input, working/grading and output color
transforms. Source transfer functions and display transforms must not be applied
again by the presenter.

The default preview display is `sRGB - Display`. SDR video export uses
`Rec.1886 Rec.709 - Display`. New projects use the ordinary-SDR mode (input policy
and semantics version 2), with the paired `Un-tone-mapped` view. A finished Rec.709
video is interpreted as `Rec.1886 Rec.709 - Display`, not `Camera Rec.709`. Its
colorimetric bridge into ACEScg and back avoids applying a second scene-rendering
transform. ACEScg remains the shared linear working representation; this conversion
does not recover the camera's original scene exposure.

Ordinary-SDR preview explicitly uses a code-preserving playback intent for
finished Rec.709 inputs. OCIO reverses that input's Rec.1886 encoding after the
shared ACEScg/Grade path, and the presenter carries those SDR code values to the
verified UNORM sRGB surface without a second gamma 2.4-to-sRGB conversion. This is
a playback-compatibility intent, not a claim of calibrated colorimetric display
matching. It avoids the measurable darkening of ordinary video codes produced by
`encode_sRGB(pow(code, 2.4))`. No exposure offset or working-space bypass is used.

The intent is runtime preview-only: exported Rec.709 pixels and tags remain
unchanged, and direct renderer output defaults to the existing colorimetric
behavior. Explicit sRGB/linear/camera overrides, legacy policy and ACES views do
not acquire this bypass. Output metadata records the actual Rec.1886 encoding
and the explicit presentation intent rather than silently relabeling the image.
An explicit `ACES rendering` mode retains the `ACES 2.0 - SDR 100 nits (Rec.709)`
view; that creative rendering can change an already finished video's appearance
even with no grade effects. Its normal display transfer adaptation is unchanged.

Existing version-1 color extensions preserve their saved Camera Rec.709 policy
and ACES output defaults, including when old fields are omitted. They appear as
`Legacy ACES` in Edit > Color Settings. Select `Ordinary SDR` there to upgrade to
the new policy in one undoable edit; input overrides are retained. Packages without
any color extension explicitly migrate to the ordinary-SDR defaults. Unknown
future versions remain preserved but are not silently interpreted.

Ambiguous input can be overridden by media reference. Unresolved fallbacks retain
a once-per-source warning and are rejected by strict export. Camera Log is not
inferred from a generic transfer label, file extension or filename. Successful
color paths emit bounded, deduplicated INFO records containing observed metadata,
resolved input, effective range, policy and output selection. The presenter logs
the real surface format and color contract once, supports verified full-range SDR
sRGB UNORM targets, and rejects unknown/HDR/scRGB or implicit-sRGB attachments
rather than displaying incorrectly encoded values.

Color settings are stored in the versioned `org.heisenberg.color` project extension.
Unknown extension fields are preserved. This extends Heisenberg's Palmier reader;
it does not guarantee that another application will preserve these extra fields.

## Effects

Effects use the existing ordered array representation, for example:

```json
[{"id":"basic-grade","type":"video.basicGrade","enabled":true,
  "params":{"exposure":{"value":0},"contrast":{"value":1},
            "saturation":{"value":1}}}]
```

A parameter may carry a `track` containing `keyframes` with `frame`, `value` and
`interpolationOut`. Hold and Linear are evaluated in clip-local timeline frames,
not native source PTS. Negative and future anchors preserve interpolation across
split/trim edits. Smooth and unknown effects are diagnosed; strict export refuses
them. Existing advanced geometry and audio automation are not implicitly supported
by preserving their JSON.

## Export contract

The first output is SDR MP4: software H.264 or H.265, YUV420p, plus 48 kHz stereo
AAC. Output dimensions and frame rate come from the timeline; odd dimensions are
rejected rather than silently cropped. Video-original audio and the independent
track are mixed before one AAC encoder. Audio-only timelines have a black video
canvas. An audio tail extends the black picture to the complete timeline duration.

The exporter captures the active timeline rather than whichever source is open in
the player. Codec/quality options do not modify project settings or undo history.
AAC encoder delay, final partial frames and presentation duration are separate
concepts: a raw decoder can return a padded final AAC frame even when MP4 has the
correct logical duration. Consumers must honor the presentation duration/edit list.

Export writes a unique staging file beside the destination. It drains codecs,
finishes and closes the container, then commits the completed file. Cancellation
and failure remove staging output and preserve an existing destination. Replacing
an existing destination requires an explicit overwrite request. This is not a
power-loss durability guarantee. Already submitted GPU/codec work is allowed to
finish safely before resources are released.

Random backward audio access currently decodes from source origin to preserve
priming and resampler phase. This favors correctness and can make long-source
seeks slower; bounded caches do not imply constant-time seeking.

## Build and deployment

OCIO 2.5.2 and the shared shaderc compiler are repository-owned precompiled SDKs
under `Dependence/OpenColorIO` and `Dependence/Shaderc`. Headers, import libraries,
Release/Debug DLL closures, licenses and SHA-256 inventories travel together.
Neither application compilation nor runtime deployment searches an external vcpkg
installation or the installed SDK for those libraries. The Vulkan SDK remains a
build-tool prerequisite for glslangValidator, spirv-val and spirv-cross. Run
`python scripts/verify_dependency_snapshots.py` to verify the vendored snapshots;
fetch binary content with Git LFS after cloning. Color processors and GPU programs use bounded per-instance
caches; dynamic uniforms update without rebuilding pipelines or re-uploading LUTs.
The imported libplacebo device receives only the features/extensions actually enabled
at device creation, capped to the instance API version, rather than all capabilities
advertised by the physical GPU. Runtime deployment includes OCIO, its dependent DLLs, shaderc,
and shader resources. Building the Framework without Qt is available through
`HEISENBERG_BUILD_UI=OFF`; normal builds keep the desktop application enabled.

The bundled FFmpeg binaries enable GPLv3 components, including x264/x265; the older
LGPL-only source comment was inaccurate. Distribution must include appropriate
notices and satisfy the actual binaries' corresponding-source obligations. Codec
patent licensing is separate from the software license. New OCIO/transitive
copyright notices are deployed alongside the application.

## Validation

The integration check is `python scripts/verify_vulkan_integration.py Release`.
It enables the Khronos validation layer, exercises window presentation, windowless
Grade/OCIO, software/D3D uploads, shared-queue workers and MP4 export, and fails on
validation messages even if GoogleTest itself reports success. Color GPU tests
also explicitly create a debug messenger and assert their own error count.

The current Windows backend requires the actual Vulkan 1.2 device contract plus
`VK_KHR_synchronization2` and the surface capabilities extension used by the bundled
libplacebo swapchain. Unsupported required features produce an initialization error.
