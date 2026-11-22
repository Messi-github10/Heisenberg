# Heisenberg

The slowest video color grading engine.

## Architecture

Framework is pure C++; UserInterface adapts its editing and playback APIs to Qt.

```text
EditorSession -> VideoProject
                  |- EditorViewModel -> ProjectFile / MediaManifest / edit history
                  `- ProjectPackageCoordinator -> ProjectCodec / package files

PlayerController -> IPreviewer -> VideoEngine (real-time clock / queues)
                                 `- CompositionExecutor (decode / sample / mix)
ExportController -> ExportEngine `- CompositionExecutor (independent instance)

VideoProject snapshot -> CompositionBuilder -> CompositionPlan
CompositionRenderer -> Color (OCIO / ACEScg) -> Grade -> FilterGraph
                    -> output transform -> VideoPresenter / swapchain
                    -> output transform -> VideoEncoder -> Muxer
AudioMixer -> AudioDevice (preview) / AudioEncoder -> Muxer (export)
```

`VideoProject` owns the sole editor and creates value snapshots of the project,
manifest, package context and revision. Saving checkpoints the captured revision;
newer edits stay dirty. Package writes are staged beside the destination with
rollback on replacement failure. They are not crash-journaled transactions.

`CompositionBuilder` captures an ordered set of video and audio tracks.
It resolves expected source
paths without probing files, maps half-open clip intervals, trims and speed, and
captures hide/mute flags, visual effects and versioned color settings. File availability
is diagnosed by the instance-local `CompositionExecutor`; preview diagnostics are
available in `VideoEngine::Snapshot`. Source preview and timeline preview use the
same decode worker and transport.

The UI thread controls the engine and receives dispatched notifications. The decode
worker owns all demux/decoder sessions and prepares video/audio data. The renderer
worker owns GPU resources, normalization, filter execution, resize and presentation,
and runs the playback clock. The audio device callback only consumes prepared PCM.
All first-frame, seek and scrub video callbacks originate from the render consumer.

`VideoFrame` retains decoded-image resources and source timing separately from its
intended output position. `TimelineSettings` carries runtime video/audio settings;
`MediaMath` provides rational frame/sample conversion. Berg projects use the `.berg`
directory suffix. Existing package JSON remains compatible: integer timeline FPS,
dimensions and manifest version 2. Older directory packages can still be opened
and saved in place; Save As defaults to `.berg`.
Settings that cannot be represented by that schema are rejected by project editing.

Video and audio tracks support overlapping contributions across tracks, including
audio-only projects. Within each track, clips remain non-overlapping. The upper video
track is composited over lower tracks in linear ACEScg, preserving source alpha,
transparent fitted borders and constant clip opacity. Each layer keeps its own grade;
the composition-wide graph and output transform run after stacking.

Clip Appearance edits constant center position, independent width/height scales,
clockwise planar rotation, local horizontal/vertical flips, source-relative crop,
rounded corners, feathered edges and opacity. Geometry runs after each clip's grade
and before stacking, in the shared preview/export renderer. The default transform
retains centered aspect fit (including sample aspect ratio); width/height multiply
that fitted size. Cropping removes source content without zooming or recentering.
Rounding and softness are measured against the shorter cropped source dimension,
then transform with the image. Filtering uses alpha-weighted colors so hidden RGB
cannot contaminate transparent borders. Perspective rotation retains a planar
preview fallback and is rejected by strict export.

Preview and export also execute imported `opacityTrack`, `positionTrack`,
`scaleTrack`, `rotationTrack` and `cropTrack` payloads. This round adds no keyframe
UI: Clip Appearance still edits constants and preserves imported tracks. Each
track is an object with a `keyframes` array, for example
`{"keyframes":[{"frame":0,"value":0,"interpolationOut":"smooth"},{"frame":12,"value":1}]}`
for opacity. Frames are strictly increasing integers in clip-local timeline time:
`localFrame = timelineFrame - clip.startFrame`, without adding source trim or
multiplying by playback speed. Values clamp to the first/last key outside the key
range. A missing track or `{"keyframes":[]}` uses its constant; `{}` is invalid.

Opacity values are scalars in [0,1] and replace constant opacity, so a zero constant
can still animate a fade-in. Position values are `{"a":centerX,"b":centerY}` in
normalized canvas coordinates. Scale values are `{"a":width,"b":height}` with
positive independent multipliers of the aspect-fit size; they replace the constant
width/height directly. Rotation values are absolute clockwise degrees, interpolated
as scalars without shortest-arc wrapping. Crop values contain all four normalized
`left`, `top`, `right`, `bottom` sides and must leave positive width and height.
The left key's `interpolationOut` selects `linear` (the default), `hold`, or `smooth`.
Smooth uses `t*t*(3-2*t)`, the same easing as Grade; it has no Bezier tangents.

Both preview strictness modes sample these supported tracks identically. Invalid
or unknown animation payloads remain saveable and survive reopening, but cannot
execute: preview and export reject them even on hidden, zero-opacity or currently
inactive clips and blank timeline frames. Export validates before accessing the
output target. Perspective, animated flips, rounding/softness, blend mode and audio
volume tracks remain outside this appearance-animation support.

Tracks can be added, removed, renamed and reordered within their media type. Clips
can move between compatible tracks. Drag a pool video or an existing video clip into
the narrow strip above the video lanes to create a new top video track and place/move
it in one undo step. Cancelling the drag leaves no empty track.
Header M controls mute; video H controls picture
visibility independently. Every video track's original audio and all audio tracks are
mixed, with per-clip 0–200% gain and linear fades applied before final clamping.
Audio can continue after video ends while the preview presents a black canvas.
Track edits use the existing undo history and serialized package fields.

Timeline clips support Ctrl/Shift-click selection, drag-to-select marquees from empty
lane space, and Ctrl+A with timeline focus. Dragging a selected clip moves the whole
selection with one time offset and one undo step. Same-type tracks retain their row
spacing during vertical moves; other selected media types keep their tracks while
following the time offset. Delete removes the selection in one step. The primary
selected clip remains the target for grading and clip settings; splitting, ripple
removal and edge trimming require a single selected clip. Empty clicks seek on
release, while ruler drags scrub immediately. Selection is transient UI state.

Source preview supports session-local In/Out marks (I/O while the preview has focus).
Media-pool placement uses the marked range, with separate overwrite and insert actions.
The timeline's Overwrite/Insert selector controls new media drops; existing clip moves
retain overwrite behavior. Insert shifts only the destination track, splitting a clip
at the insertion point when necessary. Shift+Delete ripple-deletes the selected clip
and closes its duration on that track; ordinary Delete/Backspace leaves a gap.
These edits are atomic undo steps. Marks themselves are not saved or added to history.

Shared windowless composition uses OCIO's fixed ACES 2.0 Studio configuration:
ACEScg working images, ACEScct grading stages, and separate preview/export output
transforms. Offline MP4 export uses software H.264/H.265 and AAC, independent of the
playback clock and window. See [composition, color and export](docs/composition-color-export.md)
for input policies, effect semantics, deployment and output-safety details.

There is no keyframe editing UI, hardware encoding, HDR output, waveform cache,
or pitch-preserving time stretching.

Clip Appearance also selects a constant blend mode. The fixed table follows the
published Palmier list: Normal, Darken, Multiply, Color Burn, Lighten, Screen,
Color Dodge, Overlay, Soft Light, Hard Light, Difference, Exclusion, Hue,
Saturation, Color and Luminosity. Add is provided as a seventeenth mode.
All modes, including Normal, use a custom two-input Vulkan compute compositor;
libplacebo remains responsible for unpacking and geometry filtering, not blending.
The lower canvas is the backdrop, the current upper clip is the source, and the
final canvas remains opaque. Source alpha includes geometry coverage; non-normal
results fade back to the backdrop according to clip opacity. Unknown names are
preserved in project data but rejected by preview and export until replaced.

Blend calculations run in linear ACEScg. Normal, Add, Darken, Multiply, Lighten,
Screen, Overlay, Hard Light, Difference and Exclusion use the signed/HDR input
values without an overall RGB clamp. Color Burn, Color Dodge, Soft Light and the
four nonseparable modes bound their blend operands to [0,1] to define their
normalized-domain formulas; this does not clip the original backdrop when applying
alpha/opacity. Dodge/Burn use explicit zero/one endpoint branches. Hue/Saturation/
Color/Luminosity use the PDF/W3C luminosity/saturation helpers (0.30/0.59/0.11), not
HSV substitution. These formulas and the opacity graph follow Palmier's named
Core Image operations, but pixel-identical Core Image behavior is not claimed:
Palmier delegates those kernels to Apple and uses a different color pipeline.

## Third-party dependencies

Precompiled SDKs are managed under `Dependence/`, including
[OpenColorIO](Dependence/OpenColorIO/README.md) and
[shaderc](Dependence/Shaderc/README.md), with separate Debug/Release libraries and
runtime files. No external vcpkg installation is required for those dependencies.
Fetch binary contents with Git LFS after cloning, and verify the snapshots with
`python scripts/verify_dependency_snapshots.py`. Qt, MSVC and Vulkan shader build
tools remain development prerequisites.

## Verification

The Framework regression target covers models, package transactions, composition
planning, audio timing, frame ownership, render-command ordering and engine
transport. The native preview test creates a real Win32 window and verifies
software/D3D11 source playback, timeline rendering, seek, scrub, resize and replay.
Qt UI tests exercise importing, editing, undo/redo and unsaved preview refresh.
