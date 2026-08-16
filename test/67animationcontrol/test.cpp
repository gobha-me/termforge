// TermForge — terminal-driven animation playback and observation (#117).
//
// Registration is flushed before controls are issued, matching App's real
// cadence. Controls then cross their own write boundary before committed state
// is asserted; the refusal case proves projected state rolls back with bytes.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "support/apc.hpp"
#include "support/image.hpp"
#include "support/legacy_driver.hpp"
#include "termforge/core/app.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/drivers/kitty_driver.hpp"

using namespace std::chrono_literals;
using namespace termforge;

namespace {

auto image(std::uint8_t seed) -> Image {
  return tfsupport::solid(2, 2, Pixel{seed, seed, seed, 255});
}

auto frames(const std::array<Image, 4>& images)
    -> std::array<AnimationFrame, 4> {
  return {AnimationFrame{images[0], 20ms}, AnimationFrame{images[1], 35ms},
          AnimationFrame{images[2], 0ms}, AnimationFrame{images[3], 45ms}};
}

auto controls(std::string_view wire) -> std::vector<tfsupport::Apc> {
  std::vector<tfsupport::Apc> result;
  for (const auto& command : tfsupport::apcs(wire))
    if (tfsupport::key_value(command, "a") == "a")
      result.push_back(command);
  return result;
}

class FailingSink final : public ByteSink {
 public:
  auto write(std::span<const char>) -> std::expected<void, ErrorEvent> override {
    return std::unexpected{
        ErrorEvent{Severity::Error, "sink", "animation control refused"}};
  }
};

class AppClockProbe final : public App {
 public:
  AppClockProbe()
      : m_images{image(1), image(2), image(3), image(4)},
        m_frames{frames(m_images)} {}

  auto run_two_frames(std::string& wire, std::unique_ptr<KittyDriver> driver)
      -> void {
    set_frame_ms(20);
    test_run_frames(2, 20, 5, &wire, std::move(driver));
  }

  std::optional<std::chrono::steady_clock::duration> deadline_from_play;
  bool played{false};

 protected:
  auto on_render(Screen&) -> void override {
    if (m_render == 0) {
      const auto registered = driver().register_animation(m_frames);
      REQUIRE(registered);
      m_animation = *registered;
      request_render();
    } else if (m_render == 1) {
      const auto started = now_steady();
      REQUIRE(play_animation(m_animation, AnimationPlayMode::Once,
                             AnimationReplay::Restart));
      const auto status = animation_status(m_animation);
      REQUIRE(status);
      REQUIRE(status->expected_completion);
      deadline_from_play = *status->expected_completion - started;
      played = true;
    }
    ++m_render;
  }

 private:
  std::array<Image, 4> m_images;
  std::array<AnimationFrame, 4> m_frames;
  AnimationHandle m_animation;
  int m_render{0};
};

}  // namespace

TEST_CASE("animation control: once, ignore, and restart-loop are exact",
          "[animation][control][kitty][wire]") {
  KittyDriver driver;
  driver.set_image_animation_support(true);
  std::string wire;
  driver.set_output(&wire);
  const std::array images{image(1), image(2), image(3), image(4)};
  const auto animation = driver.register_animation(frames(images));
  REQUIRE(animation);

  const auto epoch = std::chrono::steady_clock::time_point{100ms};
  REQUIRE(driver.animation_status(*animation, epoch));
  CHECK(driver.animation_status(*animation, epoch)->state ==
        AnimationRunState::Pending);
  driver.flush();
  CHECK(driver.animation_status(*animation, epoch)->state ==
        AnimationRunState::Stopped);
  wire.clear();

  REQUIRE(driver.play_animation(*animation, AnimationPlayMode::Once,
                                AnimationReplay::Restart, epoch));
  const auto once = driver.animation_status(*animation, epoch);
  REQUIRE(once);
  CHECK(once->state == AnimationRunState::PlayingOnce);
  REQUIRE(once->expected_completion);
  CHECK(*once->expected_completion == epoch + 55ms);
  CHECK(wire.empty());
  driver.flush();
  auto commands = controls(wire);
  REQUIRE(commands.size() == 1);
  CHECK(tfsupport::key_value(commands[0], "s") == "2");
  CHECK(tfsupport::key_value(commands[0], "c") == "1");
  CHECK_FALSE(commands[0].has_payload);
  CHECK(driver.last_frame_bytes().image_transmit == 0);
  CHECK(driver.last_frame_bytes().image_edit ==
        driver.last_frame_bytes().total());
  wire.clear();

  REQUIRE(driver.play_animation(*animation, AnimationPlayMode::Loop,
                                AnimationReplay::Ignore, epoch + 10ms));
  CHECK(wire.empty());
  CHECK(driver.animation_status(*animation, epoch + 10ms)->state ==
        AnimationRunState::PlayingOnce);

  REQUIRE(driver.play_animation(*animation, AnimationPlayMode::Loop,
                                AnimationReplay::Restart, epoch + 10ms));
  CHECK(wire.empty());
  CHECK(driver.animation_status(*animation, epoch + 10ms)->state ==
        AnimationRunState::Looping);
  driver.flush();
  commands = controls(wire);
  REQUIRE(commands.size() == 2);
  CHECK(tfsupport::key_value(commands[0], "s") == "1");
  CHECK(tfsupport::key_value(commands[0], "c") == "1");
  CHECK(tfsupport::key_value(commands[1], "s") == "3");
  CHECK(tfsupport::key_value(commands[1], "v") == "1");
  CHECK(tfsupport::key_value(commands[1], "c") == "1");
  CHECK(driver.animation_status(*animation, epoch + 10s)->state ==
        AnimationRunState::Looping);
}

