#pragma once

// TermForge — nearest-neighbour sampling for the cell-rect image path (#83).
//
// PRIVATE header (src/lib/). It must never be named from a public header:
// test/22headers compiles every public header against the public include path
// alone and fails the build the moment one reaches in here.
//
// ── why this exists ─────────────────────────────────────────────────────────
//
// draw_image names a destination *cell* rect; the image arrives at whatever
// pixel resolution its author chose. KittyDriver hands that mismatch to the
// terminal (c=/r= scale the placement, zero CPU). The AnsiRgb and Fallback
// tiers have no such lever — they build the output character by character, so
// they resample here.
//
// Stretch-to-fill, nearest neighbour, and nothing else. Letterboxing is a
// border policy and resampling quality is a filter library; both are out of
// scope, deliberately, for the same reason Image::sub returns the clipped
// overlap rather than padding it back out.
//
// ── the index map ───────────────────────────────────────────────────────────
//
//   src = i * src_dim / dst_dim        (integer division, truncating)
//
// The integer form is the point. A float map (i * src/dst rounded) drifts:
// the same nominal ratio lands on a different source index depending on how
// the compiler contracts the multiply-add, so the two sanitizer builds and the
// two compilers can disagree about a pixel. This form is exact, is monotone in
// i, and — the property the existing suite depends on — reduces to `src == i`
// whenever src_dim == dst_dim, so every 1:1 expectation written before #83
// still holds byte for byte.
//
// The product is int64: src_dim and dst_dim are both bounded by image and
// terminal geometry in practice, but i * src_dim overflows int at dimensions
// an adversarial Image can reach (both are ints from a public API), and a
// wrapped index would read outside the buffer.

#include <cstdint>

namespace termforge::detail {

// Map destination index `i` in [0, dst_dim) to a source index in
// [0, src_dim). Returns 0 for degenerate inputs rather than dividing by zero;
// callers guard on empty images, so this is belt-and-braces.
[[nodiscard]] constexpr auto sample_index(int i, int src_dim,
                                          int dst_dim) noexcept -> int {
  if (dst_dim <= 0 || src_dim <= 0) return 0;
  const auto s = (std::int64_t{i} * src_dim) / dst_dim;
  // i < dst_dim by contract, so s < src_dim; clamp anyway — a caller that
  // passes i == dst_dim would otherwise index one past the last row.
  if (s < 0) return 0;
  if (s >= src_dim) return src_dim - 1;
  return static_cast<int>(s);
}

}  // namespace termforge::detail
