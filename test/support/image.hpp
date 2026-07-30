#pragma once

// TermForge test support — Image builders.
//
// solid() was written twice (test/01drivers as make_image, test/28image), and
// #90's differential suite wants a third copy. checker() existed once, in the
// suite that needs it *least*: 28image's region ops. The driver suite, which
// renders row by row through three tiers, only ever fed them solids — so a
// row-stride or half-block-pairing error there was undetectable by
// construction (#101). Both live here now; the drivers use checker().
//
// Everything is inline and in namespace tfsupport so suites can dump it in
// an anonymous namespace without ODR worries.

#include <cstddef>
#include <utility>
#include <vector>

#include "termforge/core/types.hpp"

namespace tfsupport {

using termforge::Image;
using termforge::Pixel;

// Both builders size their buffer from the same w*h they pass as dimensions,
// so they satisfy the #63 invariant by construction and can never produce the
// collapsed-to-empty Image that types.hpp:180-191 yields on a mismatch. That
// is the events.hpp discipline (a builder must emit what the real thing
// emits) transposed to this domain: a test image is never a degenerate one by
// accident.
inline auto solid(int w, int h, Pixel p) -> Image {
  return Image{w, h,
               std::vector<Pixel>(
                   static_cast<std::size_t>(w) * static_cast<std::size_t>(h),
                   p)};
}

// Alternating pixels. A solid source cannot detect a row-stride bug — every
// wrong pixel looks like every right one — so the region ops and the drivers
// are tested against this.
//
// The parity is (x + y) % 2 == 0 -> a, filled row-major, and that is
// load-bearing: test/01drivers hard-codes the exact bytes each driver emits
// for a checker, down to a base64 payload. Changing either flips those
// expectations, which is the intended failure and not a reason to relax them.
inline auto checker(int w, int h, Pixel a, Pixel b) -> Image {
  std::vector<Pixel> px;
  px.reserve(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) px.push_back((x + y) % 2 == 0 ? a : b);
  return Image{w, h, std::move(px)};
}

}  // namespace tfsupport
