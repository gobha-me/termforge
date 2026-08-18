// Image region ops (#63): the pixels/dimensions invariant, Rect math, and the
// four clipped ops — sub, blit, blend, fill.
//
// All offline — these are pure value types, no TTY and no driver involved.

#include <catch2/catch_test_macros.hpp>

#include <climits>
#include <cstdint>
#include <utility>
#include <vector>

#include "detail/blend.hpp"
#include "support/image.hpp"
#include "termforge/core/types.hpp"

using termforge::Image;
using termforge::Pixel;
using termforge::Rect;
using tfsupport::checker;
using tfsupport::solid;

namespace {

constexpr Pixel kA{10, 20, 30, 255};
constexpr Pixel kB{200, 150, 100, 255};
constexpr Pixel kBg{5, 5, 5, 255};
constexpr Pixel kMark{255, 0, 255, 255};

// A placement, for the four-edge clipping tables.
struct Case4 {
  int dx, dy;
};

auto same(const Image& l, const Image& r) -> bool {
  if (l.width() != r.width() || l.height() != r.height()) return false;
  for (int y = 0; y < l.height(); ++y)
    for (int x = 0; x < l.width(); ++x)
      if (!(l.at(x, y) == r.at(x, y))) return false;
  return true;
}

} // namespace

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