TEST_CASE("animation control: seek rebases once and stop policy is explicit",
          "[animation][control][kitty][timeline]") {
  KittyDriver driver;
  driver.set_image_animation_support(true);
  std::string wire;
  driver.set_output(&wire);
  const std::array images{image(1), image(2), image(3), image(4)};
  const auto animation = driver.register_animation(frames(images));
  REQUIRE(animation);
  driver.flush();
  wire.clear();

  const auto now = std::chrono::steady_clock::time_point{1s};
  REQUIRE(driver.play_animation(*animation, AnimationPlayMode::Once,
                                AnimationReplay::Restart, now));
  driver.flush();
  wire.clear();

  REQUIRE(driver.seek_animation(*animation, 1, now + 5ms));
  const auto sought = driver.animation_status(*animation, now + 5ms);
  REQUIRE(sought);
  REQUIRE(sought->expected_completion);
  CHECK(*sought->expected_completion == now + 40ms);
  CHECK(driver.animation_status(*animation, now + 39ms)->state ==
        AnimationRunState::PlayingOnce);
  CHECK(driver.animation_status(*animation, now + 40ms)->state ==
        AnimationRunState::Complete);
  driver.flush();
  auto commands = controls(wire);
  REQUIRE(commands.size() == 1);
  CHECK_FALSE(tfsupport::has_key(commands[0], "s"));
  CHECK(tfsupport::key_value(commands[0], "c") == "2");
  wire.clear();

  REQUIRE(driver.stop_animation(*animation, AnimationStopMode::Hold));
  CHECK(driver.animation_status(*animation, now)->state ==
        AnimationRunState::Stopped);
  driver.flush();
  commands = controls(wire);
  REQUIRE(commands.size() == 1);
  CHECK(tfsupport::key_value(commands[0], "s") == "1");
  CHECK_FALSE(tfsupport::has_key(commands[0], "c"));
  wire.clear();

  REQUIRE(driver.seek_animation(*animation, 2, now));
  driver.flush();
  commands = controls(wire);
  REQUIRE(commands.size() == 1);
  CHECK(tfsupport::key_value(commands[0], "s") == "1");
  CHECK(tfsupport::key_value(commands[0], "c") == "3");
  wire.clear();

  REQUIRE(driver.stop_animation(*animation, AnimationStopMode::Finish));
  CHECK(driver.animation_status(*animation, now)->state ==
        AnimationRunState::Complete);
  driver.flush();
  commands = controls(wire);
  REQUIRE(commands.size() == 1);
  CHECK(tfsupport::key_value(commands[0], "s") == "1");
  CHECK(tfsupport::key_value(commands[0], "c") == "4");
}

