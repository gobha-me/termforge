// TermForge example: terminal-driven animation control (#117).
//
// This is a controller/status example for a sequence kept resident by Kitty.
// Registration and every playback command are independent of the cell render;
// the screen shows the authored frames as swatches and the locally commanded
// state. Kitty has no completion query, so "complete" means the declared gaps
// reached their deadline on App's monotonic clock.

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "termforge/core/app.hpp"
#include "termforge/core/types.hpp"

using namespace std::chrono_literals;
using namespace termforge;

namespace {

auto frame(Rgb rgb) -> Image {
  constexpr int kSide = 24;
  return Image{kSide, kSide,
               std::vector<Pixel>(
                   kSide * kSide,
                   Pixel{rgb.r, rgb.g, rgb.b, std::uint8_t{255}})};
}

auto state_name(AnimationRunState state) -> std::string_view {
  switch (state) {
    case AnimationRunState::Pending: return "pending upload";
    case AnimationRunState::Stopped: return "stopped";
    case AnimationRunState::PlayingOnce: return "playing once";
    case AnimationRunState::Looping: return "looping";
    case AnimationRunState::Complete: return "complete (client timeline)";
  }
  return "unknown";
}

}  // namespace

class AnimationControlDemo final : public App {
 public:
  AnimationControlDemo()
      : m_images{frame({235, 80, 80}), frame({245, 190, 70}),
                 frame({80, 210, 130}), frame({90, 145, 245})},
        m_frames{AnimationFrame{m_images[0], 180ms},
                 AnimationFrame{m_images[1], 180ms},
                 AnimationFrame{m_images[2], 180ms},
                 AnimationFrame{m_images[3], 180ms}} {
    set_frame_ms(16);
  }

  auto on_start() -> void override {
    const auto registered = driver().register_animation(m_frames);
    if (registered)
      m_animation = *registered;
    else
      m_message = registered.error().message;
  }

  auto on_stop() noexcept -> void override {
    if (m_animation) (void)unregister_animation(m_animation);
  }

  auto on_event(const Event& event) -> void override {
    const auto* key = std::get_if<KeyEvent>(&event);
    if (key == nullptr) {
      App::on_event(event);
      return;
    }
    if (key->ch == 'p' || key->ch == 'P')
      command(play_animation(m_animation, AnimationPlayMode::Once,
                             AnimationReplay::Restart));
    else if (key->ch == 'l' || key->ch == 'L')
      command(play_animation(m_animation, AnimationPlayMode::Loop,
                             AnimationReplay::Restart));
    else if (key->ch == 'i' || key->ch == 'I')
      command(play_animation(m_animation, AnimationPlayMode::Once,
                             AnimationReplay::Ignore));
    else if (key->ch == 's' || key->ch == 'S')
      command(stop_animation(m_animation, AnimationStopMode::Hold));
    else if (key->ch == 'f' || key->ch == 'F')
      command(stop_animation(m_animation, AnimationStopMode::Finish));
    else if (key->ch >= '0' && key->ch <= '3')
      command(seek_animation(m_animation,
                             static_cast<std::size_t>(key->ch - '0')));
    else
      App::on_event(event);  // ESC / Ctrl+C quit
  }

  auto on_tick(std::chrono::duration<double>) -> void override {
    // on_start queues registration; its first frame must cross the sink before
    // playback can address the accepted resident sequence.
    if (!m_animation || m_started) return;
    const auto status = animation_status(m_animation);
    if (status && status->state == AnimationRunState::Stopped) {
      command(play_animation(m_animation, AnimationPlayMode::Once,
                             AnimationReplay::Restart));
      m_started = true;
    }
  }

  auto on_render(Screen& screen) -> void override {
    screen.clear(Cell{.text = " ", .fg = {}, .bg = kBg});
    screen.write_text(2, 1, "Terminal-driven animation control", {235, 240, 250},
                      {12, 16, 24}, Attr::Bold);
    screen.write_text(2, 3, "P once/restart   L loop   I once/ignore", kFg,
                      kBg);
    screen.write_text(2, 4, "S hold           F finish  0..3 seek", kFg,
                      kBg);
    screen.write_text(2, 5, "ESC quits", kFg, kBg);

    std::string state = "unavailable";
    if (m_animation) {
      const auto status = animation_status(m_animation);
      if (status) state = std::string{state_name(status->state)};
    }
    screen.write_text(2, 7, std::format("state: {}", state), kFg, kBg);
    if (!m_message.empty()) screen.write_text(2, 9, m_message, kWarn, kBg);

    constexpr std::array<Rgb, 4> colors{{{235, 80, 80}, {245, 190, 70},
                                         {80, 210, 130}, {90, 145, 245}}};
    for (int i = 0; i < 4; ++i) {
      screen.write_text(2 + i * 5, 11, std::format(" {} ", i), {8, 10, 14},
                        colors[static_cast<std::size_t>(i)], Attr::Bold);
    }
  }

 private:
  auto command(std::expected<void, ErrorEvent> result) -> void {
    m_message = result ? std::string{} : result.error().message;
  }

  static constexpr Rgb kFg{220, 225, 235};
  static constexpr Rgb kBg{12, 16, 24};
  static constexpr Rgb kWarn{245, 130, 90};
  std::array<Image, 4> m_images;
  std::array<AnimationFrame, 4> m_frames;
  AnimationHandle m_animation;
  std::string m_message;
  bool m_started{false};
};

auto main() -> int {
  AnimationControlDemo app;
  return app.run();
}
