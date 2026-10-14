# Heisenberg

The slowest video color grading engine.

## Architecture

Framework is pure C++; UserInterface adapts its editing and playback APIs to Qt.

```text
EditorSession -> VideoProject
                  |- EditorViewModel -> ProjectFile / MediaManifest / edit history
                  `- ProjectPackageCoordinator -> ProjectCodec / package files

PlayerController -> IPreviewer -> VideoEngine
                                 |- CompositionBuilder -> CompositionPlan
                                 |- one decode worker -> DemuxContext / DecoderContext
                                 |                       AudioResampler / AudioMixer
                                 `- render/clock worker -> FrameRenderer / VideoNormalizer
                                                          VideoPresenter / swapchain
```

`VideoProject` owns the sole editor and creates value snapshots of the project,
manifest, package context and revision. Saving checkpoints the captured revision;
newer edits stay dirty. Package writes are staged beside the destination with
rollback on replacement failure. They are not crash-journaled transactions.

`CompositionBuilder` is a pure single-track planner. It resolves expected source
paths without probing files, maps half-open clip intervals, trims and speed, and
captures hide/mute flags. File availability is diagnosed when `VideoEngine` opens
a decoder session; runtime diagnostics are available in `VideoEngine::Snapshot`.
Source preview and timeline preview use the same decode worker and transport.

The UI thread controls the engine and receives dispatched notifications. The decode
worker owns all demux/decoder sessions and prepares video/audio data. The renderer
worker owns GPU resources, normalization, filter execution, resize and presentation,
and runs the playback clock. The audio device callback only consumes prepared PCM.
All first-frame, seek and scrub video callbacks originate from the render consumer.

`VideoFrame` retains decoded-image resources and source timing separately from its
intended output position. `TimelineSettings` carries runtime video/audio settings;
`MediaMath` provides rational frame/sample conversion. Existing Palmier package
JSON remains compatible: integer timeline FPS, dimensions and manifest version 2.
Settings that cannot be represented by that schema are rejected by project editing.

This version supports the existing single-video-track workflow. It does not add
multitrack composition or export. Realtime audio speed mapping does not provide
pitch-preserving time stretching.

## Verification

The Framework regression target covers models, package transactions, composition
planning, audio timing, frame ownership, render-command ordering and engine
transport. The native preview test creates a real Win32 window and verifies
software/D3D11 source playback, timeline rendering, seek, scrub, resize and replay.
Qt UI tests exercise importing, editing, undo/redo and unsaved preview refresh.
