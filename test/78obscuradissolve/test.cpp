// OBSCURA M0 dissolve spike (gobha-me/obscura#23).
//
// Failures lead: every skip boundary must converge on one final composition,
// unsupported animation must refuse before wire, and a refused registration
// must not become resident.  The happy path then measures the complete
// animation registration plus the cell-band edits against the 40 KiB budget.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "obscura_dissolve_model.hpp"
#include "support/apc.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/core/renderer.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"

using namespace std::chrono_literals;
using namespace termforge;
using namespace termforge::example::obscura;

namespace {

class RefusingSink final : public ByteSink {
 public:
  auto write(std::span<const char>)
      -> std::expected<void, ErrorEvent> override {
    return std::unexpected{
        ErrorEvent{Severity::Error, "sink", "dissolve registration refused"}};
  }
};

auto equal_composition(const Screen& lhs, const Screen& rhs) -> bool {
  if (lhs.cols() != rhs.cols() || lhs.rows() != rhs.rows()) return false;
  for (int y = 0; y < lhs.rows(); ++y) {
    for (int x = 0; x < lhs.cols(); ++x) {
      if (!(lhs.at(x, y) == rhs.at(x, y))) return false;
      if (lhs.text_at(x, y) != rhs.text_at(x, y)) return false;
    }
  }
  return true;
}

auto count_action(std::string_view wire, std::string_view action) -> int {
  int count = 0;
  for (const auto& command : tfsupport::apcs(wire)) {
    if (tfsupport::key_value(command, "a") == action) ++count;
  }
  return count;
}

} // namespace

TEST_CASE("OBSCURA dissolve: every skip boundary lands on one final frame",
          "[obscura][dissolve][failure]") {
  Timeline natural;
  REQUIRE(natural.advance(kDissolveDuration));
  REQUIRE(natural.finished());
  const VisualState final = natural.visual();
  CHECK(final == VisualState{.reveal_frame = 7,
                             .glyph_strata = 0,
                             .damage_tint = false});

  Screen expected{26, 14};
  paint_composition(expected, final);
  for (std::size_t boundary = 0; boundary < kDissolveSteps; ++boundary) {
    Timeline skipped;
    for (std::size_t step = 0; step < boundary; ++step)
      REQUIRE(skipped.advance(kStepGaps[step]));
    skipped.skip();
    CAPTURE(boundary);
    CHECK(skipped.finished());
    CHECK(skipped.visual() == final);

    Screen actual{26, 14};
    paint_composition(actual, skipped.visual());
    CHECK(equal_composition(actual, expected));
    CHECK(reveal_frame_for_resume(boundary, true) == kRevealSteps - 1);
    CHECK(reveal_frame_for_resume(boundary, false) ==
          visual_for_step(boundary).reveal_frame);
  }
}

TEST_CASE("OBSCURA dissolve: the integer schedule owns all 400 milliseconds",
          "[obscura][dissolve][schedule]") {
  CHECK(kDissolveSteps == 13);
  CHECK(kRevealSteps == 8);
  CHECK(kGlyphSteps == 4);
  CHECK(kTintSteps == 1);
  CHECK(std::accumulate(kStepGaps.begin(), kStepGaps.end(), 0ms) ==
        kDissolveDuration);

  Timeline timeline;
  REQUIRE(timeline.advance(159ms));
  CHECK(timeline.step() == 7);
  REQUIRE(timeline.advance(1ms));
  CHECK(timeline.step() == 8);
  CHECK(timeline.visual().glyph_strata == 3);
  REQUIRE(timeline.advance(140ms));
  CHECK(timeline.step() == 12);
  CHECK_FALSE(timeline.visual().damage_tint);
  CHECK_FALSE(timeline.finished());
  REQUIRE(timeline.advance(100ms));
  CHECK(timeline.finished());
}

TEST_CASE("OBSCURA dissolve: unsupported or refused registration stays empty",
          "[obscura][dissolve][failure][wire]") {
  const auto images = reveal_images();
  const auto frames = reveal_frames(images);

  FallbackDriver fallback;
  std::string fallback_wire;
  fallback.set_output(&fallback_wire);
  const auto unsupported = fallback.register_animation(frames);
  REQUIRE_FALSE(unsupported);
  CHECK(unsupported.error().severity == Severity::Warning);
  fallback.flush();
  CHECK(fallback_wire.empty());
  CHECK(fallback.residency() == ImageResidency{});

  KittyDriver refused;
  refused.set_image_animation_support(true);
  RefusingSink sink;
  refused.set_output(&sink);
  REQUIRE(refused.register_animation(frames));
  refused.flush();
  REQUIRE(refused.take_output_error());
  CHECK(refused.residency() == ImageResidency{});
}

