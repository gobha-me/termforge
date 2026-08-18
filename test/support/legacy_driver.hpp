#pragma once

// TermForge test support — a driver written against the ORIGINAL TerminalDriver
// interface.
//
// It overrides the pure virtuals that existed before #163 and nothing else: it
// has never heard of EncodedImage (#163) or PlacementFit (#137), and it does
// not spell `using TerminalDriver::draw_image;`.
//
// Its existence IS a test. Third-party drivers are a stated extensibility goal
// and NOTHING ELSE in test/ derives from TerminalDriver, so before this class
// existed a new PURE virtual would have compiled clean through the whole of CI
// while breaking every out-of-tree driver on upgrade. Each feature that adds a
// virtual here adds a case asserting the base's default is both reachable and
// honest, and "make the new virtual pure" is then a mutation that fails to
// COMPILE, on exactly this class.
//
// Keep it deliberately ignorant. Teaching it about a new interface is the one
// change that would quietly destroy what it is for.

#include <expected>
#include <string_view>

#include "termforge/core/types.hpp"
#include "termforge/drivers/terminal_driver.hpp"

namespace tfsupport {

class LegacyDriver final : public termforge::TerminalDriver {
 public:
  auto init() -> std::expected<void, termforge::ErrorEvent> override {
    return {};
  }
  auto draw_text(int, int, std::string_view, termforge::Rgb, termforge::Rgb,
                 termforge::Attr) -> void override {}
  auto draw_image(termforge::Rect, const termforge::Image&)
      -> std::expected<void, termforge::ErrorEvent> override {
    m_drew_image = true;
    return {};
  }
  [[nodiscard]] auto preferred_pixel_extent(
      termforge::Rect cells) const noexcept -> termforge::Extent override {
    return termforge::Extent{cells.w, cells.h};
  }
  auto flush() -> void override { tally_frame(0); }
  [[nodiscard]] auto capabilities() const noexcept
      -> termforge::Capabilities override {
    return termforge::Capabilities{};
  }

  [[nodiscard]] auto drew_image() const -> bool { return m_drew_image; }
  auto reset() -> void { m_drew_image = false; }

 private:
  bool m_drew_image{false};
};

} // namespace tfsupport
