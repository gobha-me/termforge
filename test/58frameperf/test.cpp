// test/58frameperf — correctness pins for #89's non-SIMD frame-time kernels.
// Timings belong to termforge_bench; this suite pins the byte/security/cache
// behavior whose corruption could otherwise look like a performance win.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "detail/payload_hash.hpp"
#include "detail/sanitize.hpp"
#include "support/terminal_grid.hpp"
#include "termforge/core/text.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"

using namespace termforge;

namespace {

auto patterned(std::size_t size) -> std::vector<std::byte> {
  std::vector<std::byte> bytes(size);
  for (std::size_t i = 0; i < size; ++i)
    bytes[i] = static_cast<std::byte>((i * 131U + 17U) & 0xFFU);
  return bytes;
}

template <typename Driver>
auto adjacent_text() -> std::string {
  Driver driver;
  std::string out;
  driver.set_output(&out);
  driver.draw_text(0, 0, "A", Rgb{1, 2, 3}, Rgb{4, 5, 6}, Attr::None);
  driver.draw_text(1, 0, "B", Rgb{1, 2, 3}, Rgb{4, 5, 6}, Attr::None);
  driver.draw_text(4, 0, "C", Rgb{1, 2, 3}, Rgb{4, 5, 6}, Attr::None);
  driver.flush();
  return out;
}

}  // namespace

TEST_CASE("payload hash: tails, metadata and every payload byte carry identity",
          "[frameperf][hash]") {
  std::set<std::uint64_t> tails;
  for (std::size_t size = 0; size <= 31; ++size) {
    const auto bytes = patterned(size);
    const auto hash = detail::payload_hash(bytes, Extent{17, 19}, 32);
    INFO("size " << size);
    REQUIRE(hash != 0);
    REQUIRE(detail::payload_hash(bytes, Extent{17, 19}, 32) == hash);
    REQUIRE(tails.insert(hash).second);
  }

  auto bytes = patterned(257);
  const auto original = detail::payload_hash(bytes, Extent{17, 19}, 32);
  REQUIRE(detail::payload_hash(bytes, Extent{18, 19}, 32) != original);
  REQUIRE(detail::payload_hash(bytes, Extent{17, 20}, 32) != original);
  REQUIRE(detail::payload_hash(bytes, Extent{17, 19}, 100) != original);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const auto saved = bytes[i];
    bytes[i] ^= std::byte{0x80};
    INFO("byte " << i);
    REQUIRE(detail::payload_hash(bytes, Extent{17, 19}, 32) != original);
    bytes[i] = saved;
  }
}

TEST_CASE("sanitize borrow predicate is exactly Strip's identity predicate",
          "[frameperf][text][security]") {
  for (unsigned first = 0; first <= 0xFF; ++first) {
    for (unsigned second = 0; second <= 0xFF; ++second) {
      const char raw[] = {static_cast<char>(first), static_cast<char>(second)};
      const std::string_view input{raw, sizeof(raw)};
      INFO("bytes " << first << ' ' << second);
      REQUIRE(detail::is_strip_sanitized(input) ==
              (text::sanitize(input) == input));
    }
  }
  for (const std::string_view input : {"plain ASCII", "é", "世", "é"}) {
    REQUIRE(detail::is_strip_sanitized(input));
    REQUIRE(text::sanitize(input) == input);
  }
}

TEST_CASE("text drivers omit only the adjacent CUP and share decimal spelling",
          "[frameperf][drivers]") {
  for (const auto& out : {adjacent_text<FallbackDriver>(),
                          adjacent_text<AnsiRgbDriver>(),
                          adjacent_text<KittyDriver>()}) {
    INFO(out);
    REQUIRE(out.find("\033[1;1H") != std::string::npos);
    REQUIRE(out.find("\033[1;2H") == std::string::npos);
    REQUIRE(out.find("\033[1;5H") != std::string::npos);
    REQUIRE(out.find("AB") != std::string::npos);
    REQUIRE(out.find('C') != std::string::npos);
    tfsupport::TerminalGrid grid{8, 2};
    grid.feed(out);
    REQUIRE(grid.at(0, 0).text == "A");
    REQUIRE(grid.at(1, 0).text == "B");
    REQUIRE(grid.at(4, 0).text == "C");
  }
}

TEST_CASE("cursor runs account for wide and combining graphemes and stop at flush",
          "[frameperf][drivers][width]") {
  FallbackDriver driver;
  std::string out;
  driver.set_output(&out);
  driver.draw_text(0, 0, "世", {}, {}, Attr::None);
  driver.draw_text(2, 0, "é", {}, {}, Attr::None);
  driver.draw_text(3, 0, "X", {}, {}, Attr::None);
  driver.flush();
  REQUIRE(out.find("\033[1;3H") == std::string::npos);
  REQUIRE(out.find("\033[1;4H") == std::string::npos);

  out.clear();
  driver.draw_text(4, 0, "Y", {}, {}, Attr::None);
  driver.flush();
  REQUIRE(out.find("\033[1;5H") != std::string::npos);
}

TEST_CASE("an image cursor move prevents a false adjacent text run",
          "[frameperf][drivers][image]") {
  FallbackDriver driver;
  std::string out;
  driver.set_output(&out);
  driver.draw_text(0, 0, "A", {}, {}, Attr::None);
  const Image image{1, 1, {Pixel{255, 255, 255, 255}}};
  REQUIRE(driver.draw_image(Rect{4, 0, 1, 1}, image).has_value());
  driver.draw_text(0, 1, "B", {}, {}, Attr::None);
  driver.flush();
  REQUIRE(out.find("\033[1;5H") != std::string::npos);
  REQUIRE(out.find("\033[2;1H") != std::string::npos);
}
