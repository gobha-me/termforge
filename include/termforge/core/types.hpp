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

// ── text attributes (#62) ────────────────────────────────────────────────────
// Per-cell display attributes beyond fg/bg color, carried on `Cell` and
// emitted by the renderer's SGR run-coalescing. A bitmask so a cell can carry
// several at once. `Cell` is stored per grid position and compared every
// frame by the diff renderer, so the attribute is packed into one byte and
// the equality is the default memberwise compare.
//
// These are the attributes a terminal can express that color alone cannot:
//   * Dim       — the semantic tool for a disabled/inactive control.
//   * Reverse   — how selection is conventionally drawn; theme-independent,
//                 unlike the fg/bg swap widgets currently hand-roll.
//   * Underline — the accepted affordance for a link / menu accelerator.
// On the low-color / FallbackDriver tier, attributes are the *only* channel
// left once color is gone, which makes this a degradation-story feature, not
// just expressiveness.
//
// Per-driver survival: AnsiRgbDriver and KittyDriver pass all six through;
// FallbackDriver (the floor) emits only Reverse and Bold — universally honored
// even on a dumb terminal — and drops the rest, surfaced as ErrorEvent{Info}
// per the degradation-is-an-event contract.
enum class Attr : std::uint8_t {
  None = 0,
  Bold = 1 << 0,       // SGR 1
  Dim = 1 << 1,        // SGR 2
  Italic = 1 << 2,     // SGR 3
  Underline = 1 << 3,  // SGR 4
  Reverse = 1 << 4,    // SGR 7
  Strike = 1 << 5,     // SGR 9
};

[[nodiscard]] constexpr auto operator|(Attr a, Attr b) -> Attr {
  return static_cast<Attr>(static_cast<std::uint8_t>(a) |
                           static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr auto operator&(Attr a, Attr b) -> Attr {
  return static_cast<Attr>(static_cast<std::uint8_t>(a) &
                           static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr auto operator~(Attr a) -> Attr {
  return static_cast<Attr>(~static_cast<std::uint8_t>(a));
}
constexpr auto operator|=(Attr& a, Attr b) -> Attr& {
  a = a | b;
  return a;
}
constexpr auto operator&=(Attr& a, Attr b) -> Attr& {
  a = a & b;
  return a;
}
// Whether any attribute bit is set (an Attr is a bitmask, not a bool).
[[nodiscard]] constexpr auto any(Attr a) -> bool { return a != Attr::None; }

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
  F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
};

// Which mouse events the terminal is asked to report (#75):
//   None   \u2014 no tracking at all: native click-drag selection (copy/paste out
//            of the app) belongs to the terminal again.
//   Click  \u2014 ?1000h: presses and releases only, no motion of any kind.
//   Drag   \u2014 ?1002h: presses/releases plus motion *while a button is held*
//            (and wheel). The default \u2014 what TermForge has always asked for.
//   Motion \u2014 ?1003h: any-event tracking, adds buttonless hover. A grid app
//            that wants its keyboard cursor to follow the pointer needs this;
//            it is also the noisiest mode (an event per crossed cell).
// The SGR *encoding* (?1006h) is orthogonal \u2014 it is not a tracking mode and
// is enabled with any non-None mode.
enum class MouseMode { None, Click, Drag, Motion };

struct KeyEvent {
  Key key{Key::Unknown};
  char32_t ch{0};       // valid when key == Key::Char
  bool ctrl{false}, alt{false}, shift{false};
};

struct MouseEvent {
  int x{0}, y{0};
  int button{0};        // 0 left, 1 middle, 2 right; -1 = wheel, 3 = buttonless motion
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
