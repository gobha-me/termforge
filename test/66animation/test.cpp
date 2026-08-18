// TermForge — terminal-driven image animation registration (#116).
//
// These cases follow production cadence: validate/queue a complete sequence,
// flush exactly once, then observe residency and asynchronous replies. The
// wire assertions distinguish creating a new frame (a=f with NO r=) from the
// root-edit primitive whose r=1 is intentionally tested elsewhere.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "support/apc.hpp"
#include "support/image.hpp"
#include "support/legacy_driver.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/drivers/kitty_driver.hpp"

using namespace std::chrono_literals;
using termforge::AnimationFrame;
using termforge::ByteSink;
using termforge::EncodedImage;
using termforge::ErrorEvent;
using termforge::Extent;
using termforge::Image;
using termforge::ImageFormat;
using termforge::ImageResidency;
using termforge::KittyDriver;
using termforge::Pixel;
using termforge::Severity;
using termforge::TerminalDriver;
using termforge::TerminalReply;

namespace {

auto art(std::uint8_t seed, int w = 2, int h = 2) -> Image {
  return tfsupport::solid(w, h,
                          Pixel{seed, static_cast<std::uint8_t>(seed + 1),
                                static_cast<std::uint8_t>(seed + 2), 255});
}

auto bytes(std::uint8_t seed, std::size_t count = 9) -> std::vector<std::byte> {
  std::vector<std::byte> result(count);
  for (std::size_t i = 0; i < count; ++i)
    result[i] = static_cast<std::byte>(seed + i);
  return result;
}

auto animation_frames(const std::array<Image, 4>& images)
    -> std::array<AnimationFrame, 4> {
  return {AnimationFrame{images[0], 20ms}, AnimationFrame{images[1], 35ms},
          AnimationFrame{images[2], 0ms}, AnimationFrame{images[3], 45ms}};
}

class FailingSink final : public ByteSink {
 public:
  auto write(std::span<const char>)
      -> std::expected<void, ErrorEvent> override {
    return std::unexpected{
        ErrorEvent{Severity::Error, "sink", "animation frame refused"}};
  }
};

} // namespace

TEST_CASE("animation: one root and ordered new frames carry exact gaps",
          "[animation][kitty][wire]") {
  KittyDriver d;
  d.set_image_animation_support(true);
  std::string out;
  d.set_output(&out);

  const std::array images{art(1), art(10), art(20), art(30)};
  const auto frames = animation_frames(images);
  const auto handle = d.register_animation(frames);
  REQUIRE(handle);
  CHECK(d.residency() == ImageResidency{}); // queued is not accepted
  d.flush();

  const auto commands = tfsupport::apcs(out);
  REQUIRE(commands.size() == 5); // root, root gap, then three frames
  CHECK(tfsupport::key_value(commands[0], "a") == "t");
  CHECK(tfsupport::key_value(commands[0], "i") == std::to_string(handle->id));
  CHECK(tfsupport::key_value(commands[1], "a") == "a");
  CHECK(tfsupport::key_value(commands[1], "r") == "1");
  CHECK(tfsupport::key_value(commands[1], "z") == "20");

  const std::array expected_gaps{"35", "-1", "45"};
  for (std::size_t i = 0; i < expected_gaps.size(); ++i) {
    const auto& command = commands[i + 2];
    CHECK(tfsupport::key_value(command, "a") == "f");
    CHECK(tfsupport::key_value(command, "i") == std::to_string(handle->id));
    CHECK(tfsupport::key_value(command, "z") == expected_gaps[i]);
    CHECK(tfsupport::key_value(command, "X") == "1");
    CHECK_FALSE(tfsupport::has_key(command, "r"));
  }

  std::vector<std::byte> expected;
  for (const auto& image : images) {
    const auto raw = std::as_bytes(image.pixels());
    expected.insert(expected.end(), raw.begin(), raw.end());
  }
  CHECK(tfsupport::reassemble(out) == expected);

  CHECK(d.residency() == ImageResidency{0, 1, expected.size()});
  CHECK(d.last_frame_bytes().image_transmit > 0);
  CHECK(d.last_frame_bytes().image_edit > 0); // positive root-gap control
}