TEST_CASE("Rect: contains still tests the half-open interior",
          "[image][rect]") {
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

TEST_CASE("Image: a buffer that disagrees with the dimensions yields empty",
          "[image][failure]") {
  // Short. Before #63 this constructed a 4x4 whose at(3,3) read past the end of
  // an 8-byte allocation, and the drivers' empty() guard did not catch it.
  const Image short_buf{4, 4, std::vector<Pixel>(2, Pixel{1, 2, 3, 4})};
  REQUIRE(short_buf.empty());
  REQUIRE(short_buf.width() == 0);
  REQUIRE(short_buf.height() == 0);
  REQUIRE(short_buf.pixels().empty());

  // Long.
  const Image long_buf{2, 2, std::vector<Pixel>(99, Pixel{9, 9, 9, 9})};
  REQUIRE(long_buf.empty());
  REQUIRE(long_buf.width() == 0);
  REQUIRE(long_buf.height() == 0);
}

TEST_CASE("Image: a mismatched buffer does not allocate to the dimensions",
          "[image][failure]") {
  // Collapsing rather than padding is what keeps this from being a 40 GB
  // request and a std::bad_alloc out of a constructor that cannot report one.
  // A caller's bad arithmetic must not be amplified into an allocation.
  const Image absurd{100000, 100000, std::vector<Pixel>(4)};
  REQUIRE(absurd.empty());
  REQUIRE(absurd.width() == 0);
  REQUIRE(absurd.height() == 0);
}

TEST_CASE("Image: a moved-from image is a valid empty image",
          "[image][failure]") {
  // The defaulted move would empty m_pixels and leave the dimensions alone, so
  // the moved-from image would claim a size it no longer has. The region ops
  // take their extent from the DIMENSIONS and never consult empty(), so the
  // next fill() on it wrote through a null data() pointer.
  Image src = solid(8, 8, kMark);

  const Image moved = std::move(src);
  REQUIRE(moved.width() == 8);
  REQUIRE(moved.height() == 8);

  REQUIRE(src.width() == 0); // NOLINT(bugprone-use-after-move)
  REQUIRE(src.height() == 0);
  REQUIRE(src.empty());
  REQUIRE(src.pixels().empty());
  // Each of these segfaulted before the move constructor zeroed the source.
  src.fill(Rect{0, 0, 8, 8}, kBg);
  src.blend(solid(2, 2, Pixel{1, 2, 3, 128}), 0, 0);
  src.blit(solid(2, 2, kMark), 0, 0);
  REQUIRE(src.sub(Rect{0, 0, 8, 8}).empty());
  REQUIRE(src.empty());

  // Move ASSIGNMENT has the same obligation.
  Image a = solid(4, 4, kMark);
  Image b;
  b = std::move(a);
  REQUIRE(b.width() == 4);
  REQUIRE(a.width() == 0); // NOLINT(bugprone-use-after-move)
  REQUIRE(a.empty());
  a.fill(Rect{0, 0, 4, 4}, kBg);
  REQUIRE(a.empty());
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

// ── sub ─────────────────────────────────────────────────────────────────────

TEST_CASE("Image: sub extracts an interior rect with the right pixels",
          "[image]") {
  const Image src = checker(4, 4, kA, kB);
  const Image got = src.sub(Rect{1, 1, 2, 2});
  REQUIRE(got.width() == 2);
  REQUIRE(got.height() == 2);
  // A checker, not a solid: a solid source cannot detect a row-stride bug.
  REQUIRE(got.at(0, 0) == src.at(1, 1));
  REQUIRE(got.at(1, 0) == src.at(2, 1));
  REQUIRE(got.at(0, 1) == src.at(1, 2));
  REQUIRE(got.at(1, 1) == src.at(2, 2));
}

TEST_CASE("Image: sub clipped at each edge returns the surviving overlap",
          "[image][failure]") {
  const Image src = checker(4, 4, kA, kB);

  const Image left = src.sub(Rect{-1, 0, 2, 2});
  REQUIRE(left.width() == 1);
  REQUIRE(left.height() == 2);
  REQUIRE(left.at(0, 0) == src.at(0, 0));
  REQUIRE(left.at(0, 1) == src.at(0, 1));

  const Image top = src.sub(Rect{0, -1, 2, 2});
  REQUIRE(top.width() == 2);
  REQUIRE(top.height() == 1);
  REQUIRE(top.at(0, 0) == src.at(0, 0));
  REQUIRE(top.at(1, 0) == src.at(1, 0));

  const Image right = src.sub(Rect{3, 0, 2, 2});
  REQUIRE(right.width() == 1);
  REQUIRE(right.height() == 2);
  REQUIRE(right.at(0, 0) == src.at(3, 0));

  const Image bottom = src.sub(Rect{0, 3, 2, 2});
  REQUIRE(bottom.width() == 2);
  REQUIRE(bottom.height() == 1);
  REQUIRE(bottom.at(0, 0) == src.at(0, 3));
}

TEST_CASE("Image: sub with an oversized rect returns only what exists",
          "[image][failure]") {
  const Image src = checker(4, 4, kA, kB);
  const Image got = src.sub(Rect{0, 0, 999, 999});
  REQUIRE(got.width() == 4);
  REQUIRE(got.height() == 4);
  REQUIRE(same(got, src));
}

TEST_CASE("Image: sub with a zero-size or fully-outside rect is empty",
          "[image][failure]") {
  const Image src = checker(4, 4, kA, kB);
  REQUIRE(src.sub(Rect{0, 0, 0, 0}).empty());
  REQUIRE(src.sub(Rect{1, 1, -3, 2}).empty());
  REQUIRE(src.sub(Rect{1, 1, 2, 0}).empty());
  REQUIRE(src.sub(Rect{99, 99, 4, 4}).empty());
  REQUIRE(src.sub(Rect{-9, 0, 4, 4}).empty());
}

TEST_CASE("Image: sub of an empty image is empty", "[image][failure]") {
  REQUIRE(Image{}.sub(Rect{0, 0, 4, 4}).empty());
}

// ── blit ────────────────────────────────────────────────────────────────────

TEST_CASE("Image: blit copies the source verbatim, alpha included", "[image]") {
  Image dst = solid(4, 4, Pixel{0, 0, 0, 255});
  const Image src = solid(2, 2, Pixel{9, 9, 9, 7});
  dst.blit(src, 1, 1);
  // blit is a COPY, not a composite: the source's a=7 lands as data.
  REQUIRE(dst.at(1, 1) == Pixel{9, 9, 9, 7});
  REQUIRE(dst.at(2, 2) == Pixel{9, 9, 9, 7});
  REQUIRE(dst.at(0, 0) == Pixel{0, 0, 0, 255});
  REQUIRE(dst.at(3, 3) == Pixel{0, 0, 0, 255});
}

TEST_CASE("Image: blit clips at each of the four edges", "[image][failure]") {
  const Image src = solid(2, 2, kMark);

  struct Case {
    int dx, dy, cx, cy; // placement, and the one cell that must be covered
  };
  const Case cases[] = {
      {-1, 1, 0, 1}, // clipped left
      {1, -1, 1, 0}, // clipped top
      {3, 1, 3, 1},  // clipped right
      {1, 3, 1, 3},  // clipped bottom
  };

  for (const auto& c : cases) {
    Image dst = solid(4, 4, kBg);
    dst.blit(src, c.dx, c.dy);
    REQUIRE(dst.at(c.cx, c.cy) == kMark);
    // Every cell outside the clipped placement is byte-identical to before.
    const Rect covered = Rect{c.dx, c.dy, 2, 2}.intersect(Rect{0, 0, 4, 4});
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x)
        if (!covered.contains(x, y)) REQUIRE(dst.at(x, y) == kBg);
  }
}

TEST_CASE("Image: blit fully outside the destination is a no-op",
          "[image][failure]") {
  const Image before = checker(4, 4, kA, kB);
  const Image src = solid(2, 2, kMark);
  for (const auto& [dx, dy] : std::vector<std::pair<int, int>>{
           {-100, 0}, {100, 0}, {0, -100}, {0, 100}, {-2, 0}, {4, 0}}) {
    Image dst = before;
    dst.blit(src, dx, dy);
    REQUIRE(same(dst, before));
  }
}

TEST_CASE("Image: blit clips origin and extent on all sides at once",
          "[image][failure]") {
  Image dst = solid(4, 4, kBg);
  const Image src = solid(8, 8, kMark);
  dst.blit(src, -2, -2);
  // The source is larger than the destination and starts outside it: both the
  // origin and the extent have to clip.
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x)
      REQUIRE(dst.at(x, y) == kMark);
}

