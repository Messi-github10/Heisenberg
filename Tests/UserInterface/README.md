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

Editing supports multiple video and audio tracks. Same-track overlaps are resolved
by overwrite edits; different tracks can overlap. Imported files are external
references, not copied into the package. Offline assets remain visible but cannot be
dragged or added; unsupported track types remain loadable but cannot be edited.
Media import itself is not part of timeline undo history.

## Track-aware clip interaction

- Click a clip body to select it without seeking. Drag its body to move the whole
  clip; the faded original and dashed ghost preview the overwrite position.
- The mouse grab offset is retained. Both clip edges snap within 8 logical pixels
  of the timeline origin, the playhead, or another clip's start/end. A yellow guide
  marks the snapped boundary. Toggle **Snap** in the toolbar/menu, or hold **Shift**
  to temporarily bypass snapping. The moving clip does not snap to its old edges.
- Release inside the original or another compatible lane to commit one overwrite
  move and one undo step. Cross-track moves leave a source gap and preserve clip ID,
  trims, grade and audio settings. Audio/video type mismatches are rejected.
  **Ctrl+Z** undoes it; **Ctrl+Shift+Z** redoes it. The preview reloads the current
  in-memory timeline, so saving is not required before playback.
- **Esc**, releasing outside the lane, or deactivating the window cancels the draft.
  A click, cancelled move, or move back to the original start does not dirty the
  project or consume redo history. Undo/redo cancels a pending draft first.
- Empty-space clicks move the playhead on release; dragging empty lane space selects
  intersecting clips. The ruler still scrubs immediately. Zoom and horizontal scrolling
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
- Click M in a track header to toggle mute, including a video's original audio.
  Click H on video tracks to hide/show picture without changing audio. Mute, gain
  and fades use the existing serialized project fields and survive undo/redo and
  save/reopen.
