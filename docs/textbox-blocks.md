# Embedded blocks in TextBox

TextBox can reserve fixed-height cell slots between ordinary text entries and
its mutable streaming tail. It owns only document layout and sanitized fallback
text: the application still owns child widgets, images, decoding and hit routing.
There is no media, transport, or AIForge dependency.

The block API is available starting with TermForge v0.57.27.

```cpp
TextBox transcript;
transcript.append("An ordinary turn");
auto inserted = transcript.append_block(6, StyledText{
    TextSpan{"[local preview: caption and open action]", TextStyle{}}});
if (!inserted) {
  // Surface inserted.error() through the application's error channel.
  return;
}
TextBlockHandle preview_slot = *inserted;
auto reply = transcript.begin_entry("Streaming reply...");
```

Inserting a block finalizes any live tail, just like appending ordinary text.
Its fallback wraps with TextBox's styled/Unicode policy but cannot enlarge the
reserved height: excess rows are clipped and unused rows remain blank. Height
zero explicitly collapses the block, retaining its fallback and identity.
`update_block` replaces both height and fallback; `set_block_rows` changes just
height; `remove_block` retires the slot. All mutations and geometry queries run
on the TextBox's owner thread, like its existing text and draw operations.

## Layout, clipping, and children

Set the transcript's geometry, draw its authored cells, then query
`block_geometry(handle)` in that same `App::on_render`:

```cpp
transcript.set_geometry(document_rect);
transcript.draw(screen);

preview_widget.set_geometry({}); // never leave last frame's hit area live
auto layout = transcript.block_geometry(preview_slot);
if (!layout) {
  // Eviction/removal/clear retired this slot. Stop collecting its child.
} else if (layout->state == TextBlockLayoutState::Visible) {
  preview_widget.set_geometry(layout->visible);
  // Carry layout->source_row into the child's own raster/crop mapping.
  // Keep this child's identity and pixel-region ordering stable.
  render_pixel_regions(preview_widget);
}
```

The expected result refuses empty, stale, foreign, or non-block handles with a
`Warning`. A valid handle produces an explicit layout state:

- `NotLaidOut`: no current draw, or content/scroll/rect changed since it. There
  are no coordinates to reuse. Empty/no-op deltas do not invalidate layout.
- `Empty`: collapsed block or zero-size viewport. A collapsed block in a
  usable viewport has a zero-height allocation; an empty viewport has none.
- `Offscreen`: full allocation exists, but no visible rectangle. Skip rendering
  and decoding. TextBox itself does not build offscreen block fallback wraps.
- `Visible`: `visible` is the intersection with the final content viewport and
  the actual Screen. The scrollbar column is never part of it.

`allocation` describes the entire block in screen-relative **cells** before
clipping; its signed 64-bit `y` can lie far above the screen. `visible` is the
only driver/child `Rect`. `source_row` is the row inside the full block at its
visible top, including clipping at the screen's edge. Bottom clipping shortens
`visible.h` without shifting `source_row`. Horizontal screen clipping can be
derived from `visible.x - allocation.x` using widened arithmetic. A returned
snapshot is a value, not a live subscription: re-query after every draw.

For hit routing, use the child's newly assigned visible rect. Add `source_row`
to a hit's relative row when addressing the full content. Do not use advisory
`content_w()` or reconstruct TextBox's wraps and scroll offset. Drawing resolves
scrollbar appearance/disappearance before it exposes geometry, including a
width-only resize; a one-column viewport preserves content and omits the bar.

For pixels, either generate the visible slice or translate cell offsets through
the application's known image mapping into `ImagePlacementOptions::source`.
Never pass a cell row offset directly as a pixel offset unless that asset
explicitly uses one pixel per cell. Unsupported crops/placements retain the
authored cell Baseline through App's existing degradation channel. Persistent
children keep one stable `Widget*` and region index, so movement/cropping is
placement state rather than retransmitted content. App already handles accepted
write acknowledgements, refused-output retry, and modal image suspension.
Outside a modal, omitting an offscreen/retired child returns its resident pin
budget. TextBox neither collects nor owns these children automatically.

## Retention, budgets, and identity

Each block, including a collapsed one, counts as one logical entry under
`TextBoxRetention::max_entries` / `line_count()`. Every stored sanitized fallback
byte counts toward `max_bytes` / `retained_bytes()`, even if clipped or collapsed.
Blocks are finalized and eligible for oldest-first eviction; the live text tail
keeps its existing exemption. Successful insertion/update can immediately evict
the block itself under zero/tight retention. Its handle is then stale: success
means the operation was applied, not a promise of continued retention.

Independent hard admission budgets are `TextBoxBlockLimits`: defaults are 256
blocks, 4,096 rows per block and 65,536 aggregate reserved rows. Applications can
set explicit limits with `set_block_limits`; count/aggregate ceilings cannot
exceed `INT_MAX`. Zero is a real limit. Negative heights, exceeded budgets, and
overflow refuse with a `Warning` before changing fallback, counters, geometry,
or the live tail. Lowering limits below existing use also refuses; use retention
or explicit removal first. Row accounting does not allocate height-sized storage.

Handles combine a slot generation with a process-unique owner namespace. Slot
reuse, removal, retention eviction and `clear()` cannot make a stale handle valid
for different content. Handles do not transfer to copied/moved boxes; assignment
invalidates the destination namespace and moving invalidates the source too.
Keep the box at a stable address while consumers retain handles, and retain the
original insertion handle rather than manufacturing one from its fields.

## Viewport rules

At the bottom, appends, streaming updates and block height changes keep following
the bottom. While scrolled up, the anchor is a document entry and its source-row
offset. Earlier growth/retention and later streaming do not move that surviving
anchor. Shrinking an anchored block clamps to its last surviving row. Collapsing
it anchors the next content at its former start (subject to the viewport's end
clamp). Removing/evicting the anchored entry shows the oldest surviving content,
matching existing TextBox retention behavior. A resize reflows text and clamps
the anchored row. When the whole document fits, following the bottom resumes.

`test/80textboxblocks` covers admission/identity/retention failures, Unicode and
tiny/zero layouts, both clipping edges, anchors and width-only resize. Its fake
pixel child runs through the real App frame loop: refusal/retry, placement-only
scrolling, modal suspension/resume, offscreen retirement and clear. No real
terminal, decoder, network, or external media is needed.
