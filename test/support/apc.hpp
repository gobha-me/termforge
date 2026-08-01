#pragma once

// TermForge test support — reading a kitty graphics stream back off the wire.
//
// Everything here is inline and in `tfsupport` so a suite can pull it into its
// own anonymous namespace, exactly as support/image.hpp is used.
//
// Hoisted out of test/38encoded (#163) when test/39fit (#137) needed the same
// parser. The alternative -- test/01drivers' `out.find("c=2")` style -- cannot
// distinguish a key in a PLACEMENT from the same characters elsewhere in the
// stream, and #137's central assertion is that two keys are ABSENT. A false
// green there is exactly the failure the feature exists to prevent, so the
// suite that checks it parses rather than greps.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tfsupport {

struct Apc {
  std::string keys;     // everything before the ';', e.g. "a=t,t=d,f=100,..."
  std::string payload;  // everything after it (empty for a keys-only command)
  bool has_payload{false};
};

// Every APC (ESC _ G ... ESC \) sequence in `out`, in emission order. Safe to
// scan for naively: the base64 alphabet contains no ESC and no ';', and the
// interleaved cursor positioning is CSI, not APC.
inline auto apcs(std::string_view out) -> std::vector<Apc> {
  std::vector<Apc> found;
  for (std::size_t at = 0;
       (at = out.find("\033_G", at)) != std::string_view::npos;) {
    const std::size_t body = at + 3;
    const std::size_t end = out.find("\033\\", body);
    REQUIRE(end != std::string_view::npos);  // an unterminated APC is a bug
    const std::string_view seq = out.substr(body, end - body);
    const std::size_t semi = seq.find(';');
    Apc a;
    if (semi == std::string_view::npos) {
      a.keys = std::string{seq};
    } else {
      a.keys = std::string{seq.substr(0, semi)};
      a.payload = std::string{seq.substr(semi + 1)};
      a.has_payload = true;
    }
    found.push_back(std::move(a));
    at = end + 2;
  }
  return found;
}

// The transmit chunks only: a=t opens a transmission, and the continuation
// chunks that follow carry m= and nothing else.
inline auto transmit_chunks(const std::vector<Apc>& all) -> std::vector<Apc> {
  std::vector<Apc> out;
  for (const Apc& a : all) {
    const bool opener = a.keys.find("a=t") != std::string::npos;
    const bool continuation = a.has_payload && a.keys.starts_with("m=");
    if (opener || continuation) out.push_back(a);
  }
  return out;
}

// The placement commands only (#137). A placement is `a=p`; every other APC in
// the stream is a transmit, a delete, or a continuation chunk.
inline auto placements(std::string_view out) -> std::vector<Apc> {
  std::vector<Apc> found;
  for (const Apc& a : apcs(out)) {
    if (a.keys.find("a=p") != std::string::npos) found.push_back(a);
  }
  return found;
}

// Whether an APC carries a given key, matched as a whole `key=` token rather
// than as a substring. `has_key(a, "c")` must not be satisfied by the `c` in
// `q=2,c=...`'s neighbour, nor by the `C` of `C=1` -- and must not be fooled
// by `p=1` when asked about `p`... which it would be, so ask precisely.
//
// Keys are comma-separated, so a token starts at the beginning of the key
// string or just after a comma.
inline auto has_key(const Apc& a, std::string_view key) -> bool {
  const std::string needle = std::string{key} + "=";
  for (std::size_t at = 0; (at = a.keys.find(needle, at)) != std::string::npos;
       at += needle.size()) {
    if (at == 0 || a.keys[at - 1] == ',') return true;
  }
  return false;
}

// The value of a key, as text. Empty when the key is absent.
inline auto key_value(const Apc& a, std::string_view key) -> std::string {
  const std::string needle = std::string{key} + "=";
  for (std::size_t at = 0; (at = a.keys.find(needle, at)) != std::string::npos;
       at += needle.size()) {
    if (at != 0 && a.keys[at - 1] != ',') continue;
    const std::size_t from = at + needle.size();
    const std::size_t comma = a.keys.find(',', from);
    return a.keys.substr(from, comma == std::string::npos ? comma
                                                          : comma - from);
  }
  return {};
}

// Independent base64 DECODER, deliberately not detail::base64_encode run
// backwards. Asserting that the emitted payload equals base64_encode(input)
// would put the driver and the test on the same side of the same function; a
// decoder makes the assertion "a terminal reading this gets the bytes the
// application handed us", which is the actual contract. base64 correctness
// itself is test/07base64's job.
inline auto b64_decode(std::string_view s) -> std::vector<std::byte> {
  auto sextet = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;  // '=' padding, or junk
  };
  std::vector<std::byte> out;
  std::uint32_t acc = 0;
  int bits = 0;
  for (const char c : s) {
    const int v = sextet(c);
    if (v < 0) continue;
    acc = (acc << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::byte>((acc >> bits) & 0xFFU));
    }
  }
  return out;
}

// The payloads the terminal reassembles, in order, concatenated.
//
// Decoded PER TRANSMISSION, not over one joined string: each a=t opens an
// independent base64 stream with its own padding, so running two of them
// together through one decoder shifts everything after the first stream's
// pad by a couple of bits. That is a bug in this helper's arithmetic, not in
// the driver -- a terminal decodes each transmission on its own.
inline auto reassemble(std::string_view out) -> std::vector<std::byte> {
  std::vector<std::byte> all;
  std::string stream;
  auto finish = [&] {
    if (stream.empty()) return;
    const auto decoded = b64_decode(stream);
    all.insert(all.end(), decoded.begin(), decoded.end());
    stream.clear();
  };
  for (const Apc& a : transmit_chunks(apcs(out))) {
    if (a.keys.find("a=t") != std::string::npos) finish();  // a new upload
    stream += a.payload;
  }
  finish();
  return all;
}

inline auto count_of(std::string_view hay, std::string_view needle) -> int {
  int n = 0;
  for (std::size_t at = 0;
       (at = hay.find(needle, at)) != std::string_view::npos;
       at += needle.size()) {
    ++n;
  }
  return n;
}

}  // namespace tfsupport
