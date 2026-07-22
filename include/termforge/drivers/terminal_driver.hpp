#pragma once

// TermForge — the driver interface.
//
// Drivers implement this virtual interface and are owned as
// std::unique_ptr<TerminalDriver>. Runtime polymorphism (not a closed
// std::variant) because the driver set is *open*: third-party drivers are an
// explicit extensibility goal. Virtual dispatch cost is irrelevant next to
// terminal I/O.
//
// The DriverImpl concept below is a compile-time conformance check only (used
// in tests via static_assert) — a concept cannot parameterize unique_ptr and
// is not a dispatch mechanism.

#include <concepts>
#include <expected>
#include <memory>
#include <string_view>
#include <type_traits>

#include "termforge/core/types.hpp"

namespace termforge {

class TerminalDriver {
 public:
  virtual ~TerminalDriver() = default;

  virtual auto init() -> std::expected<void, ErrorEvent> = 0;
  virtual auto draw_text(int x, int y, std::string_view text) -> void = 0;
  virtual auto draw_image(int x, int y, const Image& image)
      -> std::expected<void, ErrorEvent> = 0;
  virtual auto flush() -> void = 0;
  [[nodiscard]] virtual auto capabilities() const noexcept -> Capabilities = 0;
};

// Compile-time conformance check for concrete drivers. Not a dispatch tool.
template <typename T>
concept DriverImpl = std::derived_from<T, TerminalDriver> && std::is_final_v<T>;

}  // namespace termforge
