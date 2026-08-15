// TermForge example: input
//
// Demonstrates keyboard and mouse event handling. Shows how to:
//   - Override on_event(const Event&)
//   - Handle KeyEvent, MouseEvent, and ResizeEvent
//   - Display live input state on screen
//   - Use std::visit for event dispatch
//   - Opt into the kitty keyboard protocol and see press/repeat/release (#60)
//
// Press "k" to cycle KeyboardMode live. On a terminal without the protocol
// the tier line still changes but every key keeps arriving as a press, and
// the Info ErrorEvent saying so shows up in the "Last key" line at startup --
// that is the degradation-is-an-event contract, visible.

#include <format>
#include <string>
#include <string_view>

#include "termforge/core/app.hpp"

using namespace termforge;

class InputApp final : public App {
 public:
  auto on_event(const Event& ev) -> void override {
    // Route to base class for ESC/Ctrl+C handling
    std::visit([this](const auto& e) { this->handle(e); }, ev);
    App::on_event(ev);
  }

  auto on_render(Screen& screen) -> void override {
    screen.clear();
    const int W = screen.cols();
    int y = 1;

    screen.write_text(2, y++, "TermForge Input Demo", Rgb{0xFF, 0xFF, 0xFF}, {});
    y++;

    screen.write_text(2, y++, "Keyboard:", Rgb{0xC0, 0xC0, 0xC0}, {});
    screen.write_text(4, y++, "Last key: " + m_last_key, Rgb{0x00, 0xFF, 0x80}, {});
    screen.write_text(4, y++, "Modifiers: " + m_modifiers, Rgb{0x00, 0xFF, 0x80}, {});
    screen.write_text(4, y++, "Action: " + m_action, Rgb{0x00, 0xFF, 0x80}, {});
    y++;

    screen.write_text(2, y++, "Keyboard protocol (#60):", Rgb{0xC0, 0xC0, 0xC0}, {});
    screen.write_text(4, y++, "Tier: " + std::string{mode_name(keyboard_mode())},
                      Rgb{0x00, 0xFF, 0x80}, {});
    screen.write_text(4, y++,
                      std::string{"Terminal supports it: "} +
                          (capabilities().kitty_keyboard ? "yes" : "no"),
                      Rgb{0x00, 0xFF, 0x80}, {});
    y++;

    screen.write_text(2, y++, "Mouse:", Rgb{0xC0, 0xC0, 0xC0}, {});
    screen.write_text(4, y++, "Position: " + m_mouse_pos, Rgb{0x00, 0xFF, 0x80}, {});
    screen.write_text(4, y++, "Button: " + m_mouse_btn, Rgb{0x00, 0xFF, 0x80}, {});
    y++;

    screen.write_text(2, y++, "Window:", Rgb{0xC0, 0xC0, 0xC0}, {});
    screen.write_text(4, y, std::format("Size: {}x{}", W, screen.rows()), Rgb{0x00, 0xFF, 0x80}, {});
    y += 2;

    screen.write_text(0, screen.rows() - 1,
                      "Press k to cycle the keyboard tier, ESC to quit",
                      Rgb{0x80, 0x80, 0x80}, {});
  }

 private:
  auto handle(const KeyEvent& k) -> void {
    // Cycle the tier live. Only presses act -- under Enhanced this key also
    // arrives as a repeat and a release, and acting on those would cycle
    // three times per keystroke.
    if (k.key == Key::Char && k.ch == U'k' && !k.ctrl &&
        k.action == KeyAction::Press) {
      set_keyboard_mode(next_mode(keyboard_mode()));
    }
    m_action = action_name(k.action);
    m_last_key = key_name(k.key);
    if (k.key == Key::Char) {
      if (k.ch < 0x80) m_last_key += std::format(" ('{}')", static_cast<char>(k.ch));
      else m_last_key += std::format(" (U+{:04X})", static_cast<unsigned>(k.ch));
    }
    m_modifiers = std::format("{}{}{}",
      k.ctrl ? "Ctrl " : "",
      k.alt ? "Alt " : "",
      k.shift ? "Shift" : "");
    if (m_modifiers.empty()) m_modifiers = "none";
  }

