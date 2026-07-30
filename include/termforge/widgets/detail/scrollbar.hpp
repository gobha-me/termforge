#pragma once

// TermForge -- the shared scrollbar indicator (#21): one free function that
// draws a one-column track + thumb strip, so a ListWidget, a TableWidget and
// a TextBox in the same app answer "is there more, and where am I?" the same
// way. It is a FUNCTION, not a widget: it draws into a 1-column strip its
// caller carves out of its own rect, owns no state and handles no events --
// the widget keeps the (total, offset, visible) triple (detail/viewport.hpp)
// and routes any track clicks itself.
//
// Why a shared header (the #41 story, again): a thumb's geometry -- how tall,
// where it starts -- is a clamp-and-scale everyone writes identically and gets
// subtly wrong (integer division truncating toward 0, min-thumb rules, the
// content-fits case). One source, three callers, and the next scrollable
// widget gets the convention for free.
//
// Glyphs come from glyphs.hpp's scrollbar_glyphs() (Unicode │/█, ASCII |/#),
// which the caller resolves from its OWN BorderStyle knob -- this header
// deliberately takes the resolved ScrollGlyphs, not the enum, so it never
// re-derives a decision the widget already made. Both glyphs are one column
// wide, so a glyph survives FallbackDriver dropping colour -- the same bet
// the #72/#76 selection markers made, and the reason the strip can carry
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
#include "termforge/widgets/widget.hpp"  // Rect

namespace termforge::detail {

// Where the thumb sits inside the strip: [top row, height], both in rows
// relative to the track rect. `total`/`offset`/`visible` are the viewport
// triple in the library's uniform convention (detail/viewport.hpp: offset is
// rows scrolled past the top, increasing toward the end).
//
// Rules, in order:
//   - Nothing to scroll through (total <= visible, or no track) covers the
//     WHOLE strip: a bar in that state is only drawn to say "everything is
//     visible", and a partial thumb would falsely imply hidden content.
//   - The thumb is at least one row (a 10000-row list in a 10-row view must
//     still show one), and never taller than the track.
//   - The position is proportional: offset 0 pins the top, the maximum offset
//     (total - visible) pins the bottom, and the round half up keeps a tiny
//     scroll from sitting on the same row as no scroll.
// Callers clamp `offset` into range first (clamp_offset) -- this function
// trusts it, the way draw_scrollbar trusts its rect.
[[nodiscard]] constexpr auto thumb_window(int track_h, int total, int offset,
                                          int visible) noexcept
    -> std::pair<int, int> {
  if (track_h <= 0) return {0, 0};
  if (total <= 0) total = 0;
  if (visible < 0) visible = 0;
  if (total <= visible) return {0, track_h};

  // Height: the visible fraction of the content, rounded, min 1 row.
  int thumb_h =
      static_cast<int>((static_cast<long long>(track_h) * visible +
                        static_cast<long long>(total) / 2) /
                       total);
  thumb_h = std::clamp(thumb_h, 1, track_h);

  // Position: offset / max_offset of the way down the (track - thumb) run.
  const int max_offset = total - visible;
  const int run = track_h - thumb_h;
  int top = 0;
  if (max_offset > 0 && run > 0) {
    top = static_cast<int>((static_cast<long long>(offset) * run +
                            static_cast<long long>(max_offset) / 2) /
                           max_offset);
    top = std::clamp(top, 0, run);
  }
  return {top, thumb_h};
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
// The caller decides WHERE (convention: its rightmost column, only when
// total > visible) and re-paints it every frame -- immediate mode, like
// everything else on Screen.
inline auto draw_scrollbar(Screen& screen, Rect track, int total, int offset,
                           int visible, ScrollGlyphs glyphs, Rgb track_fg,
                           Rgb thumb_fg, Rgb bg) -> void {
  if (track.w <= 0 || track.h <= 0) return;
  const auto [top, thumb_h] = thumb_window(track.h, total, offset, visible);
  for (int row = 0; row < track.h; ++row) {
    const bool in_thumb = row >= top && row < top + thumb_h;
    screen.write_text(track.x, track.y + row,
                      in_thumb ? glyphs.thumb : glyphs.track,
                      in_thumb ? thumb_fg : track_fg, bg);
  }
}

}  // namespace termforge::detail