TEST_CASE("Image: the blit source-rect overload matches slicing first",
          "[image]") {
  const Image atlas = checker(8, 4, kA, kB);
  const Rect frame{4, 1, 3, 2};

  Image via_rect = solid(6, 6, kBg);
  via_rect.blit(atlas, frame, 1, 1);

  Image via_sub = solid(6, 6, kBg);
  via_sub.blit(atlas.sub(frame), 1, 1);

  REQUIRE(same(via_rect, via_sub));
}

TEST_CASE("Image: a partially clipped source rect does not slide the paste",
          "[image][failure]") {
  // The source rect starts off the left of the atlas. Only its surviving part
  // is pasted, and it must land where that part would have landed -- shifted
  // right by the amount trimmed -- not at the raw destination origin.
  const Image atlas = checker(4, 4, kA, kB);
  Image dst = solid(6, 6, kBg);
  dst.blit(atlas, Rect{-2, 0, 4, 1}, 0, 0);
  REQUIRE(dst.at(0, 0) == kBg); // trimmed columns leave the dst untouched
  REQUIRE(dst.at(1, 0) == kBg);
  REQUIRE(dst.at(2, 0) == atlas.at(0, 0));
  REQUIRE(dst.at(3, 0) == atlas.at(1, 0));
  REQUIRE(dst.at(4, 0) == kBg);
}

TEST_CASE("Image: blit of an empty source is a no-op", "[image][failure]") {
  const Image before = checker(4, 4, kA, kB);
  Image dst = before;
  dst.blit(Image{}, 0, 0);
  REQUIRE(same(dst, before));
}

// ── blend ───────────────────────────────────────────────────────────────────
//
// The exact values here are the CONTRACT, not a snapshot. #90 replaces the
// scalar kernel with SIMD that must agree bit-for-bit, so changing any of them
// is a breaking change to that oracle.

TEST_CASE("Image: blend with an a=0 source is a no-op", "[image]") {
  Image dst = solid(2, 2, Pixel{10, 20, 30, 255});
  dst.blend(solid(2, 2, Pixel{255, 255, 255, 0}), 0, 0);
  REQUIRE(dst.at(0, 0) == Pixel{10, 20, 30, 255});
}

