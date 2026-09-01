#pragma once

// OBSCURA's M0 dissolve is an example contract, not TermForge API.  The
// picture is selected only by an integer step; the App wrapper owns timing.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "termforge/core/screen.hpp"
#include "termforge/core/types.hpp"
#include "termforge/examples/obscura_dissolve_assets.hpp"

namespace termforge::example::obscura {

using namespace std::chrono_literals;

inline constexpr std::size_t kRevealSteps = 8;
inline constexpr std::size_t kGlyphSteps = 4;
inline constexpr std::size_t kTintSteps = 1;
inline constexpr std::size_t kDissolveSteps =
    kRevealSteps + kGlyphSteps + kTintSteps;
inline constexpr std::array<std::chrono::milliseconds, kDissolveSteps>
    kStepGaps{20ms, 20ms, 20ms, 20ms, 20ms, 20ms, 20ms,
              20ms, 35ms, 35ms, 35ms, 35ms, 100ms};
inline constexpr auto kDissolveDuration = 400ms;
inline constexpr Extent kPlatePixels{240, 160};
inline constexpr Rect kPlateCells{2, 4, 20, 7};
inline constexpr ImagePlacementOptions kPlatePlacement{
    .fit = PlacementFit::Stretch,
    .layer = ImageLayer::below_text(),
};
inline constexpr Rgb kVoid{7, 10, 14};
inline constexpr Rgb kInk{190, 202, 210};
inline constexpr Rgb kDamageTint{43, 25, 24};
inline constexpr Rgb kSettledTint{18, 22, 26};
inline constexpr Rgb kLabel{225, 231, 234};
inline constexpr std::array<std::string_view, 4> kNoise{"+", "#", "%", "?"};

struct VisualState {
  std::size_t reveal_frame{0};
  std::uint8_t glyph_strata{4};
  bool damage_tint{true};

  constexpr auto operator==(const VisualState&) const noexcept
      -> bool = default;
};

[[nodiscard]] constexpr auto visual_for_step(std::size_t step) noexcept
    -> VisualState {
  step = std::min(step, kDissolveSteps - 1);
  const std::size_t reveal = std::min(step, kRevealSteps - 1);
  const std::size_t removed =
      step < kRevealSteps ? 0 : std::min(step - kRevealSteps + 1, kGlyphSteps);
  return VisualState{
      .reveal_frame = reveal,
      .glyph_strata = static_cast<std::uint8_t>(kGlyphSteps - removed),
      .damage_tint = step < kRevealSteps + kGlyphSteps,
  };
}

[[nodiscard]] constexpr auto reveal_frame_for_resume(
    std::size_t step, bool finish_requested) noexcept -> std::size_t {
  return finish_requested ? kRevealSteps - 1
                          : visual_for_step(step).reveal_frame;
}

inline auto paint_composition(Screen& screen, VisualState visual) -> void {
  const Rgb tint = visual.damage_tint ? kDamageTint : kSettledTint;
  screen.clear(kInk, kVoid);
  screen.fill_rect(kPlateCells.x, kPlateCells.y, kPlateCells.w, kPlateCells.h,
                   kInk, tint);

  const int left = kPlateCells.x - 1;
  const int top = kPlateCells.y - 1;
  screen.write_text(left, top, "+--------------------+", kInk, kVoid);
  screen.write_text(left, top + 8, "+--------------------+", kInk, kVoid);
  for (int y = 0; y < kPlateCells.h; ++y) {
    screen.write_text(left, kPlateCells.y + y, "|", kInk, kVoid);
    screen.write_text(left + 21, kPlateCells.y + y, "|", kInk, kVoid);
  }

  for (int y = 0; y < kPlateCells.h; ++y) {
    for (int x = 0; x < kPlateCells.w; ++x) {
      const std::size_t stratum =
          static_cast<std::size_t>((x * 7 + y * 11) % 4);
      if (stratum >= visual.glyph_strata) continue;
      screen.write_text(kPlateCells.x + x, kPlateCells.y + y, kNoise[stratum],
                        kInk, tint);
    }
  }
  screen.write_text(kPlateCells.x + 1, kPlateCells.y, "HOLD D-0", kLabel, tint,
                    Attr::Bold);
}

class Timeline {
 public:
  [[nodiscard]] auto step() const noexcept -> std::size_t { return m_step; }
  [[nodiscard]] auto finished() const noexcept -> bool { return m_finished; }
  [[nodiscard]] auto visual() const noexcept -> VisualState {
    return visual_for_step(m_step);
  }

  auto advance(std::chrono::nanoseconds elapsed) noexcept -> bool {
    if (m_finished || elapsed <= 0ns) return false;
    bool changed = false;
    m_carry += elapsed;
    while (!m_finished && m_carry >= kStepGaps[m_step]) {
      m_carry -= kStepGaps[m_step];
      if (m_step + 1 == kDissolveSteps) {
        m_finished = true;
      } else {
        ++m_step;
      }
      changed = true;
    }
    return changed;
  }

  auto skip() noexcept -> void {
    m_step = kDissolveSteps - 1;
    m_carry = 0ns;
    m_finished = true;
  }

 private:
  std::size_t m_step{0};
  std::chrono::nanoseconds m_carry{0};
  bool m_finished{false};
};

template <std::size_t N>
[[nodiscard]] inline auto png_bytes(const std::array<unsigned char, N>& bytes)
    -> std::span<const std::byte> {
  return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

[[nodiscard]] inline auto reveal_images() -> std::array<EncodedImage, 8> {
  return {
      EncodedImage{ImageFormat::Png, png_bytes(kReveal01), kPlatePixels},
      EncodedImage{ImageFormat::Png, png_bytes(kReveal02), kPlatePixels},
      EncodedImage{ImageFormat::Png, png_bytes(kReveal03), kPlatePixels},
      EncodedImage{ImageFormat::Png, png_bytes(kReveal04), kPlatePixels},
      EncodedImage{ImageFormat::Png, png_bytes(kReveal05), kPlatePixels},
      EncodedImage{ImageFormat::Png, png_bytes(kReveal06), kPlatePixels},
      EncodedImage{ImageFormat::Png, png_bytes(kReveal07), kPlatePixels},
      EncodedImage{ImageFormat::Png, png_bytes(kReveal08), kPlatePixels},
  };
}

[[nodiscard]] inline auto reveal_frames(
    const std::array<EncodedImage, 8>& images)
    -> std::array<AnimationFrame, 8> {
  return {
      AnimationFrame{images[0], 20ms}, AnimationFrame{images[1], 20ms},
      AnimationFrame{images[2], 20ms}, AnimationFrame{images[3], 20ms},
      AnimationFrame{images[4], 20ms}, AnimationFrame{images[5], 20ms},
      AnimationFrame{images[6], 20ms}, AnimationFrame{images[7], 20ms},
  };
}

inline constexpr std::size_t kRevealPayloadBytes =
    kReveal01.size() + kReveal02.size() + kReveal03.size() + kReveal04.size() +
    kReveal05.size() + kReveal06.size() + kReveal07.size() + kReveal08.size();

} // namespace termforge::example::obscura
