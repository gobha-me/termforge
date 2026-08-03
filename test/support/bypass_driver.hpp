#pragma once

// TermForge test support — a driver that emits its bytes WITHOUT going through
// TerminalDriver::emit_frame (#178).
//
// It exists to pin the one honest limitation of the base-owned sink: the base
// cannot intercept a write it never sees, so a driver that hand-rolls its own
// output ignores set_output entirely. That is exactly as true of tally_frame()
// and has been since #139 — this class makes the property a TEST rather than a
// paragraph, so someone "fixing" it later has to edit a case whose name says
// why it is correct.
//
// It is NOT a model to copy. Every in-tree driver funnels through emit_frame,
// and an out-of-tree driver that wants the sink (which is the whole point of
// #144) must too.
//
// Distinct from support/legacy_driver.hpp, which is deliberately ignorant of
// every interface added after #10 and emits nothing at all. This one emits.

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

#include "termforge/core/types.hpp"
#include "termforge/drivers/terminal_driver.hpp"

namespace tfsupport {

class BypassDriver final : public termforge::TerminalDriver {
 public:
  auto init() -> std::expected<void, termforge::ErrorEvent> override {
    return {};
  }
  auto draw_text(int, int, std::string_view text, termforge::Rgb,
                 termforge::Rgb, termforge::Attr) -> void override {
    m_own += text;
  }
  auto draw_image(termforge::Rect, const termforge::Image&)
      -> std::expected<void, termforge::ErrorEvent> override {
    return {};
  }
  [[nodiscard]] auto preferred_pixel_extent(
      termforge::Rect cells) const noexcept -> termforge::Extent override {
    return termforge::Extent{cells.w, cells.h};
  }
  // The bypass. It meters honestly and writes nowhere the base can see.
  auto flush() -> void override {
    tally_frame(m_own.size());
    m_written += m_own.size();
    m_own.clear();
  }
  [[nodiscard]] auto capabilities() const noexcept
      -> termforge::Capabilities override {
    return termforge::Capabilities{};
  }

  [[nodiscard]] auto written() const noexcept -> std::size_t {
    return m_written;
  }

 private:
  std::string m_own;
  std::size_t m_written{0};
};

}  // namespace tfsupport