TEST_CASE("Image: blend with an a=255 source replaces the destination",
          "[image]") {
  Image dst = solid(2, 2, Pixel{10, 20, 30, 255});
  dst.blend(solid(2, 2, Pixel{1, 2, 3, 255}), 0, 0);
  REQUIRE(dst.at(0, 0) == Pixel{1, 2, 3, 255});

  Image translucent_dst = solid(2, 2, Pixel{10, 20, 30, 40});
  translucent_dst.blend(solid(2, 2, Pixel{1, 2, 3, 255}), 0, 0);
  REQUIRE(translucent_dst.at(0, 0) == Pixel{1, 2, 3, 255});
}

TEST_CASE("Image: blend midpoint over an opaque destination is exact",
          "[image]") {
  Image dst = solid(1, 1, Pixel{0, 0, 255, 255});
  dst.blend(solid(1, 1, Pixel{255, 0, 0, 128}), 0, 0);
  // R = div255(255*128 + 0*127)   = div255(32640) = 128
  // B = div255(0*128   + 255*127) = div255(32385) = 127
  REQUIRE(dst.at(0, 0) == Pixel{128, 0, 127, 255});

  Image grey = solid(1, 1, Pixel{0, 0, 0, 255});
  grey.blend(solid(1, 1, Pixel{255, 255, 255, 128}), 0, 0);
  REQUIRE(grey.at(0, 0) == Pixel{128, 128, 128, 255});
}

TEST_CASE("Image: blend over a translucent destination composites alpha",
          "[image]") {
  Image dst = solid(1, 1, Pixel{0, 0, 255, 128});
  dst.blend(solid(1, 1, Pixel{255, 0, 0, 128}), 0, 0);
  // dcw = 128*127 = 16256;  aos = 128*255 + 16256 = 48896;  a_o = div255 = 192
  // R   = round(255*128*255 / 48896) = round(170.2...) = 170
  // B   = round(255*16256   / 48896) = round(84.8...)  = 85
  // The 2:1 colour ratio matches the 128:64 alpha ratio, and 170 + 85 == 255.
  REQUIRE(dst.at(0, 0) == Pixel{170, 0, 85, 192});
}

TEST_CASE("Image: blend over a fully transparent destination yields the source",
          "[image]") {
  Image dst = solid(1, 1, Pixel{9, 9, 9, 0});
  const Pixel src{200, 100, 50, 128};
  dst.blend(solid(1, 1, src), 0, 0);
  // Nothing to composite against: the destination contributes no coverage, so
  // its colour must not leak in.
  REQUIRE(dst.at(0, 0) == src);
}

TEST_CASE("Image: blend's general path is correctly rounded Porter-Duff",
          "[image]") {
  // The general path must equal exact real-valued straight-alpha compositing
  // rounded to nearest ONCE, at the end:
  //   C_o = (C_s*a_s*255 + C_d*a_d*(255-a_s)) / (a_s*255 + a_d*(255-a_s))
  // Rounding the destination weight a_d*(1-a_s) to 8 bits before dividing by it
  // is off by up to 4/255, worst when that weight is tiny. This case is the
  // exhaustive argmax: it returned 99 before the weight was kept at 255x scale.
  const Pixel faint_src{0, 0, 0, 13};
  const Pixel faint_dst{243, 243, 243, 10};
  REQUIRE(termforge::detail::source_over(faint_src, faint_dst) ==
          Pixel{103, 103, 103, 22});

  // Exhaustive over both alphas for one colour pair, against the exact ratio in
  // 64-bit integers (rational, so this is the real-valued answer, not a float
  // approximation of it).
  for (int as = 1; as <= 254; ++as) {
    for (int ad = 1; ad <= 255; ++ad) {
      const auto inv = static_cast<std::uint64_t>(255 - as);
      const std::uint64_t den =
          static_cast<std::uint64_t>(as) * 255U + ad * inv;
      const auto exact = [&](std::uint64_t cs, std::uint64_t cd) {
        const std::uint64_t num = cs * static_cast<std::uint64_t>(as) * 255U +
                                  cd * static_cast<std::uint64_t>(ad) * inv;
        return static_cast<std::uint8_t>((2 * num + den) / (2 * den));
      };
      const Pixel s{0, 128, 255, static_cast<std::uint8_t>(as)};
      const Pixel d{243, 128, 7, static_cast<std::uint8_t>(ad)};
      const Pixel got = termforge::detail::source_over(s, d);
      REQUIRE(got.r == exact(s.r, d.r));
      REQUIRE(got.g == exact(s.g, d.g));
      REQUIRE(got.b == exact(s.b, d.b));
      // a_o is round((a_s*255 + a_d*(255-a_s))/255) either way.
      REQUIRE(got.a ==
              termforge::detail::div255(static_cast<std::uint32_t>(den)));
    }
  }
}

