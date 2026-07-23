// ImageLoader tests: valid loads, every failure mode, pixel round-trip.
// All offline — no live TTY needed.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>

#include "termforge/core/image_loader.hpp"

using termforge::ErrorEvent;
using termforge::Image;
using termforge::ImageLoader;
using termforge::Pixel;
using termforge::Severity;

namespace {

// Build a raw-RGBA asset buffer in memory.
auto make_asset(std::uint32_t w, std::uint32_t h,
                const std::vector<Pixel>& pixels) -> std::string {
  std::string out(8 + w * h * 4, '\0');
  std::memcpy(out.data(), &w, 4);
  std::memcpy(out.data() + 4, &h, 4);
  if (!pixels.empty())
    std::memcpy(out.data() + 8, pixels.data(), pixels.size() * sizeof(Pixel));
  return out;
}

}  // namespace

// ── happy path ──────────────────────────────────────────────────────────────

TEST_CASE("ImageLoader: valid 2x1 image loads with correct pixels", "[imageloader]") {
  const std::vector<Pixel> px = {
      Pixel{255, 0, 0, 255},  // red
      Pixel{0, 255, 0, 128},  // green, half-alpha
  };
  const auto data = make_asset(2, 1, px);
  auto r = ImageLoader::load_from_memory(data);
  REQUIRE(r.has_value());
  REQUIRE(r->width() == 2);
  REQUIRE(r->height() == 1);
  REQUIRE(r->at(0, 0) == Pixel{255, 0, 0, 255});
  REQUIRE(r->at(1, 0) == Pixel{0, 255, 0, 128});
}

TEST_CASE("ImageLoader: 1x1 single pixel round-trip", "[imageloader]") {
  const std::vector<Pixel> px = {Pixel{0xAB, 0xCD, 0xEF, 0x42}};
  auto r = ImageLoader::load_from_memory(make_asset(1, 1, px));
  REQUIRE(r.has_value());
  REQUIRE(r->at(0, 0) == Pixel{0xAB, 0xCD, 0xEF, 0x42});
}

// ── failure modes ───────────────────────────────────────────────────────────

TEST_CASE("ImageLoader: empty input is a warning, not a crash", "[imageloader][failure]") {
  auto r = ImageLoader::load_from_memory("");
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().severity == Severity::Warning);
  REQUIRE(r.error().source == "image_loader");
}

TEST_CASE("ImageLoader: header too short", "[imageloader][failure]") {
  auto r = ImageLoader::load_from_memory("\x01\x00\x00");  // 3 bytes
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().message.find("header too short") != std::string::npos);
}

TEST_CASE("ImageLoader: zero width rejected", "[imageloader][failure]") {
  const auto data = make_asset(0, 4, {});
  auto r = ImageLoader::load_from_memory(data);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().message.find("invalid dimensions") != std::string::npos);
}

TEST_CASE("ImageLoader: zero height rejected", "[imageloader][failure]") {
  const auto data = make_asset(4, 0, {});
  auto r = ImageLoader::load_from_memory(data);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().message.find("invalid dimensions") != std::string::npos);
}

TEST_CASE("ImageLoader: oversized dimensions rejected", "[imageloader][failure]") {
  // Craft a header claiming 4097×1 — one over the max.
  std::string data(8, '\0');
  const std::uint32_t w = ImageLoader::kMaxDimension + 1, h = 1;
  std::memcpy(data.data(), &w, 4);
  std::memcpy(data.data() + 4, &h, 4);
  // Pad to match claimed size so we don't fail on size mismatch first.
  data.resize(8 + static_cast<std::size_t>(w) * h * 4, '\0');
  auto r = ImageLoader::load_from_memory(data);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().message.find("too large") != std::string::npos);
}

TEST_CASE("ImageLoader: truncated pixel data detected", "[imageloader][failure]") {
  // Header says 2x2 but only 3 pixels of data follow.
  const auto full = make_asset(2, 2, std::vector<Pixel>(4));
  const auto truncated = full.substr(0, full.size() - 4);  // drop one pixel
  auto r = ImageLoader::load_from_memory(truncated);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().message.find("size mismatch") != std::string::npos);
}

TEST_CASE("ImageLoader: trailing garbage detected", "[imageloader][failure]") {
  auto data = make_asset(1, 1, {Pixel{}});
  data += "EXTRA";  // 5 extra bytes
  auto r = ImageLoader::load_from_memory(data);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().message.find("size mismatch") != std::string::npos);
}

TEST_CASE("ImageLoader: nonexistent file returns error", "[imageloader][failure]") {
  auto r = ImageLoader::load("/nonexistent/path/to/image.rgba");
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().severity == Severity::Warning);
  REQUIRE(r.error().message.find("cannot open") != std::string::npos);
}

// ── pixel data integrity ────────────────────────────────────────────────────

TEST_CASE("ImageLoader: all 256 byte values survive in each channel", "[imageloader]") {
  // 256 pixels: pixel i has r=g=b=a=i. Verifies no byte is mangled.
  std::vector<Pixel> px(256);
  for (int i = 0; i < 256; ++i) {
    const auto b = static_cast<std::uint8_t>(i);
    px[static_cast<std::size_t>(i)] = Pixel{b, b, b, b};
  }
  auto r = ImageLoader::load_from_memory(make_asset(256, 1, px));
  REQUIRE(r.has_value());
  for (int i = 0; i < 256; ++i) {
    const auto b = static_cast<std::uint8_t>(i);
    REQUIRE(r->at(i, 0) == Pixel{b, b, b, b});
  }
}
