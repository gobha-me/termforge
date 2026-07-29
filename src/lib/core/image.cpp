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
#include <cstring>
#include <utility>
#include <vector>

#include "detail/blend.hpp"
#include "termforge/core/types.hpp"

namespace termforge {

namespace {

// The clipped result of placing a sub-rect of a src of the given size at
// (dx, dy) in a dst of the given size: where to start reading, where to start
// writing, and how much survives.
//
// w/h are 0 when nothing survives, so every caller's loop simply runs zero
// times. That is the whole "clip, never throw" policy expressed once — all
// four ops route through here, so an off-by-one has exactly one place to live.
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
  const int px = dx + (s.x - src_rect.x);
  const int py = dy + (s.y - src_rect.y);
  const Rect v = Rect{px, py, s.w, s.h}.intersect(Rect{0, 0, dst_w, dst_h});
  if (v.empty()) return {};

  // 3. Map the surviving destination rect back into source coordinates.
  return Placement{s.x + (v.x - px), s.y + (v.y - py), v.x, v.y, v.w, v.h};
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
  const auto p = clip_placement(m_width, m_height, src.width(), src.height(),
                                src_rect, dx, dy);
  if (p.w == 0 || p.h == 0) return;

  for (int y = 0; y < p.h; ++y) {
    const auto srow = static_cast<std::size_t>(p.sy + y) *
                      static_cast<std::size_t>(src.width()) +
                      static_cast<std::size_t>(p.sx);
    const auto drow = static_cast<std::size_t>(p.dy + y) *
                      static_cast<std::size_t>(m_width) +
                      static_cast<std::size_t>(p.dx);
    // Pixel is trivially copyable and both rows are guaranteed to exist by the
    // constructor's size invariant.
    std::memcpy(m_pixels.data() + drow, src.pixels().data() + srow,
                static_cast<std::size_t>(p.w) * sizeof(Pixel));
  }
}

auto Image::blend(const Image& src, int dx, int dy) -> void {
  blend(src, Rect{0, 0, src.width(), src.height()}, dx, dy);
}

auto Image::blend(const Image& src, Rect src_rect, int dx, int dy) -> void {
  const auto p = clip_placement(m_width, m_height, src.width(), src.height(),
                                src_rect, dx, dy);
  if (p.w == 0 || p.h == 0) return;

  for (int y = 0; y < p.h; ++y) {
    const auto srow = static_cast<std::size_t>(p.sy + y) *
                      static_cast<std::size_t>(src.width()) +
                      static_cast<std::size_t>(p.sx);
    const auto drow = static_cast<std::size_t>(p.dy + y) *
                      static_cast<std::size_t>(m_width) +
                      static_cast<std::size_t>(p.dx);
    const Pixel* sp = src.pixels().data() + srow;
    Pixel* dp = m_pixels.data() + drow;
    for (int x = 0; x < p.w; ++x) dp[x] = detail::blend_pixel(sp[x], dp[x]);
  }
}

auto Image::fill(Rect r, Pixel p) -> void {
  const Rect v = r.intersect(Rect{0, 0, m_width, m_height});
  if (v.empty()) return;

  for (int y = 0; y < v.h; ++y) {
    const auto row = static_cast<std::size_t>(v.y + y) *
                     static_cast<std::size_t>(m_width) +
                     static_cast<std::size_t>(v.x);
    std::fill_n(m_pixels.begin() + static_cast<std::ptrdiff_t>(row), v.w, p);
  }
}

}  // namespace termforge