  auto handle(const MouseEvent& m) -> void {
    m_mouse_pos = std::format("({}, {})", m.x, m.y);
    if (m.scroll_up) m_mouse_btn = "scroll up";
    else if (m.scroll_down) m_mouse_btn = "scroll down";
    else m_mouse_btn = std::format("{} {}", m.pressed ? "press" : "release", m.button);
  }

  auto handle(const PasteEvent& p) -> void {
    m_last_key = std::format("paste ({} bytes)", p.text.size());
    m_modifiers = "";
    m_action = "";
  }

  auto handle(const ResizeEvent& r) -> void {
    m_last_key = std::format("resize to {}x{}", r.cols, r.rows);
    m_modifiers = "";
    m_action = "";
  }

  auto handle(const ErrorEvent& e) -> void {
    m_last_key = "error: " + e.message;
    m_modifiers = "";
    m_action = "";
  }

  auto handle(const ImageInvalidatedEvent&) -> void {
    m_last_key = "resident images invalidated";
    m_modifiers = "";
    m_action = "";
  }

  static auto action_name(KeyAction a) -> std::string {
    switch (a) {
      case KeyAction::Press: return "press";
      case KeyAction::Repeat: return "repeat";
      case KeyAction::Release: return "release";
    }
    return "?";
  }

  static auto mode_name(KeyboardMode m) -> std::string_view {
    switch (m) {
      case KeyboardMode::Legacy: return "Legacy (presses only)";
      case KeyboardMode::Disambiguate: return "Disambiguate (flags 3)";
      case KeyboardMode::Enhanced: return "Enhanced (flags 27)";
    }
    return "?";
  }

  static auto next_mode(KeyboardMode m) -> KeyboardMode {
    switch (m) {
      case KeyboardMode::Legacy: return KeyboardMode::Disambiguate;
      case KeyboardMode::Disambiguate: return KeyboardMode::Enhanced;
      case KeyboardMode::Enhanced: return KeyboardMode::Legacy;
    }
    return KeyboardMode::Legacy;
  }

  static auto key_name(Key k) -> std::string {
    switch (k) {
      case Key::Unknown: return "Unknown";
      case Key::Char: return "Char";
      case Key::Enter: return "Enter";
      case Key::Escape: return "Escape";
      case Key::Backspace: return "Backspace";
      case Key::Delete: return "Delete";
      case Key::Tab: return "Tab";
      case Key::Up: return "Up";
      case Key::Down: return "Down";
      case Key::Left: return "Left";
      case Key::Right: return "Right";
      case Key::Home: return "Home";
      case Key::End: return "End";
      case Key::PageUp: return "PageUp";
      case Key::PageDown: return "PageDown";
      case Key::F1: return "F1";
      case Key::F2: return "F2";
      case Key::F3: return "F3";
      case Key::F4: return "F4";
      case Key::F5: return "F5";
      case Key::F6: return "F6";
      case Key::F7: return "F7";
      case Key::F8: return "F8";
      case Key::F9: return "F9";
      case Key::F10: return "F10";
      case Key::F11: return "F11";
      case Key::F12: return "F12";
      case Key::LeftShift: return "LeftShift";
      case Key::LeftCtrl: return "LeftCtrl";
      case Key::LeftAlt: return "LeftAlt";
      case Key::RightShift: return "RightShift";
      case Key::RightCtrl: return "RightCtrl";
      case Key::RightAlt: return "RightAlt";
    }
    return "?";
  }

  std::string m_last_key = "none";
  std::string m_modifiers = "";
  std::string m_action = "";
  std::string m_mouse_pos = "(0, 0)";
  std::string m_mouse_btn = "none";
};

auto main() -> int {
  InputApp app;
  return app.run();
}