TEST_CASE("animation control: failures emit no partial state",
          "[animation][control][kitty][failure]") {
  KittyDriver driver;
  driver.set_image_animation_support(true);
  std::string wire;
  driver.set_output(&wire);
  const std::array images{image(1), image(2), image(3), image(4)};
  const auto animation = driver.register_animation(frames(images));
  REQUIRE(animation);
  const auto now = std::chrono::steady_clock::time_point{};

  CHECK_FALSE(driver.play_animation(*animation, AnimationPlayMode::Once,
                                    AnimationReplay::Restart, now));
  CHECK_FALSE(driver.unregister_animation(*animation));
  driver.flush();
  wire.clear();

  CHECK_FALSE(driver.seek_animation(*animation, 4, now));
  CHECK_FALSE(driver.play_animation(
      *animation, static_cast<AnimationPlayMode>(99),
      AnimationReplay::Restart, now));
  CHECK_FALSE(driver.play_animation(*animation, AnimationPlayMode::Once,
                                    static_cast<AnimationReplay>(99), now));
  CHECK_FALSE(driver.stop_animation(
      *animation, static_cast<AnimationStopMode>(99)));
  CHECK(wire.empty());
  CHECK(driver.animation_status(*animation, now)->state ==
        AnimationRunState::Stopped);

  REQUIRE(driver.play_animation(*animation, AnimationPlayMode::Loop,
                                AnimationReplay::Restart, now));
  CHECK(driver.animation_status(*animation, now)->state ==
        AnimationRunState::Looping);
  FailingSink refused;
  driver.set_output(&refused);
  driver.flush();
  REQUIRE(driver.take_output_error());
  CHECK(driver.animation_status(*animation, now)->state ==
        AnimationRunState::Stopped);

  KittyDriver other;
  other.set_image_animation_support(true);
  CHECK_FALSE(other.animation_status(*animation, now));
}

TEST_CASE("animation control: unregister deletes owned data and stales handle",
          "[animation][control][kitty][lifetime]") {
  KittyDriver driver;
  driver.set_image_animation_support(true);
  std::string wire;
  driver.set_output(&wire);
  const std::array images{image(1), image(2), image(3), image(4)};
  const auto animation = driver.register_animation(frames(images));
  REQUIRE(animation);
  driver.flush();
  wire.clear();
  REQUIRE(driver.residency().pinned_images == 1);

  REQUIRE(driver.unregister_animation(*animation));
  CHECK_FALSE(driver.animation_status(*animation,
                                      std::chrono::steady_clock::time_point{}));
  const auto stale_play = driver.play_animation(
      *animation, AnimationPlayMode::Once, AnimationReplay::Restart,
      std::chrono::steady_clock::time_point{});
  REQUIRE_FALSE(stale_play);
  CHECK(stale_play.error().severity == Severity::Warning);
  CHECK(driver.residency().pinned_images == 1);
  driver.flush();
  REQUIRE(tfsupport::apcs(wire).size() == 1);
  CHECK(tfsupport::key_value(tfsupport::apcs(wire)[0], "a") == "d");
  CHECK(tfsupport::key_value(tfsupport::apcs(wire)[0], "d") == "I");
  CHECK(driver.residency() == ImageResidency{});
}

TEST_CASE("animation control: legacy driver defaults remain honest",
          "[animation][control][compatibility]") {
  tfsupport::LegacyDriver legacy;
  TerminalDriver& driver = legacy;
  const AnimationHandle animation{1, 1, 1};
  const auto now = std::chrono::steady_clock::time_point{};

  const auto play = driver.play_animation(
      animation, AnimationPlayMode::Once, AnimationReplay::Restart, now);
  const auto seek = driver.seek_animation(animation, 0, now);
  const auto stop = driver.stop_animation(animation, AnimationStopMode::Hold);
  const auto status = driver.animation_status(animation, now);
  const auto release = driver.unregister_animation(animation);
  REQUIRE_FALSE(play);
  REQUIRE_FALSE(seek);
  REQUIRE_FALSE(stop);
  REQUIRE_FALSE(status);
  REQUIRE_FALSE(release);
  CHECK(play.error().severity == Severity::Warning);
  CHECK(seek.error().severity == Severity::Warning);
  CHECK(stop.error().severity == Severity::Warning);
  CHECK(status.error().severity == Severity::Warning);
  CHECK(release.error().severity == Severity::Warning);
}

TEST_CASE("animation control: App forwards its synthetic timeline",
          "[animation][control][app][clock]") {
  SyntheticClock clock;
  AppClockProbe app;
  app.set_clock(&clock);
  std::string wire;
  auto driver = std::make_unique<KittyDriver>();
  driver->set_image_animation_support(true);
  app.run_two_frames(wire, std::move(driver));

  CHECK(app.played);
  REQUIRE(app.deadline_from_play);
  CHECK(*app.deadline_from_play == 55ms);
  const auto commands = controls(wire);
  REQUIRE(commands.size() >= 2);  // root-gap registration plus play
  CHECK(tfsupport::key_value(commands.back(), "s") == "2");
}