TEST_CASE("animation: chunk continuations stay new-frame data without r",
          "[animation][kitty][wire][chunk]") {
  KittyDriver d;
  d.set_image_animation_support(true);
  std::string out;
  d.set_output(&out);

  const Image root = art(1, 50, 25);
  const Image next = art(2, 50, 25); // 5000 bytes: multiple base64 chunks
  const std::array frames{AnimationFrame{root, 10ms},
                          AnimationFrame{next, 10ms}};
  REQUIRE(d.register_animation(frames));
  d.flush();

  int frame_chunks = 0;
  for (const auto& command : tfsupport::apcs(out)) {
    if (tfsupport::key_value(command, "a") != "f") continue;
    ++frame_chunks;
    CHECK_FALSE(tfsupport::has_key(command, "r"));
  }
  REQUIRE(frame_chunks >= 2);
  const auto decoded = tfsupport::reassemble(out);
  CHECK(decoded.size() == 10'000);
}

TEST_CASE("animation: Rgb24 frames are verbatim and need no replies",
          "[animation][kitty][rgb24]") {
  KittyDriver d;
  d.set_image_animation_support(true);
  std::string out;
  d.set_output(&out);
  const auto first = bytes(1, 2 * 2 * 3);
  const auto second = bytes(30, 2 * 2 * 3);
  const std::array frames{
      AnimationFrame{EncodedImage{ImageFormat::Rgb24, first, Extent{2, 2}},
                     10ms},
      AnimationFrame{EncodedImage{ImageFormat::Rgb24, second, Extent{2, 2}},
                     0ms}};

  REQUIRE(d.register_animation(frames));
  d.flush();
  const auto chunks = tfsupport::transmit_chunks(tfsupport::apcs(out));
  REQUIRE(chunks.size() == 2);
  for (const auto& chunk : chunks) {
    CHECK(tfsupport::key_value(chunk, "f") == "24");
    CHECK(tfsupport::key_value(chunk, "q") == "2");
  }
  std::vector<std::byte> expected = first;
  expected.insert(expected.end(), second.begin(), second.end());
  CHECK(tfsupport::reassemble(out) == expected);
  CHECK(d.residency().source_payload_bytes == expected.size());
}

TEST_CASE("animation: identical registrations remain independent",
          "[animation][kitty][identity]") {
  KittyDriver d;
  d.set_image_animation_support(true);
  std::string out;
  d.set_output(&out);
  const std::array images{art(1), art(2), art(3), art(4)};
  const auto frames = animation_frames(images);

  const auto a = d.register_animation(frames);
  const auto b = d.register_animation(frames);
  REQUIRE(a);
  REQUIRE(b);
  CHECK(a->id != b->id);
  CHECK(a->serial != b->serial);
  d.flush();
  CHECK(tfsupport::total_transmits(out) == 2);
  CHECK(d.residency() == ImageResidency{0, 2, 128});
}

TEST_CASE("animation: complete preflight refuses without wire or state",
          "[animation][kitty][failure]") {
  KittyDriver d;
  d.set_image_animation_support(true);
  std::string out;
  d.set_output(&out);

  SECTION("empty sequence") {
    CHECK_FALSE(d.register_animation({}));
  }
  SECTION("empty frame") {
    const Image empty;
    const std::array frames{AnimationFrame{empty, 10ms}};
    CHECK_FALSE(d.register_animation(frames));
  }
  SECTION("negative gap") {
    const Image image = art(1);
    const std::array frames{AnimationFrame{image, -1ms}};
    CHECK_FALSE(d.register_animation(frames));
  }
  SECTION("gap exceeds protocol integer") {
    const Image image = art(1);
    const std::array frames{AnimationFrame{
        image, std::chrono::milliseconds{
                   static_cast<std::int64_t>(
                       std::numeric_limits<std::int32_t>::max()) +
                   1}}};
    CHECK_FALSE(d.register_animation(frames));
  }
  SECTION("extent mismatch") {
    const Image a = art(1);
    const Image b = art(2, 3, 2);
    const std::array frames{AnimationFrame{a, 10ms}, AnimationFrame{b, 10ms}};
    CHECK_FALSE(d.register_animation(frames));
  }
  SECTION("format mismatch") {
    const Image raw = art(1);
    const auto zlib = bytes(9);
    const EncodedImage opaque{ImageFormat::Rgba32Zlib, zlib, Extent{2, 2}};
    const std::array frames{AnimationFrame{raw, 10ms},
                            AnimationFrame{opaque, 10ms}};
    CHECK_FALSE(d.register_animation(frames));
  }
  SECTION("malformed RGBA") {
    const auto raw = bytes(1, 3);
    const EncodedImage malformed{ImageFormat::Rgba32, raw, Extent{2, 2}};
    const std::array frames{AnimationFrame{malformed, 10ms}};
    CHECK_FALSE(d.register_animation(frames));
  }

  CHECK(out.empty());
  d.flush();
  CHECK(d.residency() == ImageResidency{});
}

TEST_CASE("animation: unsupported capability and legacy tier refuse honestly",
          "[animation][compatibility]") {
  const Image image = art(1);
  const std::array frames{AnimationFrame{image, 10ms}};

  KittyDriver kitty;
  std::string out;
  kitty.set_output(&out);
  const auto unsupported = kitty.register_animation(frames);
  REQUIRE_FALSE(unsupported);
  CHECK(unsupported.error().severity == Severity::Warning);
  CHECK(out.empty());

  tfsupport::LegacyDriver legacy;
  TerminalDriver& base = legacy;
  CHECK_FALSE(base.supports_image_animation());
  const auto compatible_default = base.register_animation(frames);
  REQUIRE_FALSE(compatible_default);
  CHECK(compatible_default.error().severity == Severity::Warning);
}

TEST_CASE("animation: zlib bytes are verbatim and every frame is acknowledged",
          "[animation][kitty][zlib][reply]") {
  KittyDriver d;
  d.set_image_animation_support(true);
  std::string out;
  d.set_output(&out);
  const auto a = bytes(10);
  const auto b = bytes(30);
  const std::array frames{
      AnimationFrame{EncodedImage{ImageFormat::Rgba32Zlib, a, Extent{8, 8}},
                     10ms},
      AnimationFrame{EncodedImage{ImageFormat::Rgba32Zlib, b, Extent{8, 8}},
                     0ms}};

  const auto handle = d.register_animation(frames);
  REQUIRE(handle);
  std::vector<std::byte> expected = a;
  expected.insert(expected.end(), b.begin(), b.end());
  d.flush();
  CHECK(tfsupport::reassemble(out) == expected);
  const auto chunks = tfsupport::transmit_chunks(tfsupport::apcs(out));
  REQUIRE(chunks.size() == 2);
  for (const auto& chunk : chunks) {
    CHECK(tfsupport::key_value(chunk, "f") == "32");
    CHECK(tfsupport::key_value(chunk, "o") == "z");
    CHECK(tfsupport::key_value(chunk, "q") == "0");
  }
  CHECK(d.residency() == ImageResidency{0, 1, a.size() + b.size()});

  d.consume_reply(TerminalReply{handle->id, std::nullopt, "OK"});
  CHECK(d.take_driver_events().empty());
  d.consume_reply(TerminalReply{handle->id, std::nullopt, "OK"});
  CHECK(d.take_driver_events().empty());
  CHECK(d.residency() == ImageResidency{0, 1, a.size() + b.size()});
}

TEST_CASE("animation: rejection rolls back and quarantines later replies",
          "[animation][kitty][png][failure]") {
  KittyDriver d;
  d.set_image_animation_support(true);
  std::string out;
  d.set_output(&out);
  const auto a = bytes(10);
  const auto b = bytes(30);
  const std::array frames{
      AnimationFrame{EncodedImage{ImageFormat::Png, a, Extent{8, 8}}, 10ms},
      AnimationFrame{EncodedImage{ImageFormat::Png, b, Extent{8, 8}}, 10ms}};
  const auto rejected = d.register_animation(frames);
  REQUIRE(rejected);
  d.flush();
  REQUIRE(d.residency().pinned_images == 1);

  d.consume_reply(TerminalReply{rejected->id, std::nullopt, "EBADPNG"});
  CHECK(d.residency() == ImageResidency{});
  const auto errors = d.take_driver_events();
  REQUIRE(errors.size() == 1);
  CHECK(errors.front().severity == Severity::Warning);

  const Image raw = art(1);
  const std::array raw_frames{AnimationFrame{raw, 10ms}};
  const auto while_late = d.register_animation(raw_frames);
  REQUIRE(while_late);
  CHECK(while_late->id != rejected->id);
  d.consume_reply(TerminalReply{rejected->id, std::nullopt, "OK"});
  const auto after_late = d.register_animation(raw_frames);
  REQUIRE(after_late);
  CHECK(after_late->id == rejected->id);
}

TEST_CASE("animation: refused sink commits neither registration nor residency",
          "[animation][kitty][sink]") {
  KittyDriver d;
  d.set_image_animation_support(true);
  FailingSink sink;
  d.set_output(&sink);
  const Image image = art(1);
  const std::array frames{AnimationFrame{image, 10ms}};
  const auto first = d.register_animation(frames);
  REQUIRE(first);
  d.flush();
  CHECK(d.residency() == ImageResidency{});
  REQUIRE(d.take_output_error());

  std::string recovered;
  d.set_output(&recovered);
  const auto retry = d.register_animation(frames);
  REQUIRE(retry);
  CHECK(retry->id == first->id); // refused state returned the shared id
}

TEST_CASE("animation: invalidation forgets registrations without wire",
          "[animation][kitty][invalidation]") {
  KittyDriver d;
  d.set_image_animation_support(true);
  std::string out;
  d.set_output(&out);
  const Image image = art(1);
  const std::array frames{AnimationFrame{image, 10ms}};
  const auto first = d.register_animation(frames);
  REQUIRE(first);
  d.flush();
  REQUIRE(d.residency().pinned_images == 1);
  out.clear();

  d.invalidate_images();
  CHECK(out.empty());
  CHECK(d.residency() == ImageResidency{});
  const auto replacement = d.register_animation(frames);
  REQUIRE(replacement);
  CHECK(replacement->id == first->id);
  CHECK(replacement->serial != first->serial);
}

TEST_CASE("animation: all timed-out PNG replies quarantine one id",
          "[animation][kitty][png][timeout]") {
  KittyDriver d;
  d.set_image_animation_support(true);
  std::string out;
  d.set_output(&out);
  const auto a = bytes(10);
  const auto b = bytes(30);
  const std::array frames{
      AnimationFrame{EncodedImage{ImageFormat::Png, a, Extent{8, 8}}, 10ms},
      AnimationFrame{EncodedImage{ImageFormat::Png, b, Extent{8, 8}}, 10ms}};
  const auto timed_out = d.register_animation(frames);
  REQUIRE(timed_out);
  d.flush();
  for (int i = 1; i < 120; ++i)
    d.flush();
  const auto errors = d.take_driver_events();
  REQUIRE(errors.size() == 1);
  CHECK(errors.front().message.find("timed out") != std::string::npos);
  CHECK(d.residency() == ImageResidency{});

  const Image raw = art(1);
  const std::array raw_frames{AnimationFrame{raw, 10ms}};
  const auto before_replies = d.register_animation(raw_frames);
  REQUIRE(before_replies);
  CHECK(before_replies->id != timed_out->id);
  d.consume_reply(TerminalReply{timed_out->id, std::nullopt, "OK"});
  const auto after_one = d.register_animation(raw_frames);
  REQUIRE(after_one);
  CHECK(after_one->id != timed_out->id);
  d.consume_reply(TerminalReply{timed_out->id, std::nullopt, "OK"});
  const auto after_both = d.register_animation(raw_frames);
  REQUIRE(after_both);
  CHECK(after_both->id == timed_out->id);
}

TEST_CASE("animation: registrations and pins share the resident-id budget",
          "[animation][kitty][capacity]") {
  KittyDriver d;
  d.set_image_animation_support(true);
  std::string out;
  d.set_output(&out);
  const Image image = art(1);
  const std::array frames{AnimationFrame{image, 10ms}};
  const auto animation = d.register_animation(frames);
  const auto pin = d.pin_image(image);
  REQUIRE(animation);
  REQUIRE(pin);
  CHECK(animation->id ==
        KittyDriver::kFirstPinnedImageId + KittyDriver::kMaxPinnedImages - 1);
  CHECK(pin->id + 1 == animation->id);
}
