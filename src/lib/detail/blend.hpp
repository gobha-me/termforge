#pragma once

// TermForge — source-over alpha compositing, the scalar reference.
//
// PRIVATE header (src/lib/). It must never be named from a public header:
// test/22headers compiles every public header against the public include path
// alone and fails the build the moment one reaches in here. The tests reach it
// because test/CMakeLists.txt puts src/lib on the suite include path.
//
// ── the convention ──────────────────────────────────────────────────────────
//
// STRAIGHT (non-premultiplied) alpha, Porter-Duff "over". Pixel::r/g/b are the
// colour at full opacity and Pixel::a is coverage; nothing is premultiplied on
// the way in or the way out. Only Image::blend composites — Image::blit and
// Image::fill copy alpha verbatim, because they are a copy and a clear.
//
//   a_o = a_s + a_d*(1 - a_s)
//   C_o = (C_s*a_s + C_d*a_d*(1 - a_s)) / a_o
//
// in integers over 0..255, rounding to nearest at every step.
//
// ── this is a permanent reference oracle ────────────────────────────────────
//
// #90 replaces the loop that calls source_over() with a SIMD kernel that must
// agree with it BIT-EXACTLY on every input. Every rounding decision below is
// therefore part of the contract, not an implementation detail. Do not "clean
// up" the rounding.

#include <cstdint>

#include "termforge/core/types.hpp"

namespace termforge::detail {

// Round-to-nearest division by 255, exact for x in [0, 65535].
//
// Chosen over (x + 127) / 255 and (x*257 + 257) >> 16 because it is the form
// that maps to two shifts and two adds per lane with no multiply and no
// divide — which is what #90's measured speedup rests on. There are no ties to
// break: 255 is odd, so x/255 is never exactly .5 for integer x.
[[nodiscard]] constexpr auto div255(std::uint32_t x) noexcept -> std::uint32_t {
  const std::uint32_t t = x + 128U;
  return (t + (t >> 8)) >> 8;
}

// Source-over when the destination is known opaque — the overwhelmingly common
// case (an opaque background, or a scene already composited). Division-free,
// and the shape a SIMD kernel wants.
//
// Identical output to source_over(s, d) for every input with d.a == 255; see
// the proof on source_over() below. The suite pins the agreement exhaustively.
[[nodiscard]] constexpr auto source_over_opaque_dst(Pixel s, Pixel d) noexcept
    -> Pixel {
  const std::uint32_t as = s.a;
  if (as == 0U) return d;
  if (as == 255U) return s;
  const std::uint32_t inv = 255U - as;
  return Pixel{static_cast<std::uint8_t>(div255(s.r * as + d.r * inv)),
               static_cast<std::uint8_t>(div255(s.g * as + d.g * inv)),
               static_cast<std::uint8_t>(div255(s.b * as + d.b * inv)), 255};
}

// Source-over, general form — correct for a translucent destination, which is
// what the layered pattern produces (blend several sprites into a transparent
// scratch Image, then blend the scratch onto the scene).
//
// The fast path above is a provable algebraic SPECIALIZATION of this, not a
// second rounding regime. With d.a == 255: dc = div255(255*inv), and div255 is
// exactly round(x/255) on this range while 255*inv/255 is exactly inv, so
// dc == inv and a_o == a_s + inv == 255. The channel below then computes
// (2*num + 255) / 510, which is round-half-up of num/255 — and since num/255
// is never exactly .5 for integer num (255 is odd), that is plain
// round(num/255), which is div255(num). So the two agree bit-for-bit and there
// is one oracle with no seam.
[[nodiscard]] constexpr auto source_over(Pixel s, Pixel d) noexcept -> Pixel {
  const std::uint32_t as = s.a;

  // Each early-out is likewise an exact specialization: substitute into the
  // general form and the divide cancels.
  if (as == 0U) return d;                 // a_o = a_d, C_o = C_d
  if (as == 255U || d.a == 0U) return s;  // a_o = a_s, C_o = C_s

  const std::uint32_t inv = 255U - as;         // 1 - a_s
  const std::uint32_t dc = div255(d.a * inv);  // a_d*(1 - a_s), rounded
  const std::uint32_t ao = as + dc;            // 1 <= ao <= 255

  // ao >= as >= 1 here, so the divide is always defined. Written as
  // (2n + ao) / (2*ao) so the round-half-up tie rule is explicit and does not
  // depend on ao's parity. num <= 255*255 + 255*255 = 130050, so 2*num + ao
  // fits in a u32 with room to spare.
  const auto chan = [ao, as, dc](std::uint32_t cs, std::uint32_t cd) {
    const std::uint32_t num = cs * as + cd * dc;
    return static_cast<std::uint8_t>((2U * num + ao) / (2U * ao));
  };

  return Pixel{chan(s.r, d.r), chan(s.g, d.g), chan(s.b, d.b),
               static_cast<std::uint8_t>(ao)};
}

// The one call the ops make. Picking per-pixel is fine for the scalar path;
// #90 hoists the choice to a whole vector of destination pixels at a time.
[[nodiscard]] constexpr auto blend_pixel(Pixel s, Pixel d) noexcept -> Pixel {
  return d.a == 255U ? source_over_opaque_dst(s, d) : source_over(s, d);
}

}  // namespace termforge::detail