TEST_CASE("Image: blend's fast and general paths agree bit-exactly",
          "[image]") {
  // The seam that makes the opaque-destination fast path safe to keep forever.
  // #90's differential test reuses exactly this.
  const std::uint8_t colours[] = {0, 1, 63, 64, 127, 128, 191, 254, 255};
  for (int a = 0; a <= 255; ++a) {
    for (const std::uint8_t cs : colours) {
      for (const std::uint8_t cd : colours) {
        const Pixel s{cs, static_cast<std::uint8_t>(255 - cs), cs,
                      static_cast<std::uint8_t>(a)};
        const Pixel d{cd, cd, static_cast<std::uint8_t>(255 - cd), 255};
        REQUIRE(termforge::detail::source_over(s, d) ==
                termforge::detail::source_over_opaque_dst(s, d));
      }
    }
  }
}

TEST_CASE("Image: blend stays in range over every alpha pair", "[image]") {
  // 256x256 sweep of (a_s, a_d). a_o >= max(a_s, a_d) is the monotonicity that
  // catches a bad div255; nothing may exceed 255.
  for (int as = 0; as <= 255; ++as) {
    for (int ad = 0; ad <= 255; ++ad) {
      const Pixel s{255, 128, 0, static_cast<std::uint8_t>(as)};
      const Pixel d{0, 128, 255, static_cast<std::uint8_t>(ad)};
      const Pixel o = termforge::detail::source_over(s, d);
      REQUIRE(o.a >= (as > ad ? as : ad));
    }
  }
}

TEST_CASE("Image: blend clips at each of the four edges", "[image][failure]") {
  const Image src = solid(2, 2, Pixel{255, 255, 255, 255});
  const Case4 cases[] = {{-1, 1}, {1, -1}, {3, 1}, {1, 3}};
  for (const auto& c : cases) {
    Image dst = solid(4, 4, kBg);
    dst.blend(src, c.dx, c.dy);
    const Rect covered = Rect{c.dx, c.dy, 2, 2}.intersect(Rect{0, 0, 4, 4});
    REQUIRE_FALSE(covered.empty());
    for (int y = 0; y < 4; ++y) {
      for (int x = 0; x < 4; ++x) {
        const Pixel want =
            covered.contains(x, y) ? Pixel{255, 255, 255, 255} : kBg;
        REQUIRE(dst.at(x, y) == want);
      }
    }
  }
}

TEST_CASE("Image: blend fully outside the destination is a no-op",
          "[image][failure]") {
  const Image before = checker(4, 4, kA, kB);
  const Image src = solid(2, 2, Pixel{255, 255, 255, 200});
  for (const auto& [dx, dy] : std::vector<std::pair<int, int>>{
           {-100, 0}, {100, 0}, {0, -100}, {0, 100}, {-2, 0}, {4, 0}}) {
    Image dst = before;
    dst.blend(src, dx, dy);
    REQUIRE(same(dst, before));
  }
}

TEST_CASE("Image: blending into a transparent scratch then compositing agrees",
          "[image]") {
  // The layered pattern docs/map-widget.md commits the sprite tier to, and the
  // case an assume-the-destination-is-opaque blend would get wrong.
  const Image a = solid(2, 2, Pixel{255, 0, 0, 255});
  const Image b = solid(2, 2, Pixel{0, 0, 255, 128});

  Image direct = solid(2, 2, Pixel{0, 255, 0, 255});
  direct.blend(a, 0, 0);
  direct.blend(b, 0, 0);

  Image scratch = solid(2, 2, Pixel{0, 0, 0, 0});
  scratch.blend(a, 0, 0);
  scratch.blend(b, 0, 0);
  Image layered = solid(2, 2, Pixel{0, 255, 0, 255});
  layered.blend(scratch, 0, 0);

  REQUIRE(same(direct, layered));
}

