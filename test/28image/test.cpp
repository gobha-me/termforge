// Image region ops (#63): the pixels/dimensions invariant, Rect math, and the
// four clipped ops — sub, blit, blend, fill.
//
// All offline — these are pure value types, no TTY and no driver involved.

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "termforge/core/types.hpp"

using termforge::Image;
using termforge::Pixel;

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
