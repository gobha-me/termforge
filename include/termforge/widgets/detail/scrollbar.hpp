#pragma once

// TermForge -- the shared scrollbar indicator (#21 / #131): one free function
// that draws a track + thumb strip along either axis, so a ListWidget, a
// TableWidget, a TextBox and a two-row TabBar answer "is there more, and where
// am I?" the same way. It is a FUNCTION, not a widget: it draws into a strip
// its caller carves out of its own rect, owns no state and handles no events --
// the widget keeps the (total, offset, visible) triple and routes any track
// clicks itself.
//
// Why a shared header (the #41 story, again): a thumb's geometry -- how long,
// where it starts -- is a clamp-and-scale everyone writes identically and gets
// subtly wrong (integer division truncating toward 0, min-thumb rules, the
// content-fits case). One source, every caller, and the next scrollable widget
// gets the convention for free.
//
// Orientation (#131): thumb_window is pure arithmetic over a track LENGTH and
// content UNITS -- rows for a vertical strip, columns for a horizontal one,
// or any other unit the caller invents (TabBar feeds cumulative title columns,
// not tab indices). draw_scrollbar paints along track.y+row or track.x+col.
// Vertical remains the default so List/Table/TextBox call sites stay unchanged.
//
// Glyphs come from glyphs.hpp's scrollbar_glyphs() (Unicode │/█ or ─/█, ASCII
// |/# or -/#), which the caller resolves from its OWN BorderStyle knob -- this
// header deliberately takes the resolved ScrollGlyphs, not the enum, so it
// never re-derives a decision the widget already made. Both glyphs are one
// column wide, so a glyph survives FallbackDriver dropping colour -- the same
// bet the #72/#76 selection markers made, and the reason the strip can carry
// colour at all.
//
// PUBLIC (include/termforge/widgets/detail/), like scroll.hpp and
// viewport.hpp: a future public consumer may not include a private header
// (#54, test/22headers fails the BUILD on it).
//
// Everything here is pure: no widget state, no dirty flags -- callers
// mark_dirty as they already do.

#include <algorithm>

#include "termforge/core/screen.hpp"
#include "termforge/widgets/detail/width.hpp"
#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/widget.hpp" // Rect

namespace termforge::detail {

// Where the thumb sits inside the strip: [start, length], both in cells along
// the track relative to the track rect. `total`/`offset`/`visible` are CONTENT
// UNITS -- whatever the caller measures (rows, columns, title-column totals).
// They are deliberately NOT item indices: a variable-width strip cannot feed
// `count - visible` and get a meaningful ceiling (#131 / TabBar).
//
// Rules, in order:
//   - Nothing to scroll through (total <= visible, or no track) covers the
//     WHOLE strip: a bar in that state is only drawn to say "everything is
//     visible", and a partial thumb would falsely imply hidden content.
//   - The thumb is at least one cell (a 10000-unit list in a 10-cell track must
//     still show one), and never longer than the track.
//   - The position is proportional: offset 0 pins the start, the maximum offset
//     (total - visible) pins the end, and the round half up keeps a tiny
//     scroll from sitting on the same cell as no scroll.
// Callers clamp `offset` into range first -- this function trusts it, the way
// draw_scrollbar trusts its rect.
[[nodiscard]] constexpr auto thumb_window(int track_len, int total, int offset,
                                          int visible) noexcept
    -> std::pair<int, int> {
  if (track_len <= 0) return {0, 0};
  if (total <= 0) total = 0;
  if (visible < 0) visible = 0;
  if (total <= visible) return {0, track_len};

  // Length: the visible fraction of the content, rounded, min 1 cell.
  int thumb_len =
      static_cast<int>((static_cast<long long>(track_len) * visible +
                        static_cast<long long>(total) / 2) /
                       total);
  thumb_len = std::clamp(thumb_len, 1, track_len);

  // Position: offset / max_offset of the way along the (track - thumb) run.
  const int max_offset = total - visible;
  const int run = track_len - thumb_len;
  int start = 0;
  if (max_offset > 0 && run > 0) {
    start = static_cast<int>((static_cast<long long>(offset) * run +
                              static_cast<long long>(max_offset) / 2) /
                             max_offset);
    start = std::clamp(start, 0, run);
  }
  return {start, thumb_len};
}

// Paint the whole strip: track glyph everywhere, thumb glyph over its window.
// Painting the track cells too -- not just the thumb -- is what erases a
// SHRINKING thumb: callers fill their rect with their own background colour
// first, so a track painted only where the thumb isn't would leave last
// frame's longer thumb behind as stale cells. bg is the caller's row
// background, so the strip reads as cut out of the widget, not painted over
// it; the thumb takes thumb_fg (the caller's selection/highlight colour) so
// the position marker is the thing that pops, matching how the selection
// marker uses the same inversion.
//
// The caller decides WHERE (convention: rightmost column for Vertical, a
// dedicated bottom row for Horizontal) and re-paints it every frame --
// immediate mode, like everything else on Screen. Orientation defaults to
// Vertical so every #21 call site stays source-compatible; ScrollOrientation
// lives next to ScrollGlyphs in glyphs.hpp because the glyph table is what
// actually branches on it.
inline auto draw_scrollbar(
    Screen& screen, Rect track, int total, int offset, int visible,
    ScrollGlyphs glyphs, Rgb track_fg, Rgb thumb_fg, Rgb bg,
    ScrollOrientation orient = ScrollOrientation::Vertical) -> void {
  if (track.w <= 0 || track.h <= 0) return;
  const int track_len =
      orient == ScrollOrientation::Horizontal ? track.w : track.h;
  const auto [start, thumb_len] =
      thumb_window(track_len, total, offset, visible);
  if (orient == ScrollOrientation::Horizontal) {
    for (int col = 0; col < track.w; ++col) {
      const bool in_thumb = col >= start && col < start + thumb_len;
      screen.write_text(track.x + col, track.y,
                        in_thumb ? glyphs.thumb : glyphs.track,
                        in_thumb ? thumb_fg : track_fg, bg);
    }
    return;
  }
  for (int row = 0; row < track.h; ++row) {
    const bool in_thumb = row >= start && row < start + thumb_len;
    screen.write_text(track.x, track.y + row,
                      in_thumb ? glyphs.thumb : glyphs.track,
                      in_thumb ? thumb_fg : track_fg, bg);
  }
}

} // namespace termforge::detail
