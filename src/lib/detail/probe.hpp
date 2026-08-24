#pragma once

// TermForge — capability-probe reply classification (pure, offline-testable).
//
// The startup probe (Terminal::query_capabilities) writes a kitty graphics
// query followed by a DA1 request, then reads whatever the terminal sends
// back. Classifying that raw reply is pure string work with no I/O, so it
// lives here where it can be unit-tested without a real terminal (the probe
// itself needs a tty; these predicates do not). See core/terminal.cpp.
//
// The reply is a concatenation of terminal responses, e.g.
//   kitty supported : "\033_Gi=31;OK\033\\"            + "\033[?1u" +
//   "\033[?62;4;22c" kitty error     : "\033_Gi=31;ENOTSUPPORTED\033\\" +
//   "\033[?62c" no kitty        : "\033[?62;22c"
// A graphics response referencing our probe id (i=31) with an OK status,
// arriving *before* the DA1 primary reply, is the support signal. An error
// status (";E...") must NOT count as support — the terminal answered, and its
// answer was "no". Header-only, stdlib-only, no I/O.
//
// Two replies now share the "\033[?" prefix: DA1 (CSI ? ... c) and the kitty
// keyboard-flags report (CSI ? ... u, #60). So "find \033[?" is no longer a
// DA1 locator — find_da1() below checks the *final byte* too, and everything
// that needs DA1's position goes through it.

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace termforge::detail {

namespace probe_detail {

struct Da1Record {
  std::size_t offset;
  std::string_view parameters;
};

// Locate the first complete, syntactically valid DA1 record. APC bodies are
// string data until ST, so CSI-looking bytes inside one must never terminate
// the probe or advertise a capability.
[[nodiscard]] inline auto find_da1_record(std::string_view reply)
    -> std::optional<Da1Record> {
  std::size_t search_from = 0;
  while (true) {
    const auto esc = reply.find('\033', search_from);
    if (esc == std::string_view::npos) return std::nullopt;

    if (esc + 1 < reply.size() && reply[esc + 1] == '_') {
      const auto st = reply.find("\033\\", esc + 2);
      if (st == std::string_view::npos) return std::nullopt;
      search_from = st + 2;
      continue;
    }

    if (esc + 2 >= reply.size() || reply[esc + 1] != '[' ||
        reply[esc + 2] != '?') {
      search_from = esc + 1;
      continue;
    }

    const auto parameters_at = esc + 3;
    std::size_t i = parameters_at;
    bool field_has_digit = false;
    for (; i < reply.size(); ++i) {
      const char c = reply[i];
      if (c >= '0' && c <= '9') {
        field_has_digit = true;
        continue;
      }
      if (c == ';' && field_has_digit) {
        field_has_digit = false;
        continue;
      }
      if (c == 'c' && field_has_digit) {
        return Da1Record{esc, reply.substr(parameters_at, i - parameters_at)};
      }
      break;
    }
    search_from = esc + 1;
  }
}

[[nodiscard]] inline auto parse_probe_id(std::string_view digits)
    -> std::optional<std::uint32_t> {
  if (digits.empty()) return std::nullopt;
  std::uint32_t value = 0;
  const auto parsed =
      std::from_chars(digits.data(), digits.data() + digits.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size())
    return std::nullopt;
  return value;
}

// A graphics reply is G followed by comma-separated key=value fields, then a
// semicolon and one status token. Treat duplicate ids and malformed fields as
// ambiguous rather than allowing attacker-controlled text to select a tier.
[[nodiscard]] inline auto graphics_reply_ok(std::string_view body,
                                            std::uint32_t probe_id) -> bool {
  if (body.empty() || body.front() != 'G') return false;
  body.remove_prefix(1);

  const auto separator = body.find(';');
  if (separator == std::string_view::npos || body.substr(separator + 1) != "OK")
    return false;

  const auto controls = body.substr(0, separator);
  bool found_id = false;
  std::size_t field_at = 0;
  while (field_at <= controls.size()) {
    const auto comma = controls.find(',', field_at);
    const auto field = controls.substr(field_at, comma == std::string_view::npos
                                                     ? std::string_view::npos
                                                     : comma - field_at);
    const auto equals = field.find('=');
    if (field.empty() || equals == std::string_view::npos || equals == 0 ||
        equals + 1 == field.size() ||
        field.find('=', equals + 1) != std::string_view::npos)
      return false;

    if (field.substr(0, equals) == "i") {
      if (found_id) return false;
      const auto id = parse_probe_id(field.substr(equals + 1));
      if (!id || *id != probe_id) return false;
      found_id = true;
    }

    if (comma == std::string_view::npos) break;
    field_at = comma + 1;
  }
  return found_id;
}

[[nodiscard]] inline auto probe_graphics_ok(std::string_view reply,
                                            std::uint32_t probe_id) -> bool {
  const auto da1 = find_da1_record(reply);
  std::size_t search_from = 0;
  while (true) {
    const auto apc = reply.find("\033_", search_from);
    if (apc == std::string_view::npos || (da1 && apc > da1->offset))
      return false;

    const auto st = reply.find("\033\\", apc + 2);
    if (st == std::string_view::npos) return false;
    if (graphics_reply_ok(reply.substr(apc + 2, st - (apc + 2)), probe_id))
      return true;
    search_from = st + 2;
  }
}

} // namespace probe_detail

// Offset of a complete DA1 primary device-attributes report — CSI ? then only
// non-empty numeric fields separated by ';' then a final 'c' — or npos. Skips
// APC string bodies and CSI ? reports with any other final byte (notably the
// keyboard-flags report, CSI ? <flags> u), so unrelated terminal records
// cannot make text look like DA1.
[[nodiscard]] inline auto find_da1(std::string_view reply) -> std::size_t {
  const auto record = probe_detail::find_da1_record(reply);
  return record ? record->offset : std::string_view::npos;
}

// A complete DA1 primary device-attributes report is present in `reply`.
// DA1 remains the read terminator after adding optional queries: terminals
// process the query stream in order, and one that does not implement a query
// ignores it. Waiting for an answer to an ignored DECRQM would add the full
// probe timeout to every unsupported terminal. As with the keyboard query, a
// reply arriving after DA1 is conservatively missed instead of taxing all
// other sessions.
[[nodiscard]] inline auto probe_da1_complete(std::string_view reply) -> bool {
  return find_da1(reply) != std::string_view::npos;
}

// The terminal answers synchronized-output as supported: DECRQM
// ESC [ ? 2026 $ p earns a DECRPM of the form ESC [ ? 2026 ; 1 $ y (set) or
// ESC [ ? 2026 ; 2 $ y (reset) -- 0 means "not supported", and 3/4 are the
// permanently-set/reset states. Either 1 or 2 means the wire
// carries the mode and a frame can be wrapped in it. The query is
// conservative in a way kitty_keyboard's is not: a terminal that ignores
// the whole thing says nothing, and the reply is marked "no".
[[nodiscard]] inline auto probe_sync_updates(std::string_view reply) -> bool {
  constexpr std::string_view kPrefix = "\033[?2026;";
  const auto rp = reply.find(kPrefix);
  if (rp == std::string_view::npos) return false;
  const auto value = rp + kPrefix.size();
  if (value + 2 >= reply.size()) return false;
  return (reply[value] == '1' || reply[value] == '2') &&
         reply[value + 1] == '$' && reply[value + 2] == 'y';
}

// The kitty keyboard protocol is supported: the terminal answered our flags
// query (CSI ? u) with a flags report, CSI ? <digits> u. *Any* flag value
// counts, including 0 — the signal is that the terminal answered at all. A
// terminal without the protocol ignores the query and says nothing.
[[nodiscard]] inline auto probe_kitty_keyboard(std::string_view reply) -> bool {
  for (auto at = reply.find("\033[?"); at != std::string_view::npos;
       at = reply.find("\033[?", at + 3)) {
    std::size_t i = at + 3;
    for (; i < reply.size() && reply[i] >= '0' && reply[i] <= '9'; ++i) {
    }
    if (i > at + 3 && i < reply.size() && reply[i] == 'u') return true;
  }
  return false;
}

// Kitty graphics is supported: an APC graphics response ("\033_G" ... "\033\\")
// that echoes our probe id (i=31) and carries an OK status, arriving before
// the DA1 reply. A ";E..." error status or a missing/late response is a "no".
[[nodiscard]] inline auto probe_kitty_ok(std::string_view reply) -> bool {
  return probe_detail::probe_graphics_ok(reply, 31);
}

// Kitty image-animation actions are supported: the terminal accepted the
// dedicated a=f probe under an id outside every TermForge driver pool. This is
// deliberately separate from probe_kitty_ok -- a terminal can implement the
// basic transmit/place subset and still ignore animation-frame commands.
//
// Scan every APC rather than only the first because the ordinary i=31 graphics
// query intentionally precedes this one in the same startup response stream.
[[nodiscard]] inline auto probe_kitty_animation(std::string_view reply)
    -> bool {
  return probe_detail::probe_graphics_ok(reply, UINT32_C(4294967295));
}

// Sixel is advertised in the DA1 attribute list (attribute "4").
[[nodiscard]] inline auto probe_sixel(std::string_view reply) -> bool {
  const auto da1 = probe_detail::find_da1_record(reply);
  if (!da1) return false;

  std::size_t field_at = 0;
  while (field_at <= da1->parameters.size()) {
    const auto separator = da1->parameters.find(';', field_at);
    const auto field = da1->parameters.substr(
        field_at, separator == std::string_view::npos ? std::string_view::npos
                                                      : separator - field_at);
    if (field == "4") return true;
    if (separator == std::string_view::npos) break;
    field_at = separator + 1;
  }
  return false;
}

} // namespace termforge::detail
