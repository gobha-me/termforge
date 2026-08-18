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
// in integers over 0..255, rounding to nearest ONCE — at the end. Both paths
// below are correctly rounded: they equal the exact rational result rounded to
// nearest (half-up), on every one of the 256^4 inputs. Intermediate weights are
// never truncated to 8 bits, because dividing by a rounded weight is what
// costs precision when the weight is small.
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
// second rounding regime. With d.a == 255: dcw = 255*inv, so
// aos = as*255 + 255*inv = 255*(as + inv) = 255*255 = 65025, and
// a_o = div255(65025) = 255. The channel numerator is likewise a multiple of
// 255: num = 255*(cs*as + cd*inv) = 255*n. So the channel computes
// (2*255*n + 65025) / 130050 = 255*(2n + 255) / (255*510) = (2n + 255)/510,
// which is round-half-up of n/255 — and since n/255 is never exactly .5 for
// integer n (255 is odd), that is plain round(n/255), which is div255(n). So
// the two agree bit-for-bit and there is one oracle with no seam.
[[nodiscard]] constexpr auto source_over(Pixel s, Pixel d) noexcept -> Pixel {
  const std::uint32_t as = s.a;

  // Each early-out is likewise an exact specialization: substitute into the
  // general form and the divide cancels.
  if (as == 0U) return d;                // a_o = a_d, C_o = C_d
  if (as == 255U || d.a == 0U) return s; // a_o = a_s, C_o = C_s

  // Both weights are kept at 255x scale through the ratio. Rounding a_d*(1-a_s)
  // to 8 bits FIRST and then dividing by it costs up to 4/255 per channel,
  // because the weight can be as small as 1 and a half-LSB error in a weight of
  // 1 is a 50% error in the ratio. Exhaustively: the 8-bit-weight form differs
  // from correctly-rounded Porter-Duff on 8.13% of the 256^4 inputs, max 4;
  // this form is exact on all of them. a_o is unaffected either way.
  const std::uint32_t inv = 255U - as; // (1 - a_s) at 255x
  const std::uint32_t asw = as * 255U; // a_s at 65025x
  const std::uint32_t dcw = d.a * inv; // a_d*(1 - a_s) at 65025x
  const std::uint32_t aos = asw + dcw; // a_o at 65025x, 255 <= aos <= 65025

  // Same multiply count per channel as weighting by an 8-bit dc; the scale-up
  // hoists out. aos >= asw >= 255, so the divide is always defined. Written as
  // (2n + aos) / (2*aos) so the round-half-up tie rule is explicit and does not
  // depend on parity. num <= 255*aos <= 16581375, so 2*num + aos <= 33227775 —
  // a u32 with 129x headroom, and num is under 2^24, so a #90 kernel may do
  // this divide in f32 and still match bit-exactly (verified exhaustively over
  // all 256^4; note lrintf must NOT be used, it rounds halves to even).
  const auto chan = [aos, asw, dcw](std::uint32_t cs, std::uint32_t cd) {
    const std::uint32_t num = cs * asw + cd * dcw;
    return static_cast<std::uint8_t>((2U * num + aos) / (2U * aos));
  };

  return Pixel{chan(s.r, d.r), chan(s.g, d.g), chan(s.b, d.b),
               static_cast<std::uint8_t>(div255(aos))};
}

// The one call the ops make. Picking per-pixel is fine for the scalar path;
// #90 hoists the choice to a whole vector of destination pixels at a time.
[[nodiscard]] constexpr auto blend_pixel(Pixel s, Pixel d) noexcept -> Pixel {
  return d.a == 255U ? source_over_opaque_dst(s, d) : source_over(s, d);
}

} // namespace termforge::detail
