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

There is no advanced blend-mode compositing, animated geometry/opacity, hardware
encoding, HDR output, waveform cache, or pitch-preserving time stretching.

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
