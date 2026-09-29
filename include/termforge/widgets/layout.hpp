#pragma once

// Pure cell-rectangle composition. These functions return geometry only:
// callers still own every Widget, draw order, clipping and input routing.

#include <expected>
#include <span>
#include <vector>

#include "termforge/core/types.hpp"

namespace termforge {

struct LayoutTrack {
  int minimum{0};
  int preferred{0};
  int grow_weight{1};
};

// Reserve spacing between tracks, shrink trailing preferred sizes toward their
// minima if necessary, then share surplus among positive grow weights. Integer
// remainder cells go to later positive-weight tracks first. With no positive
// weights, surplus stays as trailing slack. Both axes stretch to bounds on the
// cross axis. An impossible or invalid request returns a Warning, not a partial
// layout. Empty bounds produce empty rectangles at the bounds origin.
[[nodiscard]] auto layout_row(Rect bounds, std::span<const LayoutTrack> tracks,
                              int spacing = 0)
    -> std::expected<std::vector<Rect>, ErrorEvent>;

[[nodiscard]] auto layout_column(Rect bounds,
                                 std::span<const LayoutTrack> tracks,
                                 int spacing = 0)
    -> std::expected<std::vector<Rect>, ErrorEvent>;

} // namespace termforge
