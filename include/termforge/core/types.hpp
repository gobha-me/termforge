#pragma once

// TermForge — core value types.
//
// These are the shared currency across drivers, widgets, and the renderer.
// Degradation and failure are modeled as *events* (see Event / ErrorEvent)
// rather than silent downgrade, per the project design.

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace termforge {

// ── color ────────────────────────────────────────────────────────────────

struct Rgb {
  std::uint8_t r{0}, g{0}, b{0};
  constexpr auto operator==(const Rgb&) const -> bool = default;
};

// ── image ────────────────────────────────────────────────────────────────
// Raw 32-bit RGBA pixel buffer. Loaded from raw-RGB assets (PNG/JPEG are
// deliberately out of scope for the core; decode elsewhere and hand us RGBA).

struct Pixel {
  std::uint8_t r{0}, g{0}, b{0}, a{255};
  constexpr auto operator==(const Pixel&) const -> bool = default;
};

class Image {
 public:
  Image() = default;
  Image(int width, int height, std::vector<Pixel> pixels)
      : m_width(width), m_height(height), m_pixels(std::move(pixels)) {}

  [[nodiscard]] auto width() const noexcept -> int { return m_width; }
  [[nodiscard]] auto height() const noexcept -> int { return m_height; }
  [[nodiscard]] auto empty() const noexcept -> bool { return m_pixels.empty(); }

  [[nodiscard]] auto at(int x, int y) const -> const Pixel& {
    return m_pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_width) +
                    static_cast<std::size_t>(x)];
  }

 private:
  int m_width{0};
  int m_height{0};
  std::vector<Pixel> m_pixels;
};

// ── capabilities ─────────────────────────────────────────────────────────
// Result of probing the *terminal* (never the display server). Drives driver
// selection.

struct Capabilities {
  bool kitty_graphics{false};
  bool sixel{false};
  bool truecolor{false};
  int color_levels{0};  // 0 = unknown, else 24 / 256 / 16
};

// ── events ───────────────────────────────────────────────────────────────

enum class Severity { Info, Warning, Error };

// A downgrade or failure surfaced to the application instead of being silent.
struct ErrorEvent {
  Severity severity{Severity::Info};
  std::string source;   // e.g. "kitty", "sixel", "detect"
  std::string message;
};

enum class Key {
  Unknown, Char, Enter, Escape, Backspace, Delete, Tab,
  Up, Down, Left, Right, Home, End, PageUp, PageDown,
  F1, F2, F3, F4,
};

struct KeyEvent {
  Key key{Key::Unknown};
  char32_t ch{0};       // valid when key == Key::Char
  bool ctrl{false}, alt{false}, shift{false};
};

struct MouseEvent {
  int x{0}, y{0};
  int button{0};        // 0 left, 1 middle, 2 right; -1 = none (wheel/motion)
  bool pressed{false};
  bool scroll_up{false}, scroll_down{false};
  bool ctrl{false}, alt{false}, shift{false};
};

struct ResizeEvent {
  int cols{0}, rows{0};
};

// A bracketed-paste run (mode 2004): the terminal brackets pasted text in
// ESC[200~ … ESC[201~ so it arrives as one event, and an ESC *inside* the paste
// can't masquerade as an Escape keypress. `text` is the raw pasted bytes.
struct PasteEvent {
  std::string text;
};

// The event bus: input, resize, and error/degradation all ride one variant.
using Event =
    std::variant<KeyEvent, MouseEvent, PasteEvent, ResizeEvent, ErrorEvent>;

}  // namespace termforge