// ── fill ────────────────────────────────────────────────────────────────────

TEST_CASE("Image: fill writes the pixel verbatim, alpha included", "[image]") {
  Image img = solid(4, 4, kBg);
  img.fill(Rect{1, 1, 2, 2}, Pixel{1, 2, 3, 4});
  REQUIRE(img.at(1, 1) == Pixel{1, 2, 3, 4});
  REQUIRE(img.at(2, 2) == Pixel{1, 2, 3, 4});
  REQUIRE(img.at(0, 0) == kBg);
  REQUIRE(img.at(3, 3) == kBg);
}

TEST_CASE("Image: fill clips at each edge and outside", "[image][failure]") {
  Image img = solid(4, 4, kBg);
  img.fill(Rect{-2, -2, 3, 3}, kMark);
  REQUIRE(img.at(0, 0) == kMark);
  REQUIRE(img.at(1, 0) == kBg);
  REQUIRE(img.at(0, 1) == kBg);

  const Image before = solid(4, 4, kBg);
  Image outside = before;
  outside.fill(Rect{99, 99, 4, 4}, kMark);
  REQUIRE(same(outside, before));
}

TEST_CASE("Image: fill with a zero or negative rect is a no-op",
          "[image][failure]") {
  const Image before = checker(4, 4, kA, kB);
  for (const Rect r : {Rect{0, 0, 0, 5}, Rect{0, 0, 5, 0}, Rect{0, 0, 5, -1},
                       Rect{0, 0, -1, 5}}) {
    Image img = before;
    img.fill(r, kMark);
    REQUIRE(same(img, before));
  }
}

TEST_CASE("Image: fill to transparent clears a region", "[image]") {
  Image scratch = solid(2, 2, Pixel{200, 100, 50, 255});
  scratch.fill(Rect{0, 0, 2, 2}, Pixel{0, 0, 0, 0});

  // Cleared to transparent means blending it over a background is a no-op --
  // which is fill and blend agreeing on the alpha convention.
  const Image before = solid(2, 2, kBg);
  Image dst = before;
  dst.blend(scratch, 0, 0);
  REQUIRE(same(dst, before));
}

// ── the invariant survives every op ─────────────────────────────────────────

// ── an image as its own source ───────────────────────────────────────────────
//
// Scrolling a framebuffer in place is the obvious use for the src_rect
// overloads. blit's per-row memcpy is UB on overlapping ranges (ASan reports
// memcpy-param-overlap), and blend's ascending loop would read pixels it had
// just written, smearing the row.

TEST_CASE("Image: blitting an image onto itself shifts without smearing",
          "[image][failure]") {
  // A 6x1 ramp shifted right by one. Reading through a copy is what makes the
  // overlapping ranges safe; a forward memcpy would smear 10 across the row.
  Image img{6, 1,
            std::vector<Pixel>{Pixel{10, 0, 0, 255}, Pixel{20, 0, 0, 255},
                               Pixel{30, 0, 0, 255}, Pixel{40, 0, 0, 255},
                               Pixel{50, 0, 0, 255}, Pixel{60, 0, 0, 255}}};
  img.blit(img, Rect{0, 0, 5, 1}, 1, 0);
  const std::uint8_t want[] = {10, 10, 20, 30, 40, 50};
  for (int x = 0; x < 6; ++x)
    REQUIRE(img.at(x, 0).r == want[x]);
}

TEST_CASE("Image: blitting an image onto itself shifts the other way too",
          "[image][failure]") {
  // The opposite direction, where a naive backward traversal would smear.
  Image img{6, 1,
            std::vector<Pixel>{Pixel{10, 0, 0, 255}, Pixel{20, 0, 0, 255},
                               Pixel{30, 0, 0, 255}, Pixel{40, 0, 0, 255},
                               Pixel{50, 0, 0, 255}, Pixel{60, 0, 0, 255}}};
  img.blit(img, Rect{1, 0, 5, 1}, 0, 0);
  const std::uint8_t want[] = {20, 30, 40, 50, 60, 60};
  for (int x = 0; x < 6; ++x)
    REQUIRE(img.at(x, 0).r == want[x]);
}

