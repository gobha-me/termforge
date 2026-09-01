// TermForge example: OBSCURA's M0 dissolve spike (gobha-me/obscura#23).
//
// The eight image frames upload once before playback.  The visible transition
// then runs for 400 ms: 160 ms of ordered reveal, 140 ms of glyph attrition,
// and a 100 ms tint drain/settle.  Press any key to finish immediately; ESC
// keeps App's ordinary quit behavior.

#include <array>
#include <chrono>
#include <expected>
#include <format>
#include <string>
#include <variant>

#include "obscura_dissolve_model.hpp"
#include "termforge/core/app.hpp"

using namespace termforge;
using namespace termforge::example::obscura;

namespace {

constexpr Rgb kWarn{245, 130, 90};

class ObscuraDissolveDemo final : public App {
 public:
  ObscuraDissolveDemo()
      : m_images(reveal_images()), m_frames(reveal_frames(m_images)) {
    require(AppRequirements{
        .graphics = true, .truecolor = true, .min_cols = 26, .min_rows = 14});
    set_frame_ms(20);
    set_frame_observer([this](const FrameObservation& frame) {
      if (frame.output_accepted) {
        m_finish_control_queued = false;
      } else {
        m_retry_frame = true;
      }
    });
  }

  auto on_start() -> void override {
    if (!driver().supports_image_animation()) {
      m_message = "selected graphics route cannot register image animations";
      return;
    }
    register_sequence();
  }

  auto on_stop() noexcept -> void override {
    if (m_animation) (void)driver().unregister_animation(m_animation);
  }

  auto on_event(const Event& event) -> void override {
    if (std::holds_alternative<ImageInvalidatedEvent>(event)) {
      m_animation = {};
      m_placed = false;
      m_started = false;
      m_recovering = true;
      register_sequence();
      return;
    }

    const auto* key = std::get_if<KeyEvent>(&event);
    if (key == nullptr || key->action != KeyAction::Press ||
        key->key == Key::Escape || (key->ctrl && key->ch == U'c')) {
      App::on_event(event);
      return;
    }
    if (!m_animation || m_timeline.finished()) return;

    m_skip_requested = true;
    if (m_started) {
      const auto stopped =
          stop_animation(m_animation, AnimationStopMode::Finish);
      if (stopped) {
        m_timeline.skip();
        m_finish_control_queued = true;
        request_render();
      } else {
        m_message = stopped.error().message;
      }
    }
  }

  auto on_tick(std::chrono::duration<double> elapsed) -> void override {
    if (!m_animation || !m_message.empty()) return;

    if (m_retry_frame) {
      m_retry_frame = false;
      if (m_finish_control_queued) {
        const auto stopped =
            stop_animation(m_animation, AnimationStopMode::Finish);
        if (!stopped) m_message = stopped.error().message;
      }
      request_render();
      return;
    }

    if (m_timeline.finished() && !m_recovering) return;

    const auto status = animation_status(m_animation);
    if (!status) {
      m_message = status.error().message;
      request_render();
      return;
    }
    if (status->state == AnimationRunState::Pending) return;

    if (!m_started) {
      if (m_skip_requested || m_timeline.finished() || m_recovering) {
        const std::size_t target =
            reveal_frame_for_resume(m_timeline.step(), m_skip_requested);
        const auto sought = seek_animation(m_animation, target);
        if (!sought) {
          m_message = sought.error().message;
          request_render();
          return;
        }
        if (m_skip_requested) m_timeline.skip();
        m_manual_reveal = m_recovering && m_timeline.step() < kRevealSteps;
      } else {
        const auto played = play_animation(m_animation, AnimationPlayMode::Once,
                                           AnimationReplay::Restart);
        if (!played) {
          m_message = played.error().message;
          request_render();
          return;
        }
      }
      m_started = true;
      m_recovering = false;
      request_render();
      return;
    }

    const std::size_t previous = m_timeline.step();
    if (m_timeline.advance(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed))) {
      if (m_manual_reveal && previous < kRevealSteps &&
          m_timeline.step() < kRevealSteps) {
        const auto sought = seek_animation(m_animation, m_timeline.step());
        if (!sought) m_message = sought.error().message;
      }
      request_render();
    }
  }

  auto on_render(Screen& screen) -> void override {
    paint_composition(screen, m_timeline.visual());

    const auto elapsed = [&] {
      if (m_timeline.finished()) return kDissolveDuration.count();
      std::chrono::milliseconds total{0};
      for (std::size_t i = 0; i < m_timeline.step(); ++i)
        total += kStepGaps[i];
      return total.count();
    }();
    screen.write_text(1, 1,
                      std::format("OBSCURA M0 | step {:02}/13 | {:3} ms",
                                  m_timeline.step() + 1, elapsed),
                      kInk, kVoid, Attr::Bold);
    screen.write_text(1, 12, "any key: finish   ESC: quit", kInk, kVoid);
    if (!m_message.empty()) screen.write_text(1, 13, m_message, kWarn, kVoid);
  }

  auto on_pixels(TerminalDriver& selected) -> void override {
    if (!m_animation || !m_message.empty()) return;
    const auto status = animation_status(m_animation);
    if (!status || status->state == AnimationRunState::Pending) return;

    std::expected<void, ErrorEvent> placed =
        m_placed ? selected.retain_animation(kPlateCells, m_animation,
                                             kPlatePlacement)
                 : selected.draw_animation(kPlateCells, m_animation,
                                           kPlatePlacement);
    if (placed)
      m_placed = true;
    else
      m_message = placed.error().message;
  }

 private:
  auto register_sequence() -> void {
    const auto registered = driver().register_animation(m_frames);
    if (registered)
      m_animation = *registered;
    else
      m_message = registered.error().message;
  }

  std::array<EncodedImage, 8> m_images;
  std::array<AnimationFrame, 8> m_frames;
  AnimationHandle m_animation;
  Timeline m_timeline;
  std::string m_message;
  bool m_started{false};
  bool m_placed{false};
  bool m_skip_requested{false};
  bool m_recovering{false};
  bool m_manual_reveal{false};
  bool m_retry_frame{false};
  bool m_finish_control_queued{false};
};

} // namespace

auto main() -> int {
  ObscuraDissolveDemo app;
  return app.run();
}
