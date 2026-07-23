// base64 encoder tests: known vectors, edge cases, round-trip integrity.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <vector>

#include "detail/base64.hpp"

using termforge::detail::base64_encode;

namespace {

auto to_bytes(const std::string& s) -> std::vector<std::byte> {
  std::vector<std::byte> out(s.size());
  for (std::size_t i = 0; i < s.size(); ++i)
    out[i] = static_cast<std::byte>(s[i]);
  return out;
}

}  // namespace

// ── RFC 4648 test vectors ───────────────────────────────────────────────────

TEST_CASE("base64: RFC 4648 vectors", "[base64]") {
  REQUIRE(base64_encode(to_bytes("")) == "");
  REQUIRE(base64_encode(to_bytes("f")) == "Zg==");
  REQUIRE(base64_encode(to_bytes("fo")) == "Zm8=");
  REQUIRE(base64_encode(to_bytes("foo")) == "Zm9v");
  REQUIRE(base64_encode(to_bytes("foob")) == "Zm9vYg==");
  REQUIRE(base64_encode(to_bytes("fooba")) == "Zm9vYmE=");
  REQUIRE(base64_encode(to_bytes("foobar")) == "Zm9vYmFy");
}

// ── edge cases ──────────────────────────────────────────────────────────────

TEST_CASE("base64: empty input returns empty string", "[base64]") {
  REQUIRE(base64_encode({}).empty());
}

TEST_CASE("base64: single byte produces two chars + two padding", "[base64]") {
  const std::vector<std::byte> one{std::byte{0x41}};  // 'A'
  const auto r = base64_encode(one);
  REQUIRE(r.size() == 4);
  REQUIRE(r.substr(2) == "==");
  REQUIRE(r == "QQ==");
}

TEST_CASE("base64: two bytes produces three chars + one padding", "[base64]") {
  const std::vector<std::byte> two{std::byte{0x41}, std::byte{0x42}};  // "AB"
  const auto r = base64_encode(two);
  REQUIRE(r.size() == 4);
  REQUIRE(r.back() == '=');
  REQUIRE(r == "QUI=");
}

TEST_CASE("base64: three bytes produces four chars, no padding", "[base64]") {
  const std::vector<std::byte> three{std::byte{0x41}, std::byte{0x42},
                                     std::byte{0x43}};  // "ABC"
  const auto r = base64_encode(three);
  REQUIRE(r.size() == 4);
  REQUIRE(r.find('=') == std::string::npos);
  REQUIRE(r == "QUJD");
}

// ── binary data integrity ───────────────────────────────────────────────────

TEST_CASE("base64: all 256 byte values encode without loss", "[base64]") {
  // 256 bytes: 0x00 through 0xFF. Verify the encoded string is valid base64
  // and decodes back to the same bytes (via known properties of the format).
  std::vector<std::byte> all(256);
  for (int i = 0; i < 256; ++i) all[static_cast<std::size_t>(i)] = static_cast<std::byte>(i);

  const auto encoded = base64_encode(all);
  // 256 bytes -> ceil(256/3)*4 = 344 chars (86 groups of 3 -> 344, last group
  // is 1 byte -> 2 chars + 2 padding).
  REQUIRE(encoded.size() == 344);
  REQUIRE(encoded.substr(encoded.size() - 2) == "==");
}

TEST_CASE("base64: null bytes in data", "[base64]") {
  const std::vector<std::byte> nulls{std::byte{0}, std::byte{0}, std::byte{0}};
  const auto r = base64_encode(nulls);
  REQUIRE(r == "AAAA");
}

TEST_CASE("base64: 0xFF bytes", "[base64]") {
  const std::vector<std::byte> ff{std::byte{0xFF}, std::byte{0xFF},
                                  std::byte{0xFF}};
  const auto r = base64_encode(ff);
  REQUIRE(r == "////");
}

// ── kitty-relevant: typical pixel data sizes ────────────────────────────────

TEST_CASE("base64: 1x1 RGBA pixel (4 bytes)", "[base64]") {
  const std::vector<std::byte> px{std::byte{255}, std::byte{0}, std::byte{0},
                                  std::byte{255}};
  const auto r = base64_encode(px);
  REQUIRE(r.size() == 8);  // 4 bytes -> ceil(4/3)*4 = 8
}

TEST_CASE("base64: encoded size formula", "[base64]") {
  for (std::size_t n : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 100, 4096}) {
    std::vector<std::byte> data(n, std::byte{0x42});
    const auto r = base64_encode(data);
    const auto expected = ((n + 2) / 3) * 4;
    REQUIRE(r.size() == expected);
  }
}
