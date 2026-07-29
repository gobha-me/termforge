// Image region ops (#63): the pixels/dimensions invariant, Rect math, and the
// four clipped ops — sub, blit, blend, fill.
//
// All offline — these are pure value types, no TTY and no driver involved.

#include <catch2/catch_test_macros.hpp>

#include <climits>
#include <vector>

#include "termforge/core/types.hpp"

using termforge::Image;
using termforge::Pixel;
using termforge::Rect;

// ── Rect ────────────────────────────────────────────────────────────────────
//
// intersect() is the one place the clipping arithmetic for sub/blit/blend/fill
// lives, so an off-by-one here is an off-by-one in all four.

TEST_CASE("Rect: intersect returns the overlap of two rects", "[image][rect]") {
  // Partial overlap, offset on both axes.
  REQUIRE(Rect{0, 0, 4, 4}.intersect(Rect{2, 2, 4, 4}) == Rect{2, 2, 2, 2});
  // Commutative.
  REQUIRE(Rect{2, 2, 4, 4}.intersect(Rect{0, 0, 4, 4}) == Rect{2, 2, 2, 2});
  // One fully inside the other, both ways round.
  REQUIRE(Rect{0, 0, 10, 10}.intersect(Rect{3, 4, 2, 2}) == Rect{3, 4, 2, 2});
  REQUIRE(Rect{3, 4, 2, 2}.intersect(Rect{0, 0, 10, 10}) == Rect{3, 4, 2, 2});
  // Identical.
  REQUIRE(Rect{1, 2, 3, 4}.intersect(Rect{1, 2, 3, 4}) == Rect{1, 2, 3, 4});
}

TEST_CASE("Rect: rects that only touch along an edge do not overlap",
          "[image][rect]") {
  // Half-open: {0,0,4,4} covers x 0..3, so x==4 belongs to the neighbour.
  REQUIRE(Rect{0, 0, 4, 4}.intersect(Rect{4, 0, 4, 4}).empty());
  REQUIRE(Rect{0, 0, 4, 4}.intersect(Rect{0, 4, 4, 4}).empty());
  // Fully disjoint.
  REQUIRE(Rect{0, 0, 4, 4}.intersect(Rect{99, 99, 4, 4}).empty());
  REQUIRE(Rect{0, 0, 4, 4}.intersect(Rect{-9, 0, 4, 4}).empty());
}

TEST_CASE("Rect: a degenerate or negative rect intersects to nothing",
          "[image][rect][failure]") {
  const Rect box{0, 0, 10, 10};
  REQUIRE(box.intersect(Rect{0, 0, 0, 5}).empty());
  REQUIRE(box.intersect(Rect{0, 0, 5, 0}).empty());
  REQUIRE(box.intersect(Rect{2, 2, -4, 4}).empty());
  REQUIRE(box.intersect(Rect{2, 2, 4, -4}).empty());
  // A miss returns a default Rect, not merely something with w or h <= 0.
  REQUIRE(box.intersect(Rect{0, 0, 0, 5}) == Rect{});
}

TEST_CASE("Rect: empty() tracks non-positive extents", "[image][rect]") {
  REQUIRE(Rect{}.empty());
  REQUIRE(Rect{5, 5, 0, 3}.empty());
  REQUIRE(Rect{5, 5, 3, 0}.empty());
  REQUIRE(Rect{5, 5, -1, 3}.empty());
  REQUIRE_FALSE(Rect{5, 5, 1, 1}.empty());
}

TEST_CASE("Rect: intersect does not overflow on extreme coordinates",
          "[image][rect][failure]") {
  // x + w overflows int here; the i64 arithmetic is what keeps this defined.
  // Run under -fsanitize=undefined — that is where this case earns its keep.
  const Rect huge{INT_MAX - 2, INT_MAX - 2, 4, 4};
  REQUIRE(huge.intersect(Rect{0, 0, 4, 4}).empty());
  // Self-intersection is the identity even though x + w is past INT_MAX: the
  // result is bounded by both inputs, so its EXTENT always fits back in int
  // even when its far edge does not.
  REQUIRE(huge.intersect(huge) == huge);
  const Rect low{INT_MIN, INT_MIN, 4, 4};
  REQUIRE(low.intersect(Rect{0, 0, 4, 4}).empty());
}

TEST_CASE("Rect: contains still tests the half-open interior", "[image][rect]") {
  const Rect r{2, 3, 4, 5};
  REQUIRE(r.contains(2, 3));
  REQUIRE(r.contains(5, 7));
  REQUIRE_FALSE(r.contains(6, 7));
  REQUIRE_FALSE(r.contains(5, 8));
  REQUIRE_FALSE(r.contains(1, 3));
}

// ── the pixels/dimensions invariant ─────────────────────────────────────────
//
// at() and the kitty transmit path both derive their extent from the
// *dimensions*, never from the vector, so a buffer that disagrees with them is
// an out-of-bounds read. The constructor is the only place the invariant can be
// established. These cases are the ones that read OOB before #63 — run the
// suite under cmake/toolchain/address.cmake and they are where ASan earns its
// keep.

TEST_CASE("Image: a short pixel buffer is padded to width*height",
          "[image][failure]") {
  const Image img{4, 4, std::vector<Pixel>(2, Pixel{1, 2, 3, 4})};
  REQUIRE(img.width() == 4);
  REQUIRE(img.height() == 4);
  REQUIRE_FALSE(img.empty());
  // The whole buffer is addressable — this read is the one that was OOB.
  REQUIRE(img.at(3, 3) == Pixel{});
  REQUIRE(img.at(0, 0) == Pixel{1, 2, 3, 4});
}

TEST_CASE("Image: a long pixel buffer is truncated to width*height",
          "[image][failure]") {
  const Image img{2, 2, std::vector<Pixel>(99, Pixel{9, 9, 9, 9})};
  REQUIRE(img.width() == 2);
  REQUIRE(img.height() == 2);
  REQUIRE(img.at(1, 1) == Pixel{9, 9, 9, 9});
}

TEST_CASE("Image: a non-positive dimension collapses the image to empty",
          "[image][failure]") {
  const std::vector<Pixel> px(16, Pixel{1, 1, 1, 255});

  const Image neg_w{-1, 4, px};
  REQUIRE(neg_w.empty());
  REQUIRE(neg_w.width() == 0);
  REQUIRE(neg_w.height() == 0);

  const Image zero_h{4, 0, px};
  REQUIRE(zero_h.empty());
  REQUIRE(zero_h.width() == 0);
  REQUIRE(zero_h.height() == 0);

  const Image both{-3, -3, px};
  REQUIRE(both.empty());
  REQUIRE(both.width() == 0);
  REQUIRE(both.height() == 0);
}

TEST_CASE("Image: an exactly-sized buffer is left alone", "[image]") {
  std::vector<Pixel> px;
  for (int i = 0; i < 6; ++i)
    px.push_back(Pixel{static_cast<std::uint8_t>(i), 0, 0, 255});
  const Image img{3, 2, px};
  REQUIRE(img.width() == 3);
  REQUIRE(img.height() == 2);
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 3; ++x)
      REQUIRE(img.at(x, y) ==
              Pixel{static_cast<std::uint8_t>(y * 3 + x), 0, 0, 255});
}

TEST_CASE("Image: a default-constructed image is empty", "[image]") {
  const Image img;
  REQUIRE(img.empty());
  REQUIRE(img.width() == 0);
  REQUIRE(img.height() == 0);
}