TEST_CASE("Image: self-blit matches going through an explicit copy",
          "[image][failure]") {
  // Rows as well as columns, on a checker so a stride error cannot hide.
  const Image before = checker(7, 5, kA, kB);
  for (const auto& [dx, dy] : std::vector<std::pair<int, int>>{
           {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {2, 2}, {-2, -2}, {3, -1}}) {
    Image in_place = before;
    in_place.blit(in_place, Rect{0, 0, 7, 5}, dx, dy);

    Image via_copy = before;
    via_copy.blit(before, Rect{0, 0, 7, 5}, dx, dy);

    REQUIRE(same(in_place, via_copy));
  }
}

TEST_CASE("Image: blending an image onto itself matches an explicit copy",
          "[image][failure]") {
  // Translucent, so the composite actually depends on what it reads.
  std::vector<Pixel> px;
  for (int i = 0; i < 6; ++i)
    px.push_back(Pixel{static_cast<std::uint8_t>(i * 40), 0, 0, 128});
  const Image before{6, 1, px};

  for (const int dx : {1, -1, 2, -2}) {
    Image in_place = before;
    in_place.blend(in_place, Rect{0, 0, 6, 1}, dx, 0);

    Image via_copy = before;
    via_copy.blend(before, Rect{0, 0, 6, 1}, dx, 0);

    REQUIRE(same(in_place, via_copy));
  }
}

// ── op-level overflow ────────────────────────────────────────────────────────

TEST_CASE("Image: a trimmed source rect with an extreme offset does not "
          "overflow",
          "[image][failure]") {
  // clip_placement shifts the paste origin by whatever the source clip trimmed
  // off the left/top. In int that is `INT_MAX + 4` — signed overflow, and the
  // exact adversarial input Rect::intersect went to int64 to survive. Run under
  // -fsanitize=undefined; that is where this case earns its keep.
  const Image src = solid(8, 8, kMark);
  const Image before = checker(4, 4, kA, kB);

  for (const auto& [dx, dy] :
       std::vector<std::pair<int, int>>{{INT_MAX, 0},
                                        {0, INT_MAX},
                                        {INT_MIN, 0},
                                        {0, INT_MIN},
                                        {INT_MAX, INT_MAX},
                                        {INT_MIN, INT_MIN}}) {
    Image dst = before;
    dst.blit(src, Rect{-4, -4, 8, 8}, dx, dy);
    dst.blend(src, Rect{-4, -4, 8, 8}, dx, dy);
    // Nothing can land on the destination from that far away.
    REQUIRE(same(dst, before));
  }
}

TEST_CASE("Rect: contains does not overflow near the coordinate limits",
          "[image][rect][failure]") {
  // x + w wraps negative in int, so a point genuinely inside reads as outside.
  const Rect huge{INT_MAX - 2, INT_MAX - 2, 4, 4};
  REQUIRE(huge.contains(INT_MAX - 2, INT_MAX - 2));
  REQUIRE(huge.contains(INT_MAX, INT_MAX));
  const Rect low{INT_MIN, INT_MIN, 4, 4};
  REQUIRE(low.contains(INT_MIN, INT_MIN));
  REQUIRE_FALSE(low.contains(INT_MIN + 4, INT_MIN));
}

TEST_CASE("Image: pixels() length always matches the dimensions", "[image]") {
  const auto check = [](const Image& i) {
    REQUIRE(i.pixels().size() == static_cast<std::size_t>(i.width()) *
                                     static_cast<std::size_t>(i.height()));
  };

  Image img = checker(5, 3, kA, kB);
  check(img);
  check(img.sub(Rect{1, 1, 2, 2}));
  check(img.sub(Rect{-4, -4, 2, 2}));
  check(img.sub(Rect{99, 99, 2, 2}));

  img.blit(solid(9, 9, kMark), -3, -3);
  check(img);
  img.blend(solid(2, 2, Pixel{1, 2, 3, 128}), 4, 2);
  check(img);
  img.fill(Rect{-1, -1, 99, 99}, kBg);
  check(img);
}