TEST_CASE("OBSCURA dissolve: finish selects frame eight without retransmit",
          "[obscura][dissolve][skip][wire]") {
  KittyDriver driver;
  driver.set_image_animation_support(true);
  std::string wire;
  driver.set_output(&wire);
  const auto images = reveal_images();
  const auto frames = reveal_frames(images);
  const auto animation = driver.register_animation(frames);
  REQUIRE(animation);
  driver.flush();
  for (std::size_t reply = 0; reply < kRevealSteps; ++reply)
    driver.consume_reply(TerminalReply{animation->id, std::nullopt, "OK"});
  wire.clear();

  REQUIRE(driver.play_animation(*animation, AnimationPlayMode::Once,
                                AnimationReplay::Restart,
                                std::chrono::steady_clock::time_point{}));
  driver.flush();
  wire.clear();
  REQUIRE(driver.stop_animation(*animation, AnimationStopMode::Finish));
  driver.flush();

  const auto commands = tfsupport::apcs(wire);
  REQUIRE(commands.size() == 1);
  CHECK(tfsupport::key_value(commands[0], "a") == "a");
  CHECK(tfsupport::key_value(commands[0], "s") == "1");
  CHECK(tfsupport::key_value(commands[0], "c") == "8");
  CHECK(count_action(wire, "t") == 0);
  CHECK(count_action(wire, "f") == 0);
  CHECK(driver.last_frame_bytes().image_transmit == 0);
  CHECK(driver.last_frame_bytes().image_edit ==
        driver.last_frame_bytes().total());
}

TEST_CASE("OBSCURA dissolve: invalidation resumes the current integer frame",
          "[obscura][dissolve][lifecycle][wire]") {
  for (const std::size_t step : std::array<std::size_t, 3>{3, 8, 12}) {
    CAPTURE(step);
    KittyDriver driver;
    driver.set_image_animation_support(true);
    std::string wire;
    driver.set_output(&wire);
    const auto images = reveal_images();
    const auto frames = reveal_frames(images);
    const auto original = driver.register_animation(frames);
    REQUIRE(original);
    driver.flush();
    for (std::size_t reply = 0; reply < kRevealSteps; ++reply)
      driver.consume_reply(TerminalReply{original->id, std::nullopt, "OK"});

    wire.clear();
    driver.invalidate_images();
    CHECK(driver.residency() == ImageResidency{});
    CHECK_FALSE(driver.animation_status(
        *original, std::chrono::steady_clock::time_point{}));
    CHECK(wire.empty());

    const auto replacement = driver.register_animation(frames);
    REQUIRE(replacement);
    driver.flush();
    for (std::size_t reply = 0; reply < kRevealSteps; ++reply)
      driver.consume_reply(TerminalReply{replacement->id, std::nullopt, "OK"});
    const std::size_t frame = visual_for_step(step).reveal_frame;
    REQUIRE(driver.seek_animation(*replacement, frame,
                                  std::chrono::steady_clock::time_point{}));
    REQUIRE(driver.draw_animation(kPlateCells, *replacement, kPlatePlacement));
    driver.flush();

    CHECK(count_action(wire, "t") == 1);
    CHECK(count_action(wire, "f") == 7);
    CHECK(count_action(wire, "d") == 0);
    bool selected = false;
    for (const auto& command : tfsupport::apcs(wire)) {
      if (tfsupport::key_value(command, "a") != "a") continue;
      if (tfsupport::key_value(command, "c") == std::to_string(frame + 1))
        selected = true;
    }
    CHECK(selected);
  }
}

TEST_CASE("OBSCURA dissolve: one upload and seven edits stay under 40 KiB",
          "[obscura][dissolve][bytes][kitty]") {
  KittyDriver driver;
  driver.set_image_animation_support(true);
  std::string wire;
  driver.set_output(&wire);
  const auto images = reveal_images();
  const auto frames = reveal_frames(images);
  const auto animation = driver.register_animation(frames);
  REQUIRE(animation);
  driver.flush();

  CHECK(count_action(wire, "t") == 1);
  CHECK(count_action(wire, "f") == 7);
  CHECK(tfsupport::reassemble(wire).size() == kRevealPayloadBytes);
  CHECK(driver.last_frame_bytes().image_transmit > 0);
  CHECK(driver.last_frame_bytes().image_edit > 0);
  CHECK(driver.residency().source_payload_bytes == kRevealPayloadBytes);

  for (std::size_t reply = 0; reply < kRevealSteps; ++reply)
    driver.consume_reply(TerminalReply{animation->id, std::nullopt, "OK"});
  REQUIRE(driver.take_driver_events().empty());
  REQUIRE(driver.animation_status(*animation,
                                  std::chrono::steady_clock::time_point{}));
  CHECK(
      driver
          .animation_status(*animation, std::chrono::steady_clock::time_point{})
          ->state == AnimationRunState::Stopped);

  const std::size_t registration_end = wire.size();
  REQUIRE(driver.play_animation(*animation, AnimationPlayMode::Once,
                                AnimationReplay::Restart,
                                std::chrono::steady_clock::time_point{}));

  Renderer renderer{driver};
  Screen screen{26, 14};
  for (std::size_t step = 0; step < kDissolveSteps; ++step) {
    paint_composition(screen, visual_for_step(step));
    renderer.present(screen);
    if (step == 0)
      REQUIRE(driver.draw_animation(kPlateCells, *animation, kPlatePlacement));
    else
      REQUIRE(
          driver.retain_animation(kPlateCells, *animation, kPlatePlacement));
    renderer.flush();
  }

  const std::string_view playback{wire.data() + registration_end,
                                  wire.size() - registration_end};
  CHECK(count_action(playback, "t") == 0);
  CHECK(count_action(playback, "f") == 0);
  CHECK(driver.total_bytes().total() == wire.size());
  CHECK(driver.total_bytes().total() <= 40U * 1024U);
}
