# Media pool validation

Build `MainApp` and `HeisenbergEditorUiTests`, then run CTest from the build directory.
The Qt tests use the offscreen platform, generated 24 fps video with audio, and the
bundled FFmpeg executable; no user media or GPU renderer is needed. The test runner
must be allowed to launch FFmpeg and create temporary project packages.

## Manual smoke test

1. Launch `cmake-build-debug/UserInterface/MainApp.exe`.
2. Use **Ctrl+I** or **Import...** to import videos (multiple selection is supported).
   Alternatively, drop local video files into the media pool.
3. Check thumbnails, names, duration/resolution, and search. Importing the same
   canonical path twice must not create another asset.
4. Double-click a tile to preview the source; use **Esc** or **Timeline Preview**
   to return to the edit. Scrubbing the timeline ruler or empty space also switches back.
5. Drag a tile onto the video lane. The ghost shows the overwrite position; an
   empty timeline creates its first video track. The edit can extend beyond the
   previous duration and plays immediately without saving.
6. Drop another tile over existing content. **Ctrl+Z** and **Ctrl+Shift+Z** must
   undo/redo the whole overwrite in one step. **Add Selected at Playhead** is an
   alternative to drag/drop.
7. Use **Ctrl+S** to save a new `.palmier` directory package. Reopen that directory
   with **Ctrl+O** and check both the pool and clips. Save As preserves existing
   package media and unknown files.

This first version imports/previews/places video only. Imported files are external
references, not copied into the package. Offline assets remain visible but cannot
be dragged or added; multi-track active timelines are retained but editing is
explicitly unsupported. Media import itself is not part of timeline undo history.

## Single-track clip interaction

- Click a clip body to select it without seeking. Drag its body to move the whole
  clip; the faded original and dashed ghost preview the overwrite position.
- The mouse grab offset is retained. Both clip edges snap within 8 logical pixels
  of the timeline origin, the playhead, or another clip's start/end. A yellow guide
  marks the snapped boundary. Toggle **Snap** in the toolbar/menu, or hold **Shift**
  to temporarily bypass snapping. The moving clip does not snap to its old edges.
- Release inside the video lane to commit one overwrite move and one undo step.
  **Ctrl+Z** undoes it; **Ctrl+Shift+Z** redoes it. The preview reloads the current
  in-memory timeline, so saving is not required before playback.
- **Esc**, releasing outside the lane, or deactivating the window cancels the draft.
  A click, cancelled move, or move back to the original start does not dirty the
  project or consume redo history. Undo/redo cancels a pending draft first.
- The ruler/empty space still moves the playhead. Zoom and horizontal scrolling
  preserve timeline frame mapping. Multi-track documents remain selectable/viewable
  but moving their clips is explicitly rejected.

Clip edge trimming, multi-selection, and ripple editing are not part of this step.