- Delete a selected clip using its context menu or **Delete**/**Backspace** with
  timeline focus. Those keys do not delete clips while typing in media search.
  Removal is one undo step and never changes the source file.

## Multi-track editing and compositing

- Use Timeline > Add Video Track / Add Audio Track or the track header context
  menu. Right-click a header to rename, move up/down within its media type, or delete
  a track. Removing a populated track asks for confirmation and is fully undoable.
- Drag a media-pool video or an existing timeline video into the 20 px strip above
  the video group, just below the fixed ruler. The strip highlights the proposed
  horizontal range; releasing creates a new top video track and places/moves the
  clip. One undo removes the new track and reverses placement/movement, and redo
  restores stable IDs. Source marks apply to pool drops; moved clips retain their
  own trim, effects and audio payload. Escape/drag leave/invalid drops create no track.
  Audio, trim handles, ruler, headers and the scrollbar do not create video tracks.
  When the true top boundary scrolls away, the top visible lane is not a creation target.
- Click a header name or select a clip to target its track. Video/audio targets are
  remembered separately. Pool overwrite/insert buttons use the selected compatible
  target; direct drops use their actual lane. Track removal, undo and project changes
  must leave a valid target or a predictable compatible fallback.
- Video lanes appear above audio lanes. Within video, the visually upper lane is
  composited over lower lanes. Right-click a video's **Clip Appearance...** to open
  the persistent **Clip Properties** inspector. Center X/Y are canvas percentages (50% centers the
  image); Width/Height scale the existing aspect-fit result independently. Rotation
  is clockwise in degrees and flips use the image's local axes.
- Crop percentages remove the corresponding source sides without zooming or changing
  the full-image rotation pivot. Rounding and softness use the cropped source's short
  side, so scaling/rotation also transforms the mask. Check all four crop sides,
  asymmetric crop plus flips, off-canvas placement and arbitrary rotation.
- Verify opacity 0%, 50% and 100%, existing transparent source pixels, rounded/soft
  corners over a contrasting lower layer, and differently shaped sources whose fitted
  borders should reveal lower layers rather than black. Small/downscaled images must
  not acquire a fringe from RGB stored in fully transparent texels.
- Clip Properties targets the primary selected video clip and keeps the active inspector
  tab when selection changes. Its five property groups display sampled values as the
  playhead moves. Missing/empty tracks edit constants; nonempty tracks insert/update
  a key at the current local frame. Editing one position/scale/crop component preserves
  the other sampled components and existing opaque metadata.
- Use each group's diamond to add/remove a key, the arrows to seek the actual timeline
  preview, and Linear/Hold/Smooth to edit the exact key's outgoing interpolation.
  Removing the last key restores the stored constant. Outside the clip's half-open
  range, key writes and animated values are disabled with an explanation; constant
  values and navigation to in-range keys remain available. Negative/future imported
  anchors remain stored but are not shown as clip-body markers or navigation targets.
- Drag the ↔ handle beside a number horizontally or hold a spinbox arrow: one gesture is one undo.
  Keyboard confirmation/focus-out commits; Escape cancels. Wheel scrolling must not
  change values. Save/export, selection/frame/tab changes and structural edits finish
  the gesture; switching between grading and properties commits the previous edit.
- Selected video clips show the union of their five keyframe groups as bottom diamonds,
  merging identical frames. Verify multiple selected clips, zoom/scroll, clip bounds,
  and marker refresh after undo/redo, split, trim and move. Markers retain ordinary
  clip selection/drag behavior and never modify project data.
- `opacityTrack`, `positionTrack`, `scaleTrack`, `rotationTrack` and `cropTrack` execute
  in timeline preview and export. A nonempty track overrides its corresponding constant,
  including a fade-in from constant opacity 0. Source preview continues to ignore
  timeline geometry/opacity while retaining grade.
- The imported format is `{"keyframes":[{"frame":0,"value":0,"interpolationOut":"smooth"},
  {"frame":12,"value":1}]}` (opacity example). Frames must be strictly increasing
  integers. `localFrame = timelineFrame - clip.startFrame`; source trims and speed
  do not alter appearance time. Values clamp at the endpoints; an absent track or
  `{"keyframes":[]}` uses constants, while `{}` is invalid. Position values use
  `{"a":centerX,"b":centerY}` normalized to the canvas; scale uses `{"a":width,"b":height}`
  as direct positive multipliers of the fitted size. Rotation is an absolute clockwise
  scalar angle without shortest-arc wrapping. Crop requires normalized `left`, `top`,
  `right`, `bottom` sides leaving a positive rectangle; opacity must remain in [0,1].
- Verify imported Linear, Hold and Smooth clips at multiple frames after save/reopen
  and actual H.264/H.265 export. The outgoing left key chooses `linear` (default),
  `hold`, or `smooth`; Smooth matches Grade's `t*t*(3-2*t)` easing, without Bezier
  handles. Preview strict false/true must sample supported animation identically.
  Unknown/invalid animation remains loadable and saveable, but preview/export must
  reject execution even when hidden, zero-opacity, inactive or blank. Animated flips,
  rounding/softness, blend mode and audio volume tracks are not supported here.
- **Blend Mode** in Clip Appearance offers Palmier's 16 modes, followed by Add.
  Check Normal, Multiply, Screen, Overlay, Add and Difference over a contrasting
  lower layer, then the four color-component modes on asymmetric colored sources.
  Opacity fades a non-normal result back toward the lower canvas; transparent crop
  and rounded corners must reveal the backdrop unchanged. All calculations are linear
  ACEScg, so encoded-sRGB screenshots from another editor need not match.
- Verify unchanged/absent Normal stays a no-op, cancellation preserves the original,
  mode plus geometry is one undo step, and save/reopen restores the exact mode name.
  Imported unknown modes appear as Unsupported and remain unchanged until a supported
  replacement is chosen; timeline preview and export reject them explicitly.
- Wheel scrolls vertically beneath the fixed ruler; Shift-wheel or horizontal wheel
  scrolls horizontally, Ctrl-wheel zooms. Verify hit testing, track buttons, cross-track
  ghosts and drops after scrolling and after deleting tracks near the bottom.
- Hidden video tracks retain their original sound; muted tracks retain picture.
  All independent audio tracks and video-original audio mix together with per-clip
  gain/fades. Save/reopen and actual export must reproduce order, opacity, visibility,
  mute states and audio tails. Pending grades finish before structural track edits.

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

## Multi-selection and group moves

- Ctrl-click or Shift-click toggles a clip; Ctrl+A selects every timeline clip when
  the timeline has focus. Selected clips have blue outlines; the primary clip has a
  white outline and remains the target of grading, appearance and audio settings.
- Drag empty lane space to select every intersecting painted clip. Ctrl/Shift-drag
  adds hits to the previous selection. The rectangle previews without changing the
  committed selection until release. Escape cancels it; idle Escape clears selection.
- Pressing a selected clip preserves the group for dragging. A plain click without
  a drag reduces the selection to that clip. Modifier-click never starts a drag.
- Group drags retain time offsets and gaps, including at frame zero. Vertical moves
  preserve row spacing among the lead clip's media type; other selected types follow
  in time on their existing tracks. An out-of-range destination rejects the whole move.
- Snap considers all member boundaries and excludes every selected clip's old edges.
  Shift temporarily disables snapping. All ghosts must match the resulting placement.
- Overwrite affects only occupied destination intervals, preserving stationary content
  between disjoint selected intervals. One undo/redo restores/reapplies the entire move.
- A selection from one video track can create one top video track via the existing
  strip. Mixed media or selections spanning several source tracks cannot use this
  strip; they can move to compatible existing rows.
- Delete/Backspace removes all selected clips in one undo step. Split, Ripple Delete
  and edge trimming require one selected clip. Right-clicking a selected member keeps
  the group. Check focus isolation in media search and numeric inputs.
- Verify selection pruning after deletion, undo, track removal and project switching;
  verify pending grading commits separately before a group edit. Save/reopen must
  preserve all moved IDs, tracks, trims and effects. Selection itself is not saved.

Synchronized cross-track ripple and a separate razor-tool mode are not included.
