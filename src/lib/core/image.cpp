// TermForge — Image region ops (#63): sub, blit, blend, fill.
//
// Out of line rather than inline in types.hpp for two reasons. types.hpp is
// transitively included by nearly every TU, and these are loop bodies. And
// blend() is the function #90 replaces with a SIMD kernel dispatched from
// src/lib/detail/ — a private path that a public header may not name, because
// test/22headers fails the build the moment one does. Inlining blend() today
// buys a churn commit later on the exact function whose bit-exactness this
// issue is freezing.

#include <algorithm>
#include <climits>
#include <cstdint>
#include <utility>
#include <vector>

#include "detail/blend.hpp"
#include "detail/simd.hpp"
#include "termforge/core/types.hpp"

namespace termforge {

namespace {

// The clipped result of placing a sub-rect of a src of the given size at
// (dx, dy) in a dst of the given size: where to start reading, where to start
// writing, and how much survives.
//
// w/h are 0 when nothing survives, so every caller's loop simply runs zero
// times. That is the "clip, never throw" policy expressed once for the two
// two-buffer ops. (sub and fill need only a single intersect against the
// image's own bounds and call Rect::intersect directly.)
struct Placement {
  int sx{0}, sy{0};  // origin in src
  int dx{0}, dy{0};  // origin in dst
  int w{0}, h{0};
};

[[nodiscard]] auto clip_placement(int dst_w, int dst_h, int src_w, int src_h,
                                  Rect src_rect, int dx, int dy) -> Placement {
  // 1. The requested source rect, clipped to what the source actually has.
  const Rect s = src_rect.intersect(Rect{0, 0, src_w, src_h});
  if (s.empty()) return {};

  // 2. Where that lands in the destination. The paste origin shifts by however
  //    much step 1 trimmed off the source's left/top edge — miss this and a
  //    partially-clipped source silently slides.
  //
  //    In int64: a trimmed src_rect plus a large dx is exactly the adversarial
  //    `x + w` that Rect::intersect widened to int64 to survive, and computing
  //    the shifted origin in int here would throw that guarantee away one line
  //    later. The values are then back inside int range before the cast, since
  //    intersect() bounds them by the destination.
  //    The clip is spelled out in i64 rather than handed to Rect::intersect
  //    because the shifted origin can itself be outside int range, and there is
  //    no way to put it in a Rect to be intersected without narrowing first —
  //    which is the very thing being avoided.
  using i64 = std::int64_t;
  const i64 px = i64{dx} + (i64{s.x} - src_rect.x);
  const i64 py = i64{dy} + (i64{s.y} - src_rect.y);

  const i64 x0 = px > 0 ? px : 0;
  const i64 y0 = py > 0 ? py : 0;
  const i64 x1 = px + s.w < dst_w ? px + s.w : dst_w;
  const i64 y1 = py + s.h < dst_h ? py + s.h : dst_h;
  if (x1 <= x0 || y1 <= y0) return {};

  // 3. Map the surviving destination rect back into source coordinates. Every
  //    value below is bounded by the destination's dimensions, so all of it
  //    fits back into int.
  return Placement{static_cast<int>(s.x + (x0 - px)),
                   static_cast<int>(s.y + (y0 - py)), static_cast<int>(x0),
                   static_cast<int>(y0), static_cast<int>(x1 - x0),
                   static_cast<int>(y1 - y0)};
}

// Which op a self-directed call is standing in for.
enum class Op { Blit, Blend };

// img.blit(img, …) and img.blend(img, …) — an image onto itself, which is how a
// caller scrolls or shifts a framebuffer in place. Both need handling: blit's
// per-row std::memcpy is undefined on overlapping ranges, and blend's ascending
// loop would read destination pixels it had already written, smearing the row.
//
// Resolved by reading through a clipped copy of the source region rather than by
// choosing a safe traversal order. Direction-picking is the faster answer, but
// it has to get row order and column order right independently and is silently
// wrong when it does not; this is one branch, obviously correct, and it only
// allocates on a path that produced garbage before. The copy is of the CLIPPED
// region, so the paste origin must then shift by whatever the clip trimmed off
// the left/top — the same correction clip_placement makes.
auto self_op(Image& img, Rect src_rect, int dx, int dy, Op op) -> void {
  const Image copy = img.sub(src_rect);
  if (copy.empty()) return;

  const Rect s =
      src_rect.intersect(Rect{0, 0, img.width(), img.height()});
  using i64 = std::int64_t;
  const i64 sx = i64{dx} + (i64{s.x} - src_rect.x);
  const i64 sy = i64{dy} + (i64{s.y} - src_rect.y);
  // Clamped into int range; anything past it cannot land on the image anyway,
  // and the ops clip whatever does not.
  const auto clamp = [](i64 v) {
    if (v > INT_MAX) return INT_MAX;
    if (v < INT_MIN) return INT_MIN;
    return static_cast<int>(v);
  };
  const Rect whole{0, 0, copy.width(), copy.height()};
  if (op == Op::Blit) {
    img.blit(copy, whole, clamp(sx), clamp(sy));
  } else {
    img.blend(copy, whole, clamp(sx), clamp(sy));
  }
}

}  // namespace

auto Image::sub(Rect r) const -> Image {
  const Rect v = r.intersect(Rect{0, 0, m_width, m_height});
  if (v.empty()) return Image{};

  std::vector<Pixel> out;
  out.reserve(static_cast<std::size_t>(v.w) * static_cast<std::size_t>(v.h));
  for (int y = 0; y < v.h; ++y) {
    const auto row = static_cast<std::size_t>(v.y + y) *
                     static_cast<std::size_t>(m_width) +
                     static_cast<std::size_t>(v.x);
    out.insert(out.end(), m_pixels.begin() + static_cast<std::ptrdiff_t>(row),
               m_pixels.begin() + static_cast<std::ptrdiff_t>(row) + v.w);
  }
  return Image{v.w, v.h, std::move(out)};
}

auto Image::blit(const Image& src, int dx, int dy) -> void {
  blit(src, Rect{0, 0, src.width(), src.height()}, dx, dy);
}

auto Image::blit(const Image& src, Rect src_rect, int dx, int dy) -> void {
  if (&src == this) return self_op(*this, src_rect, dx, dy, Op::Blit);

  const auto p = clip_placement(m_width, m_height, src.width(), src.height(),
                                src_rect, dx, dy);
  if (p.w == 0 || p.h == 0) return;
  const auto width = static_cast<std::size_t>(p.w);

  for (int y = 0; y < p.h; ++y) {
    const auto srow = static_cast<std::size_t>(p.sy + y) *
                      static_cast<std::size_t>(src.width()) +
                      static_cast<std::size_t>(p.sx);
    const auto drow = static_cast<std::size_t>(p.dy + y) *
                      static_cast<std::size_t>(m_width) +
                      static_cast<std::size_t>(p.dx);
    detail::copy_pixels(src.pixels().subspan(srow, width),
                        pixels().subspan(drow, width));
  }
}

auto Image::blend(const Image& src, int dx, int dy) -> void {
  blend(src, Rect{0, 0, src.width(), src.height()}, dx, dy);
}

auto Image::blend(const Image& src, Rect src_rect, int dx, int dy) -> void {
  if (&src == this) return self_op(*this, src_rect, dx, dy, Op::Blend);

  const auto p = clip_placement(m_width, m_height, src.width(), src.height(),
                                src_rect, dx, dy);
  if (p.w == 0 || p.h == 0) return;
  const auto width = static_cast<std::size_t>(p.w);

  for (int y = 0; y < p.h; ++y) {
    const auto srow = static_cast<std::size_t>(p.sy + y) *
                      static_cast<std::size_t>(src.width()) +
                      static_cast<std::size_t>(p.sx);
    const auto drow = static_cast<std::size_t>(p.dy + y) *
                      static_cast<std::size_t>(m_width) +
                      static_cast<std::size_t>(p.dx);
    detail::blend_pixels(src.pixels().subspan(srow, width),
                         pixels().subspan(drow, width));
  }
}

auto Image::fill(Rect r, Pixel p) -> void {
  const Rect v = r.intersect(Rect{0, 0, m_width, m_height});
  if (v.empty()) return;
  const auto width = static_cast<std::size_t>(v.w);

  for (int y = 0; y < v.h; ++y) {
    const auto row = static_cast<std::size_t>(v.y + y) *
                     static_cast<std::size_t>(m_width) +
                     static_cast<std::size_t>(v.x);
    detail::fill_pixels(pixels().subspan(row, width), p);
  }
}

}  // namespace termforge
