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
   to return to the edit. Clicking the timeline also switches back.
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
