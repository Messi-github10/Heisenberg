# Media pool validation

Build `MainApp` and `HeisenbergEditorUiTests`, then run CTest from the build directory.
The Qt tests use the offscreen platform, generated 24 fps video with audio, MP3
and longer WAV audio fixtures, and the bundled FFmpeg executable; no user media
or GPU renderer is needed. The test runner
must be allowed to launch FFmpeg and create temporary project packages.

## Manual smoke test

1. Launch `cmake-build-debug/UserInterface/MainApp.exe`.
2. Use **Ctrl+I** or **Import...** to import video or audio (multiple selection is
   supported), or drop local files into the media pool. Audio filters include MP3,
   WAV, AAC, M4A, FLAC and OGG.
3. Check thumbnails, names, duration/resolution, and search. Audio tiles show an
   AUDIO label and duration without generating thumbnails. Importing the same
   canonical path twice must not create another asset.
4. Single-click a tile to preview the source, initially paused; press Play to
   audition audio using the shared source progress controls. Use **Esc** or
   **Return to Timeline** to return to the edit. Scrubbing the timeline ruler or
   empty space also switches back.
5. Drag a tile onto its matching video/audio lane. The ghost shows the overwrite
   position; an absent lane creates its track, with video above audio. Audio-only
   edits are valid. Wrong-type drops are rejected. The edit can extend beyond the
   previous duration and previews immediately without saving.
6. Drop another tile over existing content. **Ctrl+Z** and **Ctrl+Shift+Z** must
   undo/redo the whole edit in one step. **Overwrite at Playhead** and **Insert at
   Playhead** are explicit alternatives to drag/drop.
7. Use **Ctrl+S** to save a new `.berg` directory package. Reopen that directory
   with **Ctrl+O** and check both the pool and clips. Save As preserves existing
   package media and unknown files.

Editing supports at most one video track and one independent audio track.
Imported files are external references, not copied into the package. Offline
assets remain visible but cannot be dragged or added; duplicate same-type tracks
and other unsupported layouts are retained but cannot be edited. Media import
itself is not part of timeline undo history.

## Track-aware clip interaction

- Click a clip body to select it without seeking. Drag its body to move the whole
  clip; the faded original and dashed ghost preview the overwrite position.
- The mouse grab offset is retained. Both clip edges snap within 8 logical pixels
  of the timeline origin, the playhead, or another clip's start/end. A yellow guide
  marks the snapped boundary. Toggle **Snap** in the toolbar/menu, or hold **Shift**
  to temporarily bypass snapping. The moving clip does not snap to its old edges.
- Release inside the clip's original lane to commit one overwrite move and one
  undo step. Moving clips between tracks is not supported.
  **Ctrl+Z** undoes it; **Ctrl+Shift+Z** redoes it. The preview reloads the current
  in-memory timeline, so saving is not required before playback.
- **Esc**, releasing outside the lane, or deactivating the window cancels the draft.
  A click, cancelled move, or move back to the original start does not dirty the
  project or consume redo history. Undo/redo cancels a pending draft first.
- The ruler/empty space still moves the playhead. Zoom and horizontal scrolling
  preserve timeline frame mapping. Both supported lanes use track-aware selection,
  move/trim ghosts and hit testing; unsupported layouts remain viewable but their
  editing is rejected.

## Edge trimming and splitting

- Hover near a clip's left/right edge: the horizontal resize cursor and grips
  indicate trimming. Drag the left edge to change the in-point; drag the right edge
  to change the out-point. The opposite edge stays fixed. The ghost shows the
  proposed range; the project only changes when released inside the original lane.
- Each clip keeps at least one frame. Extending is limited to previously trimmed
  source frames (and never before timeline frame zero). **[Limit]** indicates a
  clamped boundary. Fractional speeds use the same rounded source-frame mapping
  as the editor. The edited edge uses **Snap**/**Shift** exactly like clip moves.
- Extending over another clip uses overwrite, not ripple: neighboring content is
  trimmed/removed as needed, without pushing the rest of the timeline. The entire
  extension is one undo step. **Esc** or releasing outside the lane cancels it.
- Select a clip, scrub the ruler to a frame strictly inside that clip, then click
  **Split at Playhead** in the toolbar/Edit menu, or press **S** with timeline focus.
  S does not intercept typing in media search. The cut makes two adjacent clips;
  the left keeps its ID/selection and the right gets a stable new UUID. Boundary
  cuts, missing selection, and cuts outside the selected clip change nothing.
- Trims/splits maintain source trims, speed, fades, and clip-relative timing
  payloads. **Ctrl+Z** / **Ctrl+Shift+Z** restores/reapplies each complete edit,
  including generated IDs. Playback updates before saving; save/reopen retains
  the edit. These are non-destructive edits: original media files are unchanged.

## Audio settings, mute and deletion

- Right-click a video or audio clip and choose **Audio Settings...**. Gain is
  0–200%; linear fade in/out are entered in seconds and rounded to timeline
  frames. The two fades must fit the clip. Changing controls is only a local
  draft; OK commits one undo step, while Cancel or unchanged OK commits nothing.
- Click an existing track header to toggle mute, including a video's original
  audio. Mute, gain and fades use the existing serialized project fields and
  survive undo/redo and save/reopen.
- Delete a selected clip using its context menu or **Delete**/**Backspace** with
  timeline focus. Those keys do not delete clips while typing in media search.
  Removal is one undo step and never changes the source file.

## Source range, insert and ripple delete

- Preview a registered media-pool item, seek or step to a frame, then use **Mark In
  (I)** / **Mark Out (O)**. The preview panel must have focus for the letter keys;
  media search and grading fields retain ordinary text input. Out includes the
  currently displayed frame; the label shows its exclusive ending time.
- **Clear Range** restores full-source placement. One-sided marks default to the
  source beginning/end. Marks follow each media ID across source switches, remain
  session-local, do not dirty the project, and reset after a successful project load.
- Verify a 24 fps source range [6,18) placed in the default 30 fps timeline starts
  at source trim 8 and lasts 15 timeline frames. Drag ghosts and both placement
  buttons use this same rounded range. Empty or inverted ranges must not create edits.
- Select **Insert** in the toolbar or **Insert Media Drops** in the Timeline menu.
  A new media drop pushes later content on its target track by the selected duration;
  an insertion inside a clip splits it. The other track stays in place. Existing
  clip moves still overwrite. Switch back to **Overwrite** to retain prior drop behavior.
- **Shift+Delete**, **Ripple Delete** in the clip context menu, or **Ripple Delete
  Selected Clip** in Edit removes the selected duration and shifts later clips on
  that track forward. Other gaps keep their lengths. Delete/Backspace still leaves
  a gap. Check undo/redo, grade/audio payloads, and save/reopen for each operation.

Multi-selection, synchronized cross-track ripple, and a separate razor-tool mode
are not included.
