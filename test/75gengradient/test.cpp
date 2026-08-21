// gen_gradient round-trips (#322): 1×1 / 1×N / N×1 / normal assets load via
// ImageLoader with deterministic colours and little-endian headers.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "gen_gradient.hpp"
#include "termforge/core/image_loader.hpp"
#include "termforge/core/types.hpp"

using termforge::ImageLoader;
using termforge::Pixel;

namespace {

namespace fs = std::filesystem;

struct TempRgba {
  fs::path path;
  explicit TempRgba(const char* name) : path(fs::temp_directory_path() / name) {
    fs::remove(path);
  }
  ~TempRgba() { fs::remove(path); }
  auto c_str() const -> std::string { return path.string(); }
};

auto expect_pixel(std::uint32_t x, std::uint32_t y, std::uint32_t w,
                  std::uint32_t h) -> Pixel {
  std::uint8_t raw[4]{};
  gen_gradient::fill_pixel(x, y, w, h, raw);
  return Pixel{raw[0], raw[1], raw[2], raw[3]};
}

auto read_u32_le(const char* p) -> std::uint32_t {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(p[0])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}

auto round_trip(const std::string& path, std::uint32_t w, std::uint32_t h)
    -> void {
  REQUIRE(gen_gradient::write_file(path, w, h));

  // Header bytes are little-endian even on hosts where uint32_t store differs.
  {
    std::ifstream ifs{path, std::ios::binary};
    REQUIRE(ifs);
    char hdr[8]{};
    REQUIRE(static_cast<bool>(ifs.read(hdr, 8)));
    REQUIRE(read_u32_le(hdr) == w);
    REQUIRE(read_u32_le(hdr + 4) == h);
  }

  auto r = ImageLoader::load(path);
  REQUIRE(r.has_value());
  REQUIRE(r->width() == static_cast<int>(w));
  REQUIRE(r->height() == static_cast<int>(h));
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      REQUIRE(r->at(static_cast<int>(x), static_cast<int>(y)) ==
              expect_pixel(x, y, w, h));
    }
  }
}

// Load and assert one pixel against a literal the test states itself. The
// values below are computed by hand from the documented ramp -- r = 255 -
// x*255/x_den, g = x*255/x_den, b = y*255/y_den, a = 255, with x_den = w-1
// (or 1 when w == 1) and likewise for y -- deliberately NOT by calling
// fill_pixel. round_trip() alone cannot catch a wrong ramp, because both its
// sides come from the same function it is meant to check.
auto require_pixel(const std::string& path, int x, int y, Pixel want) -> void {
  auto r = ImageLoader::load(path);
  REQUIRE(r.has_value());
  REQUIRE(r->at(x, y) == want);
}

} // namespace

TEST_CASE("gen_gradient: 1x1 has deterministic colour and loads",
          "[gen_gradient][imageloader]") {
  TempRgba tmp("termforge-gen-gradient-1x1.rgba");
  round_trip(tmp.c_str(), 1, 1);
  // Lone sample is the low end of both ramps: red, no green, no blue.
  require_pixel(tmp.c_str(), 0, 0, Pixel{255, 0, 0, 255});
}

TEST_CASE("gen_gradient: 1xN vertical strip round-trips",
          "[gen_gradient][imageloader]") {
  TempRgba tmp("termforge-gen-gradient-1x8.rgba");
  round_trip(tmp.c_str(), 1, 8);
  // w == 1 collapses the x ramp: red pinned high, green pinned 0 throughout.
  // Blue walks y*255/7: 0 at the top, 109 at y=3, 255 at the bottom.
  require_pixel(tmp.c_str(), 0, 0, Pixel{255, 0, 0, 255});
  require_pixel(tmp.c_str(), 0, 3, Pixel{255, 0, 109, 255});
  require_pixel(tmp.c_str(), 0, 7, Pixel{255, 0, 255, 255});
}

TEST_CASE("gen_gradient: Nx1 horizontal strip round-trips",
          "[gen_gradient][imageloader]") {
  TempRgba tmp("termforge-gen-gradient-8x1.rgba");
  round_trip(tmp.c_str(), 8, 1);
  // h == 1 collapses the y ramp: blue is 0 everywhere. Red/green walk
  // x*255/7 -- 109 at x=3, and a full 0/255 swap at the far edge, which is
  // what pins the divisor to w-1 rather than w.
  require_pixel(tmp.c_str(), 0, 0, Pixel{255, 0, 0, 255});
  require_pixel(tmp.c_str(), 3, 0, Pixel{146, 109, 0, 255});
  require_pixel(tmp.c_str(), 7, 0, Pixel{0, 255, 0, 255});
}

TEST_CASE("gen_gradient: normal-size gradient round-trips",
          "[gen_gradient][imageloader]") {
  TempRgba tmp("termforge-gen-gradient-16x16.rgba");
  round_trip(tmp.c_str(), 16, 16);
  // Four corners pin both ramps; the interior sample has three distinct
  // channels, so an r/g swap cannot hide behind a symmetric value.
  require_pixel(tmp.c_str(), 0, 0, Pixel{255, 0, 0, 255});
  require_pixel(tmp.c_str(), 15, 0, Pixel{0, 255, 0, 255});
  require_pixel(tmp.c_str(), 0, 15, Pixel{255, 0, 255, 255});
  require_pixel(tmp.c_str(), 15, 15, Pixel{0, 255, 255, 255});
  require_pixel(tmp.c_str(), 5, 10, Pixel{170, 85, 170, 255});
}

TEST_CASE("gen_gradient: parse_dimension rejects junk without writing",
          "[gen_gradient][failure]") {
  std::uint32_t v = 99;
  REQUIRE_FALSE(gen_gradient::parse_dimension("", &v));
  REQUIRE_FALSE(gen_gradient::parse_dimension("0", &v));
  REQUIRE_FALSE(gen_gradient::parse_dimension("4097", &v));
  REQUIRE_FALSE(gen_gradient::parse_dimension("12x", &v));
  REQUIRE_FALSE(gen_gradient::parse_dimension("-1", &v));
  REQUIRE(gen_gradient::parse_dimension("64", &v));
  REQUIRE(v == 64);

  TempRgba tmp("termforge-gen-gradient-no-write.rgba");
  // Invalid dims must not leave a successful-looking file behind.
  REQUIRE_FALSE(gen_gradient::write_file(tmp.c_str(), 0, 4));
  REQUIRE_FALSE(fs::exists(tmp.path));
  REQUIRE_FALSE(gen_gradient::write_file(tmp.c_str(), 4, 0));
  REQUIRE_FALSE(fs::exists(tmp.path));
  REQUIRE_FALSE(gen_gradient::write_file(tmp.c_str(), 4097, 4));
  REQUIRE_FALSE(fs::exists(tmp.path));
}

TEST_CASE("gen_gradient: unopenable target reports failure, creates nothing",
          "[gen_gradient][failure]") {
  // A directory that does not exist: the ofstream cannot open, so write_file
  // must report failure rather than return a half-written asset. Valid dims,
  // so this exercises the output-error path and not the dimension guard.
  const auto missing =
      fs::temp_directory_path() / "termforge-gen-gradient-absent-dir";
  fs::remove_all(missing);
  const auto target = missing / "out.rgba";
  REQUIRE_FALSE(gen_gradient::write_file(target.string(), 4, 4));
  REQUIRE_FALSE(fs::exists(target));
  REQUIRE_FALSE(fs::exists(missing));
}
