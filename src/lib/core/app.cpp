#include "termforge/core/app.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <format>
#include <initializer_list>
#include <istream>
#include <limits>
#include <mutex>
#include <ostream>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <unistd.h>

#include "detail/keyboard.hpp"
#include "detail/requirements.hpp"
#include "detail/trace.hpp"
#include "termforge/drivers/fallback_driver.hpp"

namespace termforge {

// Track the active app for SIGWINCH -> resize push. Single-app assumption for
// now (one TUI per process); a registry is overkill at this layer.
namespace {
// Plain pointer store is async-signal-safe; relaxed because we only need the
// write to be indivisible, not ordered. (A non-atomic App* read+written from
// a signal handler would be a data race.)
std::atomic<App*> g_active{nullptr};
void on_winch(int) {
  if (auto* app = g_active.load(std::memory_order_relaxed); app != nullptr)
    app->request_resize();
}

void on_cont(int) {
  if (auto* app = g_active.load(std::memory_order_relaxed); app != nullptr)
    app->request_resume_invalidation();
}

struct ContinueSignalLease {
  struct sigaction prior {};
  bool active{false};
};

ContinueSignalLease g_continue_lease;

auto install_continue_handler() noexcept -> bool {
  if (g_continue_lease.active) return false;

  struct sigaction prior {};
  if (::sigaction(SIGCONT, nullptr, &prior) != 0) return false;

  struct sigaction action {};
  action.sa_handler = on_cont;
  ::sigemptyset(&action.sa_mask);
  // No SA_RESTART: a SIGCONT must wake a demand-mode poll so the invalidation
  // reaches the next frame boundary instead of waiting for unrelated input.
  action.sa_flags = 0;
  if (::sigaction(SIGCONT, &action, nullptr) != 0) return false;

  g_continue_lease.prior = prior;
  g_continue_lease.active = true;
  return true;
}

auto restore_continue_handler() noexcept -> void {
  if (!g_continue_lease.active) return;

  struct sigaction current {};
  if (::sigaction(SIGCONT, nullptr, &current) == 0 &&
      (current.sa_flags & SA_SIGINFO) == 0 && current.sa_handler == on_cont) {
    (void)::sigaction(SIGCONT, &g_continue_lease.prior, nullptr);
  }
  // If somebody installed a newer handler, it owns SIGCONT now and must not be
  // overwritten.  Either way this App no longer owns a lease.
  g_continue_lease = {};
}

// The App-level image pass is an ENHANCEMENT over Widget::draw(), not every
// driver's ability to spell draw_image(). FallbackDriver can turn pixels into
// a luminance ramp for direct callers, but that is not an information-complete
// replacement for a widget's authored cell path. Kitty and ANSI truecolour do
// replace that path with a strictly richer presentation, so they enter; the
// Baseline tier keeps the cells (#108).
[[nodiscard]] auto enhanced_image_path(const TerminalDriver& driver) -> bool {
  const Capabilities caps = driver.capabilities();
  return caps.kitty_graphics || caps.truecolor;
}

[[nodiscard]] auto same_capabilities(const Capabilities& a,
                                     const Capabilities& b) -> bool {
  return a.kitty_graphics == b.kitty_graphics && a.sixel == b.sixel &&
         a.truecolor == b.truecolor && a.color_levels == b.color_levels &&
         a.kitty_keyboard == b.kitty_keyboard &&
         a.sync_updates == b.sync_updates;
}

auto trace_warning(std::string message) -> ErrorEvent {
  return ErrorEvent{Severity::Warning, "trace", std::move(message)};
}

[[nodiscard]] constexpr auto valid_input_capabilities(
    InputCapabilities caps) noexcept -> bool {
  if ((caps.key_repeat || caps.key_release || caps.modifier_transitions) &&
      !caps.key_press)
    return false;
  if (caps.modifier_transitions && !caps.key_release) return false;
  return true;
}

[[nodiscard]] constexpr auto combine_input_capabilities(
    InputCapabilities a, InputCapabilities b) noexcept -> InputCapabilities {
  return {a.key_press || b.key_press, a.key_repeat || b.key_repeat,
          a.key_release || b.key_release,
          a.modifier_transitions || b.modifier_transitions};
}

[[nodiscard]] constexpr auto terminal_input_capabilities(
    const Capabilities& caps, KeyboardMode mode) noexcept -> InputCapabilities {
  const bool enhanced = caps.kitty_keyboard && mode == KeyboardMode::Enhanced;
  return {true, enhanced, enhanced, enhanced};
}

[[nodiscard]] constexpr auto bare_modifier(Key key) noexcept -> bool {
  return key >= Key::LeftShift && key <= Key::RightAlt;
}

[[nodiscard]] constexpr auto valid_key(Key key) noexcept -> bool {
  return key > Key::Unknown && key <= Key::RightAlt;
}

[[nodiscard]] constexpr auto valid_action(KeyAction action) noexcept -> bool {
  return action >= KeyAction::Press && action <= KeyAction::Release;
}

[[nodiscard]] constexpr auto valid_image_invalidation_reason(
    ImageInvalidationReason reason) noexcept -> bool {
  return reason >= ImageInvalidationReason::SuspendResume &&
         reason <= ImageInvalidationReason::TerminalReset;
}

[[nodiscard]] constexpr auto valid_scalar(char32_t ch) noexcept -> bool {
  return ch <= 0x10FFFF && !(ch >= 0xD800 && ch <= 0xDFFF);
}

[[nodiscard]] auto same_source_key(const KeyEvent& a,
                                   const KeyEvent& b) noexcept -> bool {
  return a.key == b.key && a.ch == b.ch;
}

auto validate_source_batch(std::span<const Event> events,
                           InputCapabilities caps,
                           std::vector<KeyEvent>& held,
                           std::string& reason) -> bool {
  auto next_held = held;
  for (const auto& event : events) {
    if (const auto* key = std::get_if<KeyEvent>(&event)) {
      if (!valid_key(key->key) || !valid_action(key->action) ||
          (key->key == Key::Char ? !valid_scalar(key->ch) : key->ch != 0)) {
        reason = "malformed key event";
        return false;
      }
      if (!caps.key_press) {
        reason = "key event exceeds the source's key-press capability";
        return false;
      }
      if (bare_modifier(key->key) && !caps.modifier_transitions) {
        reason = "modifier transition exceeds the source's declared capability";
        return false;
      }
      const auto it = std::find_if(next_held.begin(), next_held.end(),
                                   [&](const KeyEvent& prior) {
                                     return same_source_key(prior, *key);
                                   });
      if (key->action == KeyAction::Press) {
        if (it != next_held.end()) {
          reason = "duplicate key press without an intervening release";
          return false;
        }
        next_held.push_back(*key);
      } else if (key->action == KeyAction::Repeat) {
        if (!caps.key_repeat || it == next_held.end()) {
          reason = "key repeat is unsupported or has no matching press";
          return false;
        }
        *it = *key;
      } else {
        if (!caps.key_release || it == next_held.end()) {
          reason = "key release is unsupported or has no matching press";
          return false;
        }
        next_held.erase(it);
      }
      continue;
    }
    if (const auto* mouse = std::get_if<MouseEvent>(&event)) {
      const bool wheel = mouse->button == -1;
      const bool exactly_one_scroll = mouse->scroll_up != mouse->scroll_down;
      if (mouse->button < -1 || mouse->button > 3 ||
          (wheel && (!exactly_one_scroll || mouse->pressed)) ||
          (!wheel && (mouse->scroll_up || mouse->scroll_down)) ||
          (mouse->button == 3 && mouse->pressed)) {
        reason = "malformed mouse event";
        return false;
      }
      continue;
    }
    if (std::holds_alternative<ResizeEvent>(event)) {
      reason = "ResizeEvent is not an input event; use App::set_size";
      return false;
    }
    if (std::holds_alternative<ImageInvalidatedEvent>(event)) {
      reason = "ImageInvalidatedEvent is not an input event; use "
               "App::invalidate_images or App::post";
      return false;
    }
    if (const auto* error = std::get_if<ErrorEvent>(&event)) {
      if (error->severity < Severity::Info || error->severity > Severity::Error ||
          error->source.empty()) {
        reason = "malformed ErrorEvent";
        return false;
      }
    }
    // PasteEvent is intentionally byte-opaque.  Sanitization remains the
    // renderer's boundary, just as it is for terminal-decoded paste.
  }
  held = std::move(next_held);
  return true;
}

auto input_source_error(std::string message,
                        Severity severity = Severity::Warning) -> ErrorEvent {
  return ErrorEvent{severity, "input_source", std::move(message)};
}
}  // namespace

struct App::RecordingState {
  std::ostream* out{nullptr};
  std::chrono::steady_clock::time_point started{};
  bool header_written{false};
};

struct App::PlaybackState {
  detail::Trace trace;
  std::size_t next{0};
  SyntheticClock clock;
  std::optional<ErrorEvent> failure;
};

App::App() = default;

App::~App() { teardown(); }

auto App::input_capabilities() const noexcept -> InputCapabilities {
  if (m_playback) return m_playback->trace.header.input_capabilities;
  const auto terminal_caps =
      terminal_input_capabilities(m_caps, m_term.keyboard_mode());
  if (!m_event_source) return terminal_caps;

  // A configured source is the declared route between runs; a failed source in
  // a live loop is not.  Replacement stays replacement-only after failure so
  // terminal bytes cannot suddenly duplicate the physical input it replaced.
  const auto source_caps =
      m_event_source_active
          ? m_source_capabilities
          : (!m_loop_active ? m_event_source->capabilities()
                            : InputCapabilities{});
  if (m_event_source_mode == EventSourceMode::ReplaceTerminal) return source_caps;
  return combine_input_capabilities(terminal_caps, source_caps);
}

auto App::set_event_source(std::unique_ptr<EventSource> source,
                           EventSourceMode mode)
    -> std::expected<void, ErrorEvent> {
  if (!source) {
    return std::unexpected{
        input_source_error("set_event_source: source is null")};
  }
  if (mode != EventSourceMode::ReplaceTerminal &&
      mode != EventSourceMode::ComposeTerminal) {
    return std::unexpected{
        input_source_error("set_event_source: mode is invalid")};
  }
  const auto caps = source->capabilities();
  if (!valid_input_capabilities(caps)) {
    return std::unexpected{input_source_error(
        "set_event_source: capability declaration is inconsistent")};
  }

  const bool live_source_session = m_loop_active && !m_playback;
  bool candidate_started{false};
  if (live_source_session) {
    std::expected<void, ErrorEvent> started;
    try {
      started = source->start();
    } catch (const std::exception& e) {
      return std::unexpected{input_source_error(std::format(
          "set_event_source: source threw while starting: {}", e.what()))};
    } catch (...) {
      return std::unexpected{input_source_error(
          "set_event_source: source threw while starting")};
    }
    if (!started) {
      auto error = std::move(started.error());
      error.severity = Severity::Warning;
      if (error.source.empty()) error.source = "input_source";
      return std::unexpected{std::move(error)};
    }
    candidate_started = true;
    if (source->poll_fd() < 0 ||
        !valid_input_capabilities(source->capabilities())) {
      source->stop();
      return std::unexpected{input_source_error(
          "set_event_source: started source has an invalid fd or capabilities")};
    }
  }

  const bool crossed_replacement_boundary =
      mode == EventSourceMode::ReplaceTerminal ||
      (m_event_source &&
       m_event_source_mode == EventSourceMode::ReplaceTerminal);
  if (live_source_session) release_source_keys();
  stop_event_source();
  m_event_source = std::move(source);
  m_event_source_mode = mode;
  m_source_capabilities = m_event_source->capabilities();
  m_event_source_active = candidate_started;
  m_source_woke = false;
  if (live_source_session && crossed_replacement_boundary) {
    m_input.discard_incomplete();
    m_got_bytes = false;
    m_esc_waited = false;
  }
  if (m_in_screen) update_requirements(current_size());
  return {};
}

auto App::clear_event_source() -> void {
  if (!m_event_source) return;
  const bool replaced_terminal =
      m_event_source_mode == EventSourceMode::ReplaceTerminal;
  const bool live_source_session = m_loop_active && !m_playback;
  if (live_source_session) release_source_keys();
  stop_event_source();
  m_event_source.reset();
  m_source_capabilities = {};
  m_source_woke = false;
  if (live_source_session && replaced_terminal) {
    m_input.discard_incomplete();
    m_got_bytes = false;
    m_esc_waited = false;
  }
  if (m_in_screen) update_requirements(current_size());
}

auto App::set_keyboard_mode(KeyboardMode mode) -> void {
  m_term.set_keyboard_mode(mode);
  // Before setup there are no observed facts to evaluate; setup owns the
  // Error-grade startup decision. During a live session, changing away from
  // Enhanced can invalidate a repeat/release floor just as surely as a resize
  // can invalidate a geometry floor, so use the same transition path.
  if (m_in_screen) update_requirements(current_size());
}

auto App::start_recording(std::ostream& out) -> void {
  if (m_loop_active) {
    m_input.push_error(trace_warning("start_recording: the App loop is active"));
    return;
  }
  if (m_playback) {
    m_input.push_error(trace_warning("start_recording: playback is active"));
    return;
  }
  if (m_recording) {
    m_input.push_error(trace_warning("start_recording: a recording is already active"));
    return;
  }
  if (!out.good()) {
    m_input.push_error(trace_warning("start_recording: output stream is not writable"));
    return;
  }
  m_recording = std::make_unique<RecordingState>();
  m_recording->out = &out;
}

auto App::stop_recording() -> void { finish_recording(false); }

auto App::play(std::istream& in) -> std::expected<void, ErrorEvent> {
  if (m_loop_active) {
    return std::unexpected{trace_warning("play: the App loop is active")};
  }
  if (m_recording) {
    return std::unexpected{trace_warning("play: stop the active recording first")};
  }
  auto parsed = detail::read_trace(in);
  if (!parsed) return std::unexpected{std::move(parsed.error())};

  // Validate every payload and every production phase before changing the
  // caller's Terminal or App. A corrupt record at the end of a long file must
  // not produce a partial replay before it is discovered.
  std::uint64_t expected_frame{0};
  bool saw_end{false};
  detail::TracePhase last_phase{detail::TracePhase::FrameStart};
  InputCapabilities source_caps = parsed->header.input_capabilities;
  std::vector<KeyEvent> source_held;
  for (std::size_t i = 0; i < parsed->records.size(); ++i) {
    const auto& record = parsed->records[i];
    switch (record.kind) {
      case detail::TraceKind::Frame:
        if (record.phase != detail::TracePhase::FrameStart ||
            !record.payload.empty() || record.frame != expected_frame++) {
          return std::unexpected{trace_warning("play: frame record order is invalid")};
        }
        last_phase = detail::TracePhase::FrameStart;
        break;
      case detail::TraceKind::Input:
        if ((record.phase != detail::TracePhase::InputPump &&
             record.phase != detail::TracePhase::Wait) ||
            record.frame >= expected_frame) {
          return std::unexpected{trace_warning("play: input record phase is invalid")};
        }
        if (record.phase < last_phase) {
          return std::unexpected{trace_warning("play: record phases are out of order")};
        }
        last_phase = record.phase;
        break;
      case detail::TraceKind::Source:
        if ((record.phase != detail::TracePhase::InputPump &&
             record.phase != detail::TracePhase::Wait) ||
            record.frame >= expected_frame) {
          return std::unexpected{
              trace_warning("play: source-event record phase is invalid")};
        }
        if (record.phase < last_phase)
          return std::unexpected{
              trace_warning("play: record phases are out of order")};
        last_phase = record.phase;
        if (auto event = detail::decode_event(record); !event) {
          return std::unexpected{std::move(event.error())};
        } else {
          std::string reason;
          const std::array<Event, 1> batch{*event};
          if (!validate_source_batch(batch, source_caps, source_held, reason)) {
            return std::unexpected{trace_warning(std::format(
                "play: source event is invalid: {}", reason))};
          }
        }
        break;
      case detail::TraceKind::InputCapabilities:
        if ((record.phase != detail::TracePhase::InputPump &&
             record.phase != detail::TracePhase::Wait) ||
            record.frame >= expected_frame) {
          return std::unexpected{trace_warning(
              "play: input-capability record phase is invalid")};
        }
        if (record.phase < last_phase)
          return std::unexpected{
              trace_warning("play: record phases are out of order")};
        last_phase = record.phase;
        if (auto caps = detail::decode_input_capabilities(record); !caps) {
          return std::unexpected{std::move(caps.error())};
        } else {
          if (source_caps.key_release && !caps->key_release &&
              !source_held.empty()) {
            return std::unexpected{trace_warning(
                "play: input capabilities lost release with held keys")};
          }
          source_caps = *caps;
        }
        break;
      case detail::TraceKind::Resize:
        if (record.phase != detail::TracePhase::FrameStart ||
            record.frame >= expected_frame) {
          return std::unexpected{trace_warning("play: resize record phase is invalid")};
        }
        if (record.phase < last_phase) {
          return std::unexpected{trace_warning("play: record phases are out of order")};
        }
        last_phase = record.phase;
        if (auto size = detail::decode_size(record); !size)
          return std::unexpected{std::move(size.error())};
        break;
      case detail::TraceKind::ImageInvalidation:
        if (record.phase != detail::TracePhase::FrameStart ||
            record.frame >= expected_frame) {
          return std::unexpected{
              trace_warning("play: image-invalidation phase is invalid")};
        }
        if (record.phase < last_phase) {
          return std::unexpected{
              trace_warning("play: record phases are out of order")};
        }
        last_phase = record.phase;
        if (auto event = detail::decode_event(record); !event) {
          return std::unexpected{std::move(event.error())};
        } else if (!std::holds_alternative<ImageInvalidatedEvent>(*event)) {
          return std::unexpected{trace_warning(
              "play: image-invalidation record has the wrong event type")};
        }
        break;
      case detail::TraceKind::Posted:
        if (record.phase != detail::TracePhase::Posted ||
            record.frame >= expected_frame) {
          return std::unexpected{trace_warning("play: posted-event phase is invalid")};
        }
        if (record.phase < last_phase) {
          return std::unexpected{trace_warning("play: record phases are out of order")};
        }
        last_phase = record.phase;
        if (auto event = detail::decode_event(record); !event)
          return std::unexpected{std::move(event.error())};
        break;
      case detail::TraceKind::End:
        if (i + 1 != parsed->records.size() ||
            record.phase != detail::TracePhase::End ||
            record.frame != expected_frame) {
          return std::unexpected{trace_warning("play: end record is invalid")};
        }
        if (auto end = detail::decode_end(record); !end)
          return std::unexpected{std::move(end.error())};
        saw_end = true;
        break;
    }
  }
  if (!saw_end) return std::unexpected{trace_warning("play: trace has no end record")};

  auto playback = std::make_unique<PlaybackState>();
  playback->trace = std::move(*parsed);

  const auto prior_caps = m_term.pushed_capabilities();
  if (prior_caps &&
      !same_capabilities(*prior_caps, playback->trace.header.capabilities)) {
    return std::unexpected{
        trace_warning("play: recorded capabilities conflict with the caller's push")};
  }
  const bool pushed_caps = !prior_caps.has_value();
  if (pushed_caps) {
    if (auto applied = m_term.set_capabilities(playback->trace.header.capabilities);
        !applied)
      return std::unexpected{std::move(applied.error())};
  }

  const auto prior_size = m_pushed_size;
  const bool prior_resize = m_resize_pending.load();
  SyntheticClock* const prior_clock = m_clock;
  Input prior_input = std::move(m_input);
  const bool prior_esc_waited = m_esc_waited;
  const bool prior_got_bytes = m_got_bytes;
  const Capabilities prior_observed_caps = m_caps;
  const std::uint64_t prior_frame_index = m_frame_index;
  const TracePoint prior_trace_point = m_trace_point;
  const bool prior_frame_active = m_frame_active;
  const bool prior_render_requested = m_render_requested;
  auto prior_source_events = std::move(m_source_events);
  auto prior_source_held = std::move(m_source_held);
  const bool prior_source_woke = m_source_woke;

  const auto& initial = playback->trace.header.initial_size;
  m_pushed_size = Size{initial.cols, initial.rows, initial.px_w, initial.px_h};
  // The recorded FrameStart resize record, not applying the header, owns the
  // first ResizeEvent. set_size() would arm a duplicate on every playback.
  m_resize_pending.store(false);
  m_input = Input{};
  m_esc_waited = false;
  m_got_bytes = false;
  m_source_events.clear();
  m_source_held.clear();
  m_source_woke = false;
  m_playback = std::move(playback);
  m_clock = &m_playback->clock;

  auto restore = [&] {
    m_clock = prior_clock;
    m_pushed_size = prior_size;
    m_resize_pending.store(prior_resize);
    m_input = std::move(prior_input);
    m_esc_waited = prior_esc_waited;
    m_got_bytes = prior_got_bytes;
    m_caps = prior_observed_caps;
    m_frame_index = prior_frame_index;
    m_trace_point = prior_trace_point;
    m_frame_active = prior_frame_active;
    m_render_requested = prior_render_requested;
    m_source_events = std::move(prior_source_events);
    m_source_held = std::move(prior_source_held);
    m_source_woke = prior_source_woke;
    m_playback.reset();
    if (pushed_caps) m_term.clear_capabilities();
  };

  try {
    const int result = run();
    std::optional<ErrorEvent> playback_error;
    if (m_playback) {
      playback_error = std::move(m_playback->failure);
      if (!playback_error &&
          m_playback->next != m_playback->trace.records.size()) {
        playback_error = trace_warning("play: application ended before the trace");
      }
    }
    restore();
    if (playback_error) return std::unexpected{std::move(*playback_error)};
    if (result != 0) {
      return std::unexpected{
          ErrorEvent{Severity::Error, "trace", "play: App setup failed"}};
    }
  } catch (...) {
    restore();
    throw;
  }
  return {};
}

auto App::begin_recording_run() -> void {
  if (!m_recording || m_recording->header_written) return;
  m_recording->started = now_steady();
  const auto size = current_size();
  detail::TraceHeader header;
  header.capabilities = m_caps;
  header.input_capabilities = input_capabilities();
  header.initial_size = {size.cols, size.rows, size.px_w, size.px_h};
  if (auto written = detail::write_trace_header(*m_recording->out, header); !written) {
    fail_recording(std::move(written.error()));
    return;
  }
  m_recording->header_written = true;
}

auto App::finish_recording(bool clean) -> void {
  if (!m_recording) return;
  if (!m_recording->header_written) {
    m_recording.reset();
    return;
  }
  detail::TraceRecord end;
  end.kind = detail::TraceKind::End;
  end.phase = detail::TracePhase::End;
  end.frame = m_frame_index + (m_frame_active ? 1U : 0U);
  const auto elapsed = now_steady() - m_recording->started;
  end.offset_ns = elapsed > std::chrono::steady_clock::duration::zero()
                      ? static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
                                .count())
                      : 0;
  end.payload = detail::encode_end(clean ? detail::TraceEnd::Clean
                                         : detail::TraceEnd::Prefix);
  if (auto written = detail::write_trace_record(*m_recording->out, end); !written) {
    auto error = std::move(written.error());
    m_recording.reset();
    // A clean end is written after the last input pump, so queueing this one
    // would strand it until some later run. Finalization remains inside the
    // run_loop() exception guard specifically so the ordinary event callback
    // can report the refusal without weakening terminal restoration.
    if (clean && m_loop_active && !m_frame_active) {
      dispatch_event(error);
    } else {
      m_input.push_error(std::move(error));
    }
    return;
  }
  m_recording.reset();
}

auto App::fail_recording(ErrorEvent error) -> void {
  m_recording.reset();
  m_input.push_error(std::move(error));
}

auto App::record_payload(std::uint8_t kind, TracePoint point,
                         std::vector<std::uint8_t> payload) -> void {
  if (!m_recording || !m_recording->header_written) return;
  auto phase = detail::TracePhase::FrameStart;
  switch (point) {
    case TracePoint::FrameStart: phase = detail::TracePhase::FrameStart; break;
    case TracePoint::InputPump: phase = detail::TracePhase::InputPump; break;
    case TracePoint::Posted: phase = detail::TracePhase::Posted; break;
    case TracePoint::Wait: phase = detail::TracePhase::Wait; break;
    case TracePoint::End: phase = detail::TracePhase::End; break;
  }
  const auto elapsed = now_steady() - m_recording->started;
  detail::TraceRecord record{static_cast<detail::TraceKind>(kind), phase,
                             elapsed > std::chrono::steady_clock::duration::zero()
                                 ? static_cast<std::uint64_t>(
                                       std::chrono::duration_cast<
                                           std::chrono::nanoseconds>(elapsed)
                                           .count())
                                 : 0,
                             m_frame_index, std::move(payload)};
  if (auto written = detail::write_trace_record(*m_recording->out, record); !written)
    fail_recording(std::move(written.error()));
}

auto App::record_frame(std::chrono::steady_clock::time_point frame_start) -> void {
  if (!m_recording || !m_recording->header_written) return;
  const auto elapsed = frame_start - m_recording->started;
  detail::TraceRecord record{detail::TraceKind::Frame,
                             detail::TracePhase::FrameStart,
                             elapsed > std::chrono::steady_clock::duration::zero()
                                 ? static_cast<std::uint64_t>(
                                       std::chrono::duration_cast<
                                           std::chrono::nanoseconds>(elapsed)
                                           .count())
                                 : 0,
                             m_frame_index,
                             {}};
  if (auto written = detail::write_trace_record(*m_recording->out, record); !written)
    fail_recording(std::move(written.error()));
}

auto App::record_input(std::string_view bytes) -> void {
  record_payload(static_cast<std::uint8_t>(detail::TraceKind::Input), m_trace_point,
                 std::vector<std::uint8_t>{bytes.begin(), bytes.end()});
}

auto App::record_resize(Size size) -> void {
  record_payload(static_cast<std::uint8_t>(detail::TraceKind::Resize),
                 TracePoint::FrameStart,
                 detail::encode_size({size.cols, size.rows, size.px_w, size.px_h}));
}

auto App::record_posted(const Event& event) -> void {
  record_payload(static_cast<std::uint8_t>(detail::TraceKind::Posted),
                 TracePoint::Posted, detail::encode_event(event));
}

auto App::record_source_event(const Event& event) -> void {
  record_payload(static_cast<std::uint8_t>(detail::TraceKind::Source),
                 m_trace_point, detail::encode_event(event));
}

auto App::record_input_capabilities(InputCapabilities capabilities) -> void {
  record_payload(
      static_cast<std::uint8_t>(detail::TraceKind::InputCapabilities),
      m_trace_point, detail::encode_input_capabilities(capabilities));
}

auto App::playback_begin_frame() -> void {
  if (!m_playback || m_playback->failure) return;
  if (m_playback->next >= m_playback->trace.records.size()) {
    m_playback->failure = trace_warning("play: trace ended before a frame marker");
    quit();
    return;
  }
  const auto& record = m_playback->trace.records[m_playback->next];
  if (record.kind != detail::TraceKind::Frame || record.frame != m_frame_index) {
    m_playback->failure = trace_warning("play: expected frame marker is missing");
    quit();
    return;
  }
  const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          m_playback->clock.now().time_since_epoch())
                          .count();
  if (record.offset_ns > static_cast<std::uint64_t>(now_ns)) {
    m_playback->clock.advance(std::chrono::nanoseconds{
        record.offset_ns - static_cast<std::uint64_t>(now_ns)});
  }
  ++m_playback->next;
}

auto App::playback_apply_frame_transitions() -> void {
  if (!m_playback || m_playback->failure) return;
  while (m_playback->next < m_playback->trace.records.size()) {
    const auto& record = m_playback->trace.records[m_playback->next];
    if ((record.kind != detail::TraceKind::Resize &&
         record.kind != detail::TraceKind::ImageInvalidation) ||
        record.frame != m_frame_index) {
      break;
    }
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            m_playback->clock.now().time_since_epoch())
                            .count();
    if (record.offset_ns > static_cast<std::uint64_t>(now_ns)) {
      m_playback->clock.advance(std::chrono::nanoseconds{
          record.offset_ns - static_cast<std::uint64_t>(now_ns)});
    }
    if (record.kind == detail::TraceKind::Resize) {
      auto decoded = detail::decode_size(record);
      if (!decoded) {
        m_playback->failure = std::move(decoded.error());
        quit();
        return;
      }
      m_pushed_size = Size{decoded->cols, decoded->rows, decoded->px_w,
                           decoded->px_h};
      m_resize_pending.store(true);
    } else {
      auto event = detail::decode_event(record);
      const auto* invalidated =
          event ? std::get_if<ImageInvalidatedEvent>(&*event) : nullptr;
      if (!event || invalidated == nullptr ||
          !stage_image_invalidation(invalidated->reason)) {
        m_playback->failure =
            event ? trace_warning("image-invalidation record is invalid")
                  : std::move(event.error());
        quit();
        return;
      }
    }
    ++m_playback->next;
  }
}

auto App::playback_feed(TracePoint point) -> int {
  if (!m_playback || m_playback->failure) return 0;
  const detail::TracePhase wanted = point == TracePoint::Wait
                                        ? detail::TracePhase::Wait
                                        : detail::TracePhase::InputPump;
  int total{0};
  bool fed_terminal_bytes{false};
  while (m_playback->next < m_playback->trace.records.size()) {
    const auto& record = m_playback->trace.records[m_playback->next];
    const bool feed_kind =
        record.kind == detail::TraceKind::Input ||
        record.kind == detail::TraceKind::Source ||
        record.kind == detail::TraceKind::InputCapabilities;
    if (!feed_kind || record.phase != wanted || record.frame != m_frame_index) {
      break;
    }
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            m_playback->clock.now().time_since_epoch())
                            .count();
    if (record.offset_ns > static_cast<std::uint64_t>(now_ns)) {
      m_playback->clock.advance(std::chrono::nanoseconds{
          record.offset_ns - static_cast<std::uint64_t>(now_ns)});
    }
    if (record.kind == detail::TraceKind::Input) {
      const auto* chars = reinterpret_cast<const char*>(record.payload.data());
      m_input.feed(std::string_view{chars, record.payload.size()});
      fed_terminal_bytes = fed_terminal_bytes || !record.payload.empty();
      if (record.payload.size() >
          static_cast<std::size_t>(std::numeric_limits<int>::max() - total)) {
        total = std::numeric_limits<int>::max();
      } else {
        total += static_cast<int>(record.payload.size());
      }
    } else if (record.kind == detail::TraceKind::Source) {
      auto event = detail::decode_event(record);
      if (!event) {
        m_playback->failure = std::move(event.error());
        quit();
        return total;
      }
      m_source_events.push_back(std::move(*event));
      if (total < std::numeric_limits<int>::max()) ++total;
    } else {
      auto caps = detail::decode_input_capabilities(record);
      if (!caps) {
        m_playback->failure = std::move(caps.error());
        quit();
        return total;
      }
      m_playback->trace.header.input_capabilities = *caps;
      if (m_in_screen) update_requirements(current_size());
      if (total < std::numeric_limits<int>::max()) ++total;
    }
    ++m_playback->next;
  }
  if (fed_terminal_bytes) m_got_bytes = true;
  return total;
}

auto App::playback_dispatch_posted() -> void {
  if (!m_playback || m_playback->failure) return;
  while (m_playback->next < m_playback->trace.records.size()) {
    const auto& record = m_playback->trace.records[m_playback->next];
    if (record.kind != detail::TraceKind::Posted ||
        record.frame != m_frame_index) {
      break;
    }
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            m_playback->clock.now().time_since_epoch())
                            .count();
    if (record.offset_ns > static_cast<std::uint64_t>(now_ns)) {
      m_playback->clock.advance(std::chrono::nanoseconds{
          record.offset_ns - static_cast<std::uint64_t>(now_ns)});
    }
    auto event = detail::decode_event(record);
    if (!event) {
      m_playback->failure = std::move(event.error());
      quit();
      return;
    }
    ++m_playback->next;
    dispatch_event(*event);
  }
}

auto App::playback_finish_frame() -> void {
  if (!m_playback || m_playback->failure) return;
  if (m_playback->next >= m_playback->trace.records.size()) {
    m_playback->failure = trace_warning("play: trace ended without an end record");
    quit();
    return;
  }
  const auto& record = m_playback->trace.records[m_playback->next];
  if (record.kind == detail::TraceKind::End && record.frame == m_frame_index) {
    auto end = detail::decode_end(record);
    if (!end) {
      m_playback->failure = std::move(end.error());
      quit();
      return;
    }
    ++m_playback->next;
    if (*end == detail::TraceEnd::Clean && m_running) {
      m_playback->failure =
          trace_warning("play: application did not stop at the recorded frame");
    }
    quit();
    return;
  }
  if (!m_running) {
    m_playback->failure =
        trace_warning("play: application stopped before the recorded end");
  }
}

auto App::post(Event event) -> void {
  std::lock_guard lock{m_post_mutex};
  // Allocate before signalling. If allocation throws, no wake claims an event
  // exists; if it succeeds, the queue remains authoritative even when the
  // best-effort byte cannot be added because the pipe is already full.
  m_posted.push_back(std::move(event));
  signal_posted_locked();
}

auto App::stage_image_invalidation(
    ImageInvalidationReason reason) noexcept -> bool {
  if (!valid_image_invalidation_reason(reason)) return false;
  // There is one terminal state to clear.  If several notifications arrive
  // before the application can observe that transition, clearing/re-pinning
  // once is the only safe cadence; report the most recent explanation.
  m_image_invalidation_pending = reason;
  request_render();
  return true;
}

auto App::invalidate_images(ImageInvalidationReason reason)
    -> std::expected<void, ErrorEvent> {
  if (!stage_image_invalidation(reason)) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "app", "invalidate_images: reason is invalid"}};
  }
  return {};
}

auto App::apply_image_invalidation() -> void {
  if (!m_image_invalidation_pending) return;
  const ImageInvalidationReason reason = *m_image_invalidation_pending;
  // Clear before invoking application code.  A handler may immediately
  // request a second transition; that request belongs to the next clean frame
  // rather than being erased on return from this one.
  m_image_invalidation_pending.reset();

  if (m_driver) m_driver->invalidate_images();
  if (m_renderer) m_renderer->invalidate();
  m_pixel_force_repaint = true;

  for (auto& state : m_persistent_pixels) {
    // The terminal and driver no longer know this handle.  Do not call
    // unpin_image on it: that would either warn as stale or, without serial
    // checking, delete a newly recycled stranger.  Widget storage remains the
    // source of truth and recreate asks for it again in this frame.
    state.pin = {};
    state.visible = false;
    state.recreate = true;
    state.pending_content = false;
    state.pending_visible = false;
    state.touched_wire = false;
  }

  record_payload(
      static_cast<std::uint8_t>(detail::TraceKind::ImageInvalidation),
      TracePoint::FrameStart,
      detail::encode_event(Event{ImageInvalidatedEvent{reason}}));

  // System transitions bypass overlays, like ResizeEvent and ErrorEvent.  The
  // driver/Persistent state is already invalid before the callback, so direct
  // pin owners can safely rebuild from their own payloads here.
  request_render();
  on_event(Event{ImageInvalidatedEvent{reason}});
}

auto App::start_event_source() -> std::expected<void, ErrorEvent> {
  if (!m_event_source || m_playback) return {};
  if (m_event_source_active) return {};
  m_source_capabilities = m_event_source->capabilities();
  if (!valid_input_capabilities(m_source_capabilities)) {
    return std::unexpected{input_source_error(
        "event source has an inconsistent capability declaration",
        Severity::Error)};
  }
  std::expected<void, ErrorEvent> started;
  try {
    started = m_event_source->start();
  } catch (const std::exception& e) {
    return std::unexpected{input_source_error(
        std::format("event source threw while starting: {}", e.what()),
        Severity::Error)};
  } catch (...) {
    return std::unexpected{input_source_error(
        "event source threw while starting", Severity::Error)};
  }
  if (!started) {
    auto error = std::move(started.error());
    error.severity = Severity::Error;
    if (error.source.empty()) error.source = "input_source";
    return std::unexpected{std::move(error)};
  }
  m_source_capabilities = m_event_source->capabilities();
  if (m_event_source->poll_fd() < 0 ||
      !valid_input_capabilities(m_source_capabilities)) {
    m_event_source->stop();
    return std::unexpected{input_source_error(
        "started event source has an invalid fd or capabilities",
        Severity::Error)};
  }
  m_event_source_active = true;
  m_source_woke = false;
  m_source_held.clear();
  return {};
}

auto App::stop_event_source() noexcept -> void {
  if (m_event_source && m_event_source_active) m_event_source->stop();
  m_event_source_active = false;
  m_source_woke = false;
  m_source_held.clear();
}

auto App::release_source_keys() -> void {
  for (auto key : m_source_held) {
    key.action = KeyAction::Release;
    m_source_events.emplace_back(key);
    record_source_event(m_source_events.back());
  }
  m_source_held.clear();
}

auto App::apply_source_capabilities(InputCapabilities next) -> void {
  if (next == m_source_capabilities) return;
  const auto prior = m_source_capabilities;
  const bool lost = (prior.key_press && !next.key_press) ||
                    (prior.key_repeat && !next.key_repeat) ||
                    (prior.key_release && !next.key_release) ||
                    (prior.modifier_transitions && !next.modifier_transitions);
  if (prior.key_release && !next.key_release) release_source_keys();
  m_source_capabilities = next;
  record_input_capabilities(input_capabilities());
  m_source_events.emplace_back(input_source_error(
      lost ? "event-source capabilities degraded"
           : "event-source capabilities restored",
      lost ? Severity::Warning : Severity::Info));
  record_source_event(m_source_events.back());
  if (m_in_screen) update_requirements(current_size());
}

auto App::fail_event_source(ErrorEvent error) -> void {
  if (!m_event_source_active) return;
  release_source_keys();
  error.severity = Severity::Warning;
  if (error.source.empty()) error.source = "input_source";
  m_source_events.emplace_back(std::move(error));
  record_source_event(m_source_events.back());
  stop_event_source();
  m_source_capabilities = {};
  record_input_capabilities(input_capabilities());
  if (m_in_screen) update_requirements(current_size());
}

auto App::poll_event_source() -> int {
  if (!m_event_source_active || m_playback) return 0;
  const auto prior_caps = m_source_capabilities;
  std::expected<std::vector<Event>, ErrorEvent> batch;
  try {
    batch = m_event_source->poll();
  } catch (const std::exception& e) {
    fail_event_source(input_source_error(
        std::format("event source threw while polling: {}", e.what())));
    return 1;
  } catch (...) {
    fail_event_source(
        input_source_error("event source threw while polling"));
    return 1;
  }
  if (!batch) {
    fail_event_source(std::move(batch.error()));
    return 1;
  }

  std::string reason;
  auto held = m_source_held;
  if (!validate_source_batch(*batch, prior_caps, held, reason)) {
    fail_event_source(input_source_error(
        std::format("malformed event batch: {}", reason)));
    return 1;
  }
  m_source_held = std::move(held);
  for (auto& event : *batch) {
    record_source_event(event);
    m_source_events.push_back(std::move(event));
  }

  const auto next_caps = m_event_source->capabilities();
  if (!valid_input_capabilities(next_caps)) {
    fail_event_source(input_source_error(
        "event source changed to an inconsistent capability declaration"));
    return 1;
  }
  const bool changed = next_caps != prior_caps;
  apply_source_capabilities(next_caps);
  return static_cast<int>(batch->size()) + (changed ? 1 : 0);
}

auto App::dispatch_source_events() -> void {
  std::deque<Event> ready;
  ready.swap(m_source_events);
  for (const auto& event : ready) dispatch_event(event);
}

auto App::discard_terminal_input() -> int {
  char buf[256];
  int total{0};
  while (true) {
    const int n = read_available(buf, sizeof(buf));
    if (n <= 0) break;
    total += n;
  }
  return total;
}

auto App::open_post_pipe() -> std::expected<void, ErrorEvent> {
  {
    std::lock_guard lock{m_post_mutex};
    if (m_post_read >= 0 && m_post_write >= 0) return {};
  }

  int fds[2]{-1, -1};
  if (::pipe(fds) != 0) {
    const int error = errno;
    return std::unexpected{ErrorEvent{
        Severity::Error, "app",
        std::format("post wake pipe: {}", std::strerror(error))}};
  }

  auto fail = [&](const char* operation) -> std::expected<void, ErrorEvent> {
    const int error = errno;
    ::close(fds[0]);
    ::close(fds[1]);
    return std::unexpected{ErrorEvent{
        Severity::Error, "app",
        std::format("post wake pipe {}: {}", operation, std::strerror(error))}};
  };

  for (const int fd : fds) {
    const int status = ::fcntl(fd, F_GETFL);
    if (status < 0 || ::fcntl(fd, F_SETFL, status | O_NONBLOCK) != 0)
      return fail("nonblocking setup");
    const int descriptor = ::fcntl(fd, F_GETFD);
    if (descriptor < 0 || ::fcntl(fd, F_SETFD, descriptor | FD_CLOEXEC) != 0)
      return fail("close-on-exec setup");
  }

  {
    std::lock_guard lock{m_post_mutex};
    m_post_read = fds[0];
    m_post_write = fds[1];
    if (!m_posted.empty()) signal_posted_locked();
  }
  return {};
}

auto App::close_post_pipe() noexcept -> void {
  std::lock_guard lock{m_post_mutex};
  if (m_post_read >= 0) ::close(m_post_read);
  if (m_post_write >= 0) ::close(m_post_write);
  m_post_read = -1;
  m_post_write = -1;
  m_post_woke = false;
}

auto App::signal_posted_locked() noexcept -> void {
  if (m_post_write < 0) return;
  constexpr char byte{'p'};
  while (true) {
    const ssize_t written = ::write(m_post_write, &byte, 1);
    if (written == 1) return;
    if (written < 0 && errno == EINTR) continue;
    // EAGAIN means the pipe is already readable. Any other failure cannot be
    // repaired from this void cross-thread API, but the queued event remains
    // available at the next frame boundary rather than being dropped.
    return;
  }
}

auto App::drain_post_pipe_locked() noexcept -> void {
  if (m_post_read < 0) return;
  char bytes[256];
  while (true) {
    const ssize_t count = ::read(m_post_read, bytes, sizeof(bytes));
    if (count > 0) continue;
    if (count < 0 && errno == EINTR) continue;
    return;  // dry, closed, or otherwise unusable
  }
}

auto App::pump_posted() -> void {
  if (m_playback) {
    playback_dispatch_posted();
    return;
  }
  std::deque<Event> ready;
  {
    // Swap one frame's snapshot and empty the signalling pipe under the same
    // lock post() uses. A producer arriving after this point writes a fresh
    // byte and its event belongs to the next frame by construction.
    std::lock_guard lock{m_post_mutex};
    ready.swap(m_posted);
    drain_post_pipe_locked();
  }
  for (auto& event : ready) {
    record_posted(event);
    dispatch_event(event);
  }
}

auto App::setup() -> std::expected<void, ErrorEvent> {
  if (auto r = open_post_pipe(); !r) return r;
  if (auto r = m_term.enter_raw(); !r) return r;
  if (auto r = start_event_source(); !r) return r;
  // Probe once, then select the driver from that single result. A probe
  // failure isn't fatal: degrade to the fallback driver on empty caps.
  // A caller that pushed capabilities (a cached tier, a user override) gets
  // them served here without any probe traffic (#181) — query_capabilities()
  // short-circuits on the push itself.
  m_caps = {};
  m_persistent_pixels.clear();
  if (auto r = m_term.query_capabilities(); r) m_caps = *r;
  m_driver = m_term.select_driver(m_caps);
  if (auto r = m_driver->init(); !r) return r;

  const auto size = current_size();
  m_screen = std::make_unique<Screen>(size.cols, size.rows);
  m_renderer = std::make_unique<Renderer>(*m_driver);
  push_cell_pixel_size(size);

  // #91: evaluate the app's floor after probe/driver/size/cell geometry are
  // known, and before the alt-screen. A refusal leaves raw mode entered —
  // run() (or the caller of test_setup) must tear it down — but never paints
  // into a buffer the user will not see.
  if (auto r = check_requirements_startup(size); !r) return r;

  // Degradation is an event: an app that asked for the kitty keyboard
  // protocol on a terminal that hasn't got it is told so, rather than
  // waiting forever for releases that will never arrive (#60). This follows
  // the fatal floor check so a refused startup does not strand a duplicate
  // fallback Info in the input queue for a later run.
  const auto effective_input = input_capabilities();
  if (m_term.keyboard_mode() != KeyboardMode::Legacy &&
      !(effective_input.key_repeat && effective_input.key_release)) {
    if (!m_event_source) {
      if (auto e = detail::keyboard_fallback_event(m_term.keyboard_mode(),
                                                   m_caps.kitty_keyboard))
        m_input.push_error(std::move(*e));
    } else {
      m_input.push_error(ErrorEvent{
          Severity::Info, "keyboard",
          "effective input routes do not provide complete key repeat and "
          "release events; keys may arrive as presses only"});
    }
  }

  m_term.enter_screen();
  m_in_screen = true;
  g_active.store(this, std::memory_order_relaxed);
  std::signal(SIGWINCH, on_winch);
  m_winch_hooked = true;
  if (install_continue_handler()) {
    m_cont_hooked = true;
  } else {
    m_input.push_error(ErrorEvent{
        Severity::Warning, "app",
        "setup: could not install SIGCONT image-invalidation handler"});
  }
  // Reads never block, ever: VMIN=0/VTIME=0 once, here, and the loop never
  // touches termios again. All waiting is done by wait_readable(), which has
  // millisecond granularity where VTIME has only deciseconds. (The old loop
  // toggled VTIME 4-9 times per frame — two syscalls each — to emulate a
  // wait it could only express in 100ms steps.)
  m_term.set_read_timeout(0);
  return {};
}

auto App::teardown() -> void {
  stop_event_source();
  if (m_in_screen) {
    m_term.leave_screen();
    m_in_screen = false;
  }
  m_term.leave_raw();
  // Gated, because run()'s setup-failure path reaches teardown() on a run
  // where setup() never got this far: an unconditional reset would clobber a
  // SIGWINCH disposition an embedding program owns and we never replaced.
  if (m_winch_hooked) {
    std::signal(SIGWINCH, SIG_DFL);
    m_winch_hooked = false;
  }
  if (m_cont_hooked) {
    restore_continue_handler();
    m_cont_hooked = false;
  }
  // Last, and here rather than only in ~App: the handler is unhooked above, so
  // leaving a process-wide pointer to this App behind would serve nothing and
  // outlive teardown on the one path (an exception escaping main) where ~App
  // is never going to run.
  App* expected = this;
  g_active.compare_exchange_strong(expected, nullptr, std::memory_order_relaxed);
  m_resume_invalidation_pending.store(false, std::memory_order_relaxed);
  m_image_invalidation_pending.reset();
  close_post_pipe();
}

auto App::shutdown_driver() -> void {
  // #148: the end-of-session driver handoff, run ONLY from a live loop
  // (run_loop, and the test seam test_run_frames) while the driver's output
  // sink is provably alive -- never from ~App. shutdown() routes what the
  // driver owes the terminal (kitty freeing its resident images) through the
  // session's sink, then detaches that borrowed sink. Deliberately
  // separate from teardown(): teardown() also runs from ~App, where a derived
  // class's sink may already be destroyed and writing through it would be a
  // use-after-free. shutdown() self-guards on repeat.
  if (m_driver) m_driver->shutdown();
}

auto App::run() -> int {
  // setup() is guarded too, not just the loop: it enters raw mode first and
  // *then* allocates — the capability probe builds strings, and the Screen is
  // sized from whatever TIOCGWINSZ reports. A bad_alloc between those two
  // points would escape run() with the terminal raw, which is this issue
  // exactly, one function earlier.
  try {
    if (auto r = setup(); !r) {
      // Undo whatever setup() got through before it failed. A no-op on both of
      // today's failure paths — enter_raw() fails before anything is armed, and
      // driver->init() fails before enter_screen() — but teardown() is
      // idempotent, and the alternative is that a failure point added after
      // enter_screen() someday leaks the alt-screen with nothing to catch it.
      teardown();
      std::fprintf(stderr, "termforge: setup failed: %s\n", r.error().message.c_str());
      return 1;
    }
  } catch (...) {
    teardown();
    throw;
  }
  m_running = true;
  // The tick clock starts at the first frame, not at construction and not at
  // setup(): the capability probe blocks on terminal replies for anywhere from
  // microseconds to a DA1 timeout, and charging the simulation for that would
  // make "how fast did the terminal answer" a gameplay variable.
  m_last_tick.reset();
  m_tick_accum = std::chrono::duration<double>::zero();
  m_frame_index = 0;
  m_frame_active = false;
  m_trace_point = TracePoint::FrameStart;
  m_render_requested = true;
  begin_recording_run();
  return run_loop();
}

auto App::run_loop() -> int {
  // The only exceptions that reach here come from user code — on_event,
  // on_tick, on_render — because the library itself reports failure as an
  // ErrorEvent through std::expected and never throws. run() has no channel
  // to report one through an int, so it restores the terminal and rethrows
  // rather than deciding an application's exception is meaningless.
  //
  // Restoring *here* rather than leaning on ~App is the whole point: for the
  // shape the examples teach (`MyApp app; return app.run();`) an exception
  // with no handler anywhere calls std::terminate without unwinding, so ~App
  // never runs. Before this, the terminal was rescued only by the SIGABRT
  // entry in the fatal-signal backstop — the crash handler doing the work the
  // documented path claimed to.
  //
  // The frame is abandoned mid-flight: a throw from on_render skips
  // present(), restore_backdrop(), flush_pixel_regions() and the frame's
  // single flush(), so the Screen can be left dimmed under an overlay.
  // Harmless because the loop is over — but it is the first thing to fix if
  // catching-and-resuming ever becomes a feature.
  // #97: the terminal is fully up here and no frame has run -- the one point
  // that satisfies on_start()'s contract. A throw is a startup failure: the
  // loop never begins, m_app_started stays false so no on_stop() is owed,
  // and the catch restores the terminal before the exception propagates.
  m_loop_active = true;
  try {
    on_start();
    m_app_started = true;
    if (m_playback && m_playback->next < m_playback->trace.records.size()) {
      const auto& next = m_playback->trace.records[m_playback->next];
      if (next.kind == detail::TraceKind::End &&
          next.frame == m_frame_index) {
        playback_finish_frame();
      }
    }
    while (m_running) frame_step();
    finish_recording(true);
  } catch (...) {
    // No end record: an exception-aborted artifact is intentionally rejected
    // as truncated instead of being advertised as a complete replay.
    m_recording.reset();
    stop_app();
    shutdown_driver();  // #148: sink still alive here; teardown's ~App path is not
    teardown();
    m_loop_active = false;
    throw;
  }
  stop_app();
  shutdown_driver();  // #148: route driver teardown through the live sink
  teardown();
  m_loop_active = false;
  return 0;
}

auto App::frame_step() -> void {
  m_frame_active = true;
  m_trace_point = TracePoint::FrameStart;
  playback_begin_frame();
  // Playback is an isolated source, not a fourth custom virtual override. A
  // consumer may already override now_steady() for its ordinary tests; the
  // trace's clock must still be the production loop's timestamp during play.
  const auto frame_start =
      m_playback ? m_playback->clock.now() : now_steady();
  record_frame(frame_start);
  playback_apply_frame_transitions();
  m_pixel_force_repaint = false;
  if (m_resume_invalidation_pending.exchange(false,
                                             std::memory_order_relaxed)) {
    (void)stage_image_invalidation(ImageInvalidationReason::SuspendResume);
  }
  apply_image_invalidation();
  if (m_resize_pending) {
    // Clear *before* measuring: a SIGWINCH landing between the ioctl and
    // the store would otherwise be erased by it, leaving the screen at a
    // stale size until the next resize. Clear-then-measure re-arms the
    // next iteration instead.
    m_resize_pending.store(false);
    const auto size = current_size();
    record_resize(size);
    m_screen->resize(size.cols, size.rows);
    m_renderer->invalidate();
    // A full cell repaint includes the blank cells under persistent images.
    // Their content is unchanged, but their placement must be emitted again
    // after that diff (especially for Unicode placeholders and ANSI cells).
    m_pixel_force_repaint = true;
    // Before the dispatch, and so before this frame's collect pass: push it
    // after and the first frame of every resize rasterizes at the old cell
    // geometry, which under kitty is a visibly wrong scale.
    push_cell_pixel_size(size);
    // #91: re-evaluate the floor on every resize. Crossing it is an event and
    // a latch that suppresses enhanced submission — not a modal the framework
    // invents, and not a silent downgrade.
    update_requirements(size);
    dispatch_event(ResizeEvent{size.cols, size.rows});
  }
  m_trace_point = TracePoint::InputPump;
  pump_input();
  m_trace_point = TracePoint::Posted;
  pump_posted();
  // After the resize dispatch and the input pump, before the draw: a tick may
  // bound motion by screen().cols()/rows(), and the tick following a keypress
  // must be the tick that acts on it. Drawing then shows the state the tick
  // just produced rather than one frame of stale state.
  tick_step(frame_start);
  const bool requested = std::exchange(m_render_requested, false);
  const bool rendered =
      m_render_mode == RenderMode::Continuous || requested;
  if (rendered) {
    m_pixel_regions.clear();
    for (auto& region : m_persistent_pixels) {
      region.seen = false;
      region.pending_content = false;
      region.pending_visible = false;
      region.touched_wire = false;
    }
    on_render(*m_screen);
    render_overlays(*m_screen);
    m_renderer->present(*m_screen);
    restore_backdrop(*m_screen);  // the overlay pass leaves no trace behind
    // #148: the frame's images queue AFTER its cell diff but in the SAME flush.
    // The order inside the buffer is the one the terminal composites: the cell
    // diff paints text and blanks first, and the image's placeholder/id grid is
    // appended last so it is not overwritten by that diff -- collect_pixel_regions
    // blanked the region's cells so present() emits spaces for them, and those
    // spaces must precede, not follow, the image cells. queueing images last is
    // also what makes "remove-then-write" single-buffer rather than a torn pair:
    // a deletion/re-placement is emitted before the placeholder grid references it.
    // flush_pixel_regions drives kitty's collection on EVERY RENDERED frame; a
    // demand-idle frame deliberately does not touch driver state at all.
    flush_pixel_regions();
    m_renderer->flush();  // #148: ONE write carries the whole rendered frame
    // #178: a sink that refused this frame's bytes surfaces as an ErrorEvent
    // rather than a silently dropped frame. flush() is `-> void` and pure, so
    // the driver latches the refusal and this is where it is read -- after the
    // frame's SINGLE write above, so a frame carrying pixel regions is drained
    // exactly once rather than once per write it used to split into. Queued
    // through the same channel setup() uses for degradations, so it drains on
    // the next frame's pump and dispatch_event routes it past the overlay stack.
    auto output_error = m_driver->take_output_error();
    finish_pixel_frame(!output_error.has_value());
    if (output_error) m_input.push_error(std::move(*output_error));
  }
  m_trace_point = TracePoint::Wait;
  wait_frame(frame_start, rendered);
  ++m_frame_index;
  m_trace_point = TracePoint::End;
  m_frame_active = false;
  playback_finish_frame();
}

auto App::set_tick_hz(int hz) -> void {
  m_tick_hz = hz > 0 ? hz : 0;
  // 1.0, not 1: integer division here would silently pin every period to zero.
  m_tick_dt = m_tick_hz > 0
                  ? std::chrono::duration<double>{1.0 / m_tick_hz}
                  : std::chrono::duration<double>::zero();
  // The carried remainder is denominated in the old timestep — see set_tick_hz.
  m_tick_accum = std::chrono::duration<double>::zero();
}

auto App::tick_step(std::chrono::steady_clock::time_point frame_start) -> void {
  using Seconds = std::chrono::duration<double>;

  // On the first frame of a run there is no previous frame to measure against,
  // so the delta is zero rather than a fabricated frame budget — an integrator
  // handed a made-up dt is wrong in a way nothing downstream can detect,
  // whereas zero advances nothing and is exactly true. It goes through the same
  // path as any other delta, so a fixed timestep simply banks it (i.e. delivers
  // no ticks) instead of leaking one variable-dt call into a constant-dt world.
  Seconds dt = Seconds::zero();
  if (m_last_tick) {
    dt = frame_start - *m_last_tick;
    if (dt < Seconds::zero()) dt = Seconds::zero();  // steady_clock says impossible
    if (m_max_tick_dt > Seconds::zero() && dt > m_max_tick_dt) dt = m_max_tick_dt;
  }
  // Store the RAW stamp, never the clamped one: the clamp is a lie told to the
  // simulation about how much time passed, and folding it back into the clock
  // would compound that lie into permanent drift.
  m_last_tick = frame_start;

  if (m_tick_dt <= Seconds::zero()) {  // variable timestep — the default
    on_tick(dt);
    return;
  }

  // Fixed timestep. The accumulator's input is the clamped delta, so this loop
  // runs at most ceil(max_tick_dt / m_tick_dt) times no matter how long the
  // frame took or how slow on_tick is — see set_tick_hz.
  m_tick_accum += dt;
  while (m_tick_accum >= m_tick_dt) {
    m_tick_accum -= m_tick_dt;
    on_tick(m_tick_dt);
    // quit() from inside a tick ends the catch-up too: the remaining ticks
    // would advance state the app has already declared dead.
    if (!m_running) break;
  }
}

// ── loop seams (overridden in tests to fake the clock and the fd) ──────────

auto App::now_steady() const -> std::chrono::steady_clock::time_point {
  return m_clock ? m_clock->now() : std::chrono::steady_clock::now();
}

auto App::wait_readable(int timeout_ms) -> bool {
  return m_term.wait_readable(timeout_ms);
}

auto App::wait_for_sources(int timeout_ms) -> bool {
  const bool indefinite = timeout_ms < 0;

  int post_fd = -1;
  {
    std::lock_guard lock{m_post_mutex};
    post_fd = m_post_read;
  }
  // Preserve the protected terminal seam for existing headless tests when it
  // is the only possible source.  A configured structured source has its own
  // real readiness fd and must enter the combined poll even without setup's
  // post pipe.
  if (post_fd < 0 && !m_event_source_active) {
    return wait_readable(indefinite ? std::numeric_limits<int>::max()
                                    : timeout_ms);
  }

  // Poll the terminal and the self-pipe in one wait. Deadline handling mirrors
  // Terminal::wait_readable: SIGWINCH or another signal resumes only the
  // remaining budget instead of stretching or abandoning the frame.
  using Clock = std::chrono::steady_clock;
  const auto deadline =
      indefinite ? Clock::time_point::max()
                 : Clock::now() + std::chrono::milliseconds(timeout_ms);
  while (true) {
    std::array<pollfd, 3> fds{};
    nfds_t fd_count{0};
    int terminal_index{-1};
    int post_index{-1};
    int source_index{-1};
    if (m_term.raw() && m_term.io().in >= 0) {
      terminal_index = static_cast<int>(fd_count);
      fds[fd_count++] = {m_term.io().in, POLLIN, 0};
    }
    if (post_fd >= 0) {
      post_index = static_cast<int>(fd_count);
      fds[fd_count++] = {post_fd, POLLIN, 0};
    }
    if (m_event_source_active) {
      source_index = static_cast<int>(fd_count);
      fds[fd_count++] = {m_event_source->poll_fd(), POLLIN, 0};
    }
    if (fd_count == 0) return false;
    const int result = ::poll(fds.data(), fd_count,
                              indefinite ? -1 : timeout_ms);
    if (result > 0) {
      const bool post_ready =
          post_index >= 0 &&
          (fds[static_cast<std::size_t>(post_index)].revents &
           (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0;
      if (post_ready) {
        std::lock_guard lock{m_post_mutex};
        drain_post_pipe_locked();
        m_post_woke = true;
      }
      const bool source_ready =
          source_index >= 0 &&
          (fds[static_cast<std::size_t>(source_index)].revents &
           (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0;
      if (source_ready) m_source_woke = true;
      const bool terminal_ready =
          terminal_index >= 0 &&
          (fds[static_cast<std::size_t>(terminal_index)].revents &
           (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0;
      return post_ready || source_ready || terminal_ready;
    }
    if (result == 0) return false;
    if (errno != EINTR) return false;
    // A resize signal is itself a demand-mode source. Continuous frames keep
    // their historical authoritative budget; demand frames wake promptly or
    // the newly armed resize could be stranded in an indefinite wait.
    if (m_render_mode == RenderMode::Demand && m_resize_pending.load())
      return true;
    if (indefinite) continue;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - Clock::now());
    if (left.count() <= 0) return false;
    timeout_ms = static_cast<int>(left.count());
  }
}

auto App::read_available(char* out, int max) -> int {
  // Production reaches a frame only after setup() put the Terminal into the
  // nonblocking event-loop mode. The public headless hooks deliberately skip
  // setup; treating their caller's cooked stdin as immediately readable turns
  // the first drain into a blocking read. Keep the default source empty there.
  // A test override is still dispatched virtually and can supply bytes without
  // a Terminal at all.
  return m_term.raw() ? m_term.read_input(out, max) : 0;
}

auto App::drain_input() -> int {
  if (m_playback) return playback_feed(m_trace_point);
  // Production reads are non-blocking (VMIN=0/VTIME=0, set once in setup), so
  // this empties whatever the tty has buffered and stops the instant it's dry.
  // The default headless source returns empty before touching an unprepared fd;
  // a read_available() override carries the same nonblocking contract itself.
  char buf[256];
  int total = 0;
  while (true) {
    const int n = read_available(buf, sizeof(buf));
    if (n <= 0) break;
    const std::string_view bytes{buf, static_cast<std::size_t>(n)};
    record_input(bytes);
    m_input.feed(bytes);
    total += n;
  }
  if (total > 0) m_got_bytes = true;
  return total;
}

auto App::wait_frame(std::chrono::steady_clock::time_point frame_start,
                     bool rendered) -> void {
  // The frame's one and only wait. Two rules make the rate hold steady:
  //   * the deadline is absolute, measured from the *start* of the frame, so
  //     rendering time comes out of the budget rather than adding to it;
  //   * input arriving mid-wait is absorbed, not treated as a reason to end
  //     the frame early. Those bytes dispatch at the top of the next frame.
  //     (The old loop returned the moment a byte landed, which is why the
  //     frame rate used to depend on whether the user was typing.)
  // request_resize()/set_size() may be called after this frame's resize point,
  // without a signal to interrupt poll. Demand mode must hand control straight
  // to the next frame so the armed resize is consumed rather than sleeping.
  if (m_render_mode == RenderMode::Demand && m_resize_pending.load()) return;

  const bool demand_idle = m_running && m_render_mode == RenderMode::Demand &&
                           !rendered && !m_render_requested &&
                           !m_input.esc_pending();
  if (demand_idle && !m_playback) {
    // No timer is armed. A real run blocks in the terminal/post poll; a
    // headless override sees INT_MAX as the nonnegative spelling of this wait.
    if (wait_for_sources(-1)) {
      // Bytes are deliberately only absorbed here. They are decoded and
      // dispatched at the next frame's ordinary pump, preserving frame order.
      if (!m_playback && m_event_source &&
          m_event_source_mode == EventSourceMode::ReplaceTerminal)
        (void)discard_terminal_input();
      else
        (void)drain_input();
      if (std::exchange(m_source_woke, false)) (void)poll_event_source();
      (void)std::exchange(m_post_woke, false);
    }
    return;
  }

  auto deadline = frame_start + std::chrono::milliseconds(m_frame_ms);
  // The sanctioned overrun: a half-arrived escape sequence gets kEscGraceMs
  // to finish, even under a tighter budget, or a 16ms frame would chop every
  // arrow key into ESC + '[' + 'A'.
  if (m_input.esc_pending() && !m_esc_waited) {
    const auto grace = frame_start + std::chrono::milliseconds(kEscGraceMs);
    if (grace > deadline) deadline = grace;
    m_esc_waited = true;
  }
  if (m_playback) {
    (void)playback_feed(TracePoint::Wait);
    // The next Frame record carries the observed boundary. Advancing to the
    // configured deadline here would erase real render overruns and poll's
    // millisecond truncation -- precisely the timing the trace is preserving.
    return;
  }
  while (true) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now_steady());
    if (left.count() <= 0) break;
    // A synthetic run preserves the source checks but never lends a real fd
    // its simulated timeout. If nothing is ready now, advancing the borrowed
    // clock spends the entire remaining budget deterministically. If bytes are
    // ready, drain them exactly as the real wait does and poll again at zero;
    // this is what keeps a scripted source from spinning or consuming wall
    // time while retaining the ordinary absorb-now/dispatch-next-frame order.
    const int timeout_ms = m_clock ? 0 : static_cast<int>(left.count());
    if (!wait_for_sources(timeout_ms)) {
      if (m_clock) m_clock->advance(left);
      break;  // real or simulated budget spent
    }
    // Readable but empty means EOF/hangup: stop, or we'd spin on a dead fd
    // for the rest of the budget.
    const int input_bytes =
        !m_playback && m_event_source &&
                m_event_source_mode == EventSourceMode::ReplaceTerminal
            ? discard_terminal_input()
            : drain_input();
    const int source_work = std::exchange(m_source_woke, false)
                                ? poll_event_source()
                                : 0;
    if (std::exchange(m_post_woke, false)) break;
    // Continuous mode absorbs input without shortening the authoritative frame
    // budget. Demand mode is latency-oriented once a source has work: end the
    // wait, then dispatch at the next frame boundary.
    if (m_render_mode == RenderMode::Demand &&
        (input_bytes > 0 || source_work > 0))
      break;
    if (input_bytes == 0 && source_work == 0) break;
  }
}

auto App::test_wire_headless(int cols, int rows, std::string* sink) -> void {
  // The default tier, and the ONLY place it is named (#189). Every other
  // headless path either goes through here or is handed a driver.
  test_wire_headless(cols, rows, sink, std::make_unique<FallbackDriver>());
}

auto App::test_wire_headless(int cols, int rows, std::string* sink,
                             std::unique_ptr<TerminalDriver> driver) -> void {
  // Everything setup() does *except* the parts that need a tty: no enter_raw,
  // no capability probe, no alt-screen, no SIGWINCH handler. The frame body
  // itself is the real one, so cadence and input handling are the shipped
  // code paths and not a reimplementation.
  //
  // The set_output call goes through m_driver -- the BASE pointer -- since
  // #178. It used to have to construct a concrete FallbackDriver, redirect it,
  // and upcast afterwards, because set_output existed only on the concrete
  // drivers; that ordering is why headless tests were pinned to the fallback
  // tier and could never exercise KittyDriver offline. #178 made the tier a
  // free choice; this parameter is what finally offers it (#189).
  //
  // No null fallback. A caller that meant "the default" has the three-argument
  // overload, so a null here is a bug and reading it as FallbackDriver would
  // silently hide it -- and the tier is exactly what such a test is asserting
  // about.
  m_driver = std::move(driver);
  m_persistent_pixels.clear();
  m_driver->set_output(sink);
  m_screen = std::make_unique<Screen>(cols, rows);
  m_renderer = std::make_unique<Renderer>(*m_driver);
}

auto App::test_run_frames(int frames, int cols, int rows, std::string* sink) -> void {
  test_run_frames(frames, cols, rows, sink, std::make_unique<FallbackDriver>());
}

auto App::test_run_frames(int frames, int cols, int rows, std::string* sink,
                          std::unique_ptr<TerminalDriver> driver) -> void {
  test_wire_headless(cols, rows, sink, std::move(driver));
  if (auto started = start_event_source(); !started)
    m_input.push_error(std::move(started.error()));
  m_running = true;
  m_loop_active = true;
  try {
    for (int i = 0; i < frames && m_running; ++i) frame_step();
  } catch (...) {
    stop_event_source();
    m_loop_active = false;
    throw;
  }
  // #148: this seam drives frame_step directly, without run_loop -- so the
  // driver's end-of-session handoff runs here too, while the caller's sink
  // string is still in scope. It emits kitty's d=A and then detaches; bytes
  // already accepted by the sink remain available to the test. A suite that
  // parses the stream accounts for that trailing cleanup write.
  shutdown_driver();
  stop_event_source();
  m_loop_active = false;
}

auto App::test_run_guarded(int cols, int rows, std::string* sink) -> int {
  test_wire_headless(cols, rows, sink);
  if (auto started = start_event_source(); !started)
    m_input.push_error(std::move(started.error()));
  // Stand in for the piece of setup() that teardown() undoes, so teardown()
  // has real work to do and a test can see it happen.
  //
  // Deliberately NOT m_in_screen: teardown() would answer that by calling
  // leave_screen(), which writes to whatever fd the Terminal found. Under
  // ctest that fd is -1 and nothing is emitted, but a developer running this
  // binary straight from a shell would get the alt-screen leave sequence
  // spat into a terminal that was never in the alt-screen — a test hook with
  // the same failure mode as the bug it is here to pin. The SIGWINCH hook
  // costs one process-wide disposition and writes nothing anywhere.
  m_winch_hooked = true;
  m_running = true;
  return run_loop();
}

auto App::stop_app() noexcept -> void {
  // One on_stop() per completed on_start(), BEFORE teardown() takes the
  // terminal down, on the normal and the exception path alike (#97). The
  // flag clears first so nothing can re-enter, and the noexcept call sites
  // make a throwing on_stop() override terminate right here, where the
  // contract says it must.
  if (m_app_started) {
    m_app_started = false;
    on_stop();
  }
}

auto App::pump_input() -> void {
  if (!m_playback && m_event_source &&
      m_event_source_mode == EventSourceMode::ReplaceTerminal)
    (void)discard_terminal_input();
  else
    (void)drain_input();
  (void)poll_event_source();

  // Only flush at a true input boundary, and never while an escape sequence
  // may still be in flight — flushing a lone ESC commits it as an Escape
  // keypress, which would turn every arrow key into a quit in the default
  // on_event. wait_frame() gives that ESC one grace window; if it's still
  // alone after that, m_esc_waited says the wait already happened and this
  // frame commits it.
  const bool hold_for_esc = m_input.esc_pending() && !m_esc_waited;
  if (m_got_bytes && !hold_for_esc) {
    m_input.flush();
    m_got_bytes = false;
  }
  if (!m_input.esc_pending()) m_esc_waited = false;
  for (auto& ev : m_input.poll()) dispatch_event(ev);
  dispatch_source_events();
}

auto App::on_event(const Event& ev) -> void {
  // Default behavior: ESC or Ctrl+C quits. Subclasses override for real input.
  // A release does not re-fire it (#60): under KeyboardMode::Enhanced every
  // Escape press is followed by an Escape release, and quitting twice on one
  // keystroke is at best confusing and at worst a double teardown.
  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    if (k->action != KeyAction::Press) return;
    if (k->key == Key::Escape || (k->ctrl && (k->ch == 'c' || k->ch == 'C'))) quit();
  }
}

auto App::push_overlay(Widget& w, OverlayOptions opts) -> void {
  m_overlays.push_back(OverlayEntry{&w, opts});
  request_render();
}

auto App::pop_overlay() -> void {
  if (m_overlays.empty()) return;
  m_overlays.pop_back();
  request_render();
}

auto App::clear_overlays() -> void {
  if (m_overlays.empty()) return;
  m_overlays.clear();
  request_render();
}

auto App::dispatch_event(const Event& ev) -> void {
  // Even an event the application declines is observable input and may have
  // changed state in a routing callback. Demand mode coalesces this with any
  // more specific request made below into the current frame's one render.
  request_render();
  if (const auto* invalidated = std::get_if<ImageInvalidatedEvent>(&ev)) {
    if (!stage_image_invalidation(invalidated->reason)) {
      on_event(Event{ErrorEvent{Severity::Warning, "app",
                               "ImageInvalidatedEvent reason is invalid"}});
    }
    return;
  }
  // Resize, image lifecycle, and error never get captured — the app underneath
  // still owns its layout/resources, and a degradation notice must not be
  // swallowed by a dialog. Image invalidation returned above after staging.
  if (std::holds_alternative<ResizeEvent>(ev) ||
      std::holds_alternative<ErrorEvent>(ev)) {
    on_event(ev);
    return;
  }
  // Nor does a key *release* (#60). An overlay that ate one would leave the
  // app beneath holding a key forever — press captured before the dialog
  // opened, release captured by the dialog — which is the stuck-key bug every
  // game with a pause menu hits. Repeat is deliberately NOT in this class: the
  // protocol sends it *instead of* a second press, so an overlay that never
  // saw one would lose hold-to-scroll and hold-to-type.
  if (const auto* rel = std::get_if<KeyEvent>(&ev)) {
    if (rel->action == KeyAction::Release) {
      on_event(ev);
      return;
    }
  }
  // Ctrl+C is the break-glass. Raw mode turned it from a signal into an
  // ordinary key, so if an overlay could swallow it, an app whose dialog has
  // no wired close path would be unkillable from its own terminal. No dialog
  // wants Ctrl+C, and the alternative is telling users to find another shell.
  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    if (k->ctrl && (k->ch == U'c' || k->ch == U'C')) {
      on_event(ev);
      return;
    }
  }
  if (m_overlays.empty()) {
    on_event(ev);
    return;
  }

  // Copy out of the vector before dispatching: the handler may push or pop
  // (a dialog button that closes itself), which reallocates m_overlays. A
  // reference or back() re-read after the call would dangle.
  Widget* top = m_overlays.back().widget;
  const OverlayOptions opts = m_overlays.back().opts;

  if (const auto* m = std::get_if<MouseEvent>(&ev)) {
    // Hit-test the overlay TREE, not the top widget's rect: an overlay (e.g.
    // a Dialog) may host a child whose interactive area extends past the
    // overlay's own rect — Select paints its open dropdown below the dialog
    // and overrides hit_test() to match (#37). Routing by the base rect made
    // those rendered rows dead, or dialog-dismissing under
    // dismiss_on_click_outside. Widgets without children report their own
    // hit_test, so this reduces to the old behavior for them.
    if (top->hit_test_tree(m->x, m->y)) {
      top->on_event(ev);
      return;
    }
    // Outside the overlay: swallowed either way. Only a press dismisses —
    // drag motion and wheel scroll must not close a dialog under the cursor.
    // An overlay with no geometry yet has not been drawn (a dialog sizes
    // itself from the Screen in draw()), and every point is "outside" it, so
    // dismissing now would pop it before it was ever visible.
    const Rect r = top->rect();
    const bool laid_out = r.w > 0 && r.h > 0;
    if (m->pressed && opts.dismiss_on_click_outside && laid_out) pop_overlay();
    return;
  }

  top->on_event(ev);  // key / paste — result ignored, capture is total
}

auto App::render_overlays(Screen& screen) -> void {
  m_backdrop_backup.clear();
  if (m_overlays.empty()) return;

  // Walk a snapshot: an overlay's draw() may legally push or pop (a toast
  // that expires as it renders), and mutating the vector mid-walk would
  // otherwise skip whichever entry shifted into the current index. A push
  // during draw simply lands on the next frame.
  const std::vector<OverlayEntry> stack = m_overlays;
  for (const OverlayEntry& entry : stack) {
    if (entry.widget == nullptr) continue;

    switch (entry.opts.backdrop) {
      case Backdrop::Fill:
        save_backdrop(screen);
        screen.fill_rect(0, 0, screen.cols(), screen.rows(), Cell{}.fg,
                         Cell{}.bg);
        break;
      case Backdrop::Dim:
        save_backdrop(screen);
        dim_screen(screen);
        break;
      case Backdrop::None:
        break;
    }
    entry.widget->draw(screen);
  }

  // Only the topmost overlay may put pixels on screen: its images flush last
  // and so land above everything. Anything below it is cells-only.
  if (!m_overlays.empty() && m_overlays.back().widget != nullptr)
    collect_pixel_regions(*m_overlays.back().widget);
}

auto App::save_backdrop(const Screen& screen) -> void {
  // Snapshot once per frame, before the first backdrop touches anything.
  // A backdrop is destructive — Dim halves every channel, Fill blanks every
  // cell — and the Screen persists across frames, so without this the damage
  // compounds: a cell the app does not repaint every frame gets halved again
  // and again until it is black, and stays that way after the dialog closes.
  if (!m_backdrop_backup.empty()) return;
  m_backdrop_backup.reserve(
      static_cast<std::size_t>(screen.cols()) *
      static_cast<std::size_t>(screen.rows() > 0 ? screen.rows() : 0));
  for (int y = 0; y < screen.rows(); ++y)
    for (int x = 0; x < screen.cols(); ++x)
      m_backdrop_backup.push_back(screen.at(x, y));
}

auto App::restore_backdrop(Screen& screen) -> void {
  // Put the frame back the way the app left it, now that the dimmed/filled
  // version is on the wire. The overlay pass is then non-destructive: what
  // on_render sees next frame is exactly what it drew last frame.
  if (m_backdrop_backup.empty()) return;
  std::size_t i = 0;
  for (int y = 0; y < screen.rows(); ++y) {
    for (int x = 0; x < screen.cols(); ++x) {
      if (i >= m_backdrop_backup.size()) break;  // resized mid-frame
      screen.at(x, y) = m_backdrop_backup[i++];
    }
  }
  m_backdrop_backup.clear();
}

auto App::dim_screen(Screen& screen) -> void {
  // Halve each channel. Cheap, exact, and diff-friendly: the Renderer still
  // emits only the cells that actually changed.
  for (int y = 0; y < screen.rows(); ++y) {
    for (int x = 0; x < screen.cols(); ++x) {
      Cell& c = screen.at(x, y);
      c.fg = Rgb{static_cast<std::uint8_t>(c.fg.r / 2),
                 static_cast<std::uint8_t>(c.fg.g / 2),
                 static_cast<std::uint8_t>(c.fg.b / 2)};
      c.bg = Rgb{static_cast<std::uint8_t>(c.bg.r / 2),
                 static_cast<std::uint8_t>(c.bg.g / 2),
                 static_cast<std::uint8_t>(c.bg.b / 2)};
    }
  }
}

// These two hold the loops; both public spellings forward here. A braced list
// cannot bind to a span until P2447 (C++26), so both spellings have to exist --
// but only one of them gets to define the contract (#123).
auto App::route_mouse_span(const MouseEvent& ev,
                           std::span<Widget* const> widgets) -> bool {
  // Check in reverse order (last registered = topmost). A null entry is
  // ABSENT, not opaque: skip it and keep descending, so a sometimes-populated
  // pointer behaves here the way it already does in tick_widgets (#123).
  for (auto it = widgets.end(); it != widgets.begin();) {
    --it;
    if (*it == nullptr) continue;
    if ((*it)->hit_test(ev.x, ev.y)) {
      return (*it)->on_event(ev);
    }
  }
  return false;
}

auto App::tick_widgets_span(std::chrono::duration<double> dt,
                            std::span<Widget* const> widgets) -> void {
  for (Widget* w : widgets)
    if (w != nullptr) w->on_tick(dt);
}

// The braced spellings. span's range constructor, not (begin(), size()): an
// empty braced list has a NULL begin(), and the two-argument form would rest
// its "[first, first + count) is a valid range" precondition on nullptr + 0.
// The range form asks no such question, and route_mouse(ev, {}) is legal.
auto App::route_mouse(const MouseEvent& ev,
                      std::initializer_list<Widget*> widgets) -> bool {
  return route_mouse_span(ev, std::span<Widget* const>{widgets});
}

auto App::tick_widgets(std::chrono::duration<double> dt,
                       std::initializer_list<Widget*> widgets) -> void {
  tick_widgets_span(dt, std::span<Widget* const>{widgets});
}

auto App::render_pixel_regions(Widget& widget) -> void {
  // Modal: skip the app's images entirely. They would be emitted after the
  // cell diff and paint over the dialog, and collecting them also blanks the
  // cells they cover — punching a hole in the backdrop.
  if (!m_overlays.empty()) return;
  collect_pixel_regions(widget);
}

auto App::collect_pixel_regions(Widget& widget) -> void {
  // Below the declared floor, keep the authored cell Baseline and do not ask
  // widgets for enhanced frames (#91). The app observes requirements_met() /
  // the transition ErrorEvent and decides whether to show its own UI.
  if (!m_driver || !enhanced_image_path(*m_driver) || !m_requirements_met)
    return;

  const auto regions = widget.pixel_regions();
  for (std::size_t ordinal = 0; ordinal < regions.size(); ++ordinal) {
    const Rect region = regions[ordinal];
    const PixelRegionState state = widget.pixel_region_state(region);
    const PlacementFit fit = widget.pixel_fit(region);
    PersistentPixelRegion* retained = nullptr;
    if (state.mode == PixelRegionMode::Persistent) {
      const auto it = std::find_if(
          m_persistent_pixels.begin(), m_persistent_pixels.end(),
          [&](const PersistentPixelRegion& candidate) {
            return candidate.owner == &widget &&
                   candidate.ordinal == ordinal;
          });
      if (it == m_persistent_pixels.end()) {
        m_persistent_pixels.push_back(
            PersistentPixelRegion{.owner = &widget, .ordinal = ordinal});
        retained = &m_persistent_pixels.back();
      } else {
        retained = &*it;
      }
      retained->seen = true;
    }

    // The widget cannot ask the driver itself, so hand it the answer.
    const Extent px = m_driver->preferred_pixel_extent(region);
    bool needs_image = state.mode == PixelRegionMode::Immediate;
    if (retained != nullptr) {
      // A generated raster may depend on the destination's cell extent and on
      // the driver's current pixels-per-cell answer. Ask again at those two
      // boundaries; fixed-grid producers simply return the same-sized image,
      // which leaves their content resident and changes placement only.
      const bool destination_extent_changed =
          retained->content_ready &&
          (retained->rect.w != region.w || retained->rect.h != region.h);
      needs_image = !retained->content_ready || state.content_dirty ||
                    retained->recreate || destination_extent_changed ||
                    m_pixel_force_repaint;
      // A non-resident enhanced tier (currently ANSI) needs the source again
      // when placement must be repainted; App never borrows it across frames.
      if (m_driver->max_pinned_images() == 0 &&
          (!retained->visible || retained->rect != region ||
           retained->fit != fit || m_pixel_force_repaint)) {
        needs_image = true;
      }
    }

    const Image* image = needs_image ? widget.draw_pixels(region, px) : nullptr;
    // Non-empty as well as non-null: an engaged optional holding an Image{}
    // used to blank the covered cells here and then be rejected by the driver
    // as "draw_image: empty image", leaving a hole in the UI.
    const bool cached = retained != nullptr && retained->content_ready;
    if ((image != nullptr && !image->empty()) || (!needs_image && cached)) {
      m_pixel_regions.push_back({.owner = &widget,
                                 .ordinal = ordinal,
                                 .rect = region,
                                 .image = image,
                                 .fit = fit,
                                 .mode = state.mode,
                                 .content_dirty = state.content_dirty});

      // Clear the Screen cells in this region so the cell diff does not emit
      // the fallback underneath the image. This is load-bearing on ANSI too:
      // when the region disappears, the next authored cell path then differs
      // from blank and is repainted instead of leaving the old half-blocks.
      for (int y = region.y; y < region.y + region.h; ++y)
        for (int x = region.x; x < region.x + region.w; ++x)
          m_screen->at(x, y) = Cell{};
    } else if (retained != nullptr) {
      // No enhanced frame exists for this visible region. Its placement will
      // be collected at this frame boundary; remember that so a later retry
      // uses draw_pinned rather than a no-wire retain of a vanished placement.
      retained->visible = false;
    }
  }
}

auto App::flush_pixel_regions() -> void {
  // The frame's image window, and the only correct place in the frame for an
  // image draw. Despite the name it does NOT flush -- since #148 the frame's
  // single flush is Renderer::flush() after this returns, and this window's
  // draws queue AFTER the cell diff in the driver's buffer. (The name is now
  // historical: it used to perform the frame's second flush.) The after-diff
  // order is the compositing one: collect_pixel_regions blanked the region's
  // cells, present() emitted the diff and blanks, and this placeholder/id
  // grid lands last so that diff cannot overwrite it -- one write, images on
  // top, no torn pair.
  //
  // Keep the application hook on the same capability gate as the region path:
  // Kitty gets native placements, ANSI truecolour gets half-block raster, and
  // Baseline keeps its authored cells (#108).
  const bool enhanced =
      m_driver && enhanced_image_path(*m_driver) && m_requirements_met;

  // Ungated: m_pixel_regions can only be non-empty if collect_pixel_regions
  // already passed the same test.
  for (const auto& pr : m_pixel_regions) {
    if (pr.mode == PixelRegionMode::Immediate) {
      if (auto drawn = m_driver->draw_image(pr.rect, *pr.image, pr.fit);
          !drawn) {
        m_input.push_error(std::move(drawn.error()));
      }
      continue;
    }

    const auto state_it = std::find_if(
        m_persistent_pixels.begin(), m_persistent_pixels.end(),
        [&](const PersistentPixelRegion& candidate) {
          return candidate.owner == pr.owner &&
                 candidate.ordinal == pr.ordinal;
        });
    if (state_it == m_persistent_pixels.end()) continue;
    auto& state = *state_it;

    const Extent next_extent =
        pr.image != nullptr
            ? Extent{pr.image->width(), pr.image->height()}
            : state.extent;
    const bool extent_changed =
        state.content_ready && pr.image != nullptr &&
        state.extent != next_extent;

    if (m_driver->max_pinned_images() == 0) {
      const bool placement_changed = !state.visible || state.rect != pr.rect ||
                                     state.fit != pr.fit ||
                                     m_pixel_force_repaint;
      const bool submit_content = !state.content_ready || pr.content_dirty ||
                                  state.recreate || extent_changed;
      if (submit_content || placement_changed) {
        // collect_pixel_regions asks for the image when either predicate can
        // reach here; keep the guard defensive because a null borrowed view is
        // a fallback request, never permission to dereference it.
        if (pr.image == nullptr) continue;
        if (auto drawn = m_driver->draw_image(pr.rect, *pr.image, pr.fit);
            !drawn) {
          m_input.push_error(std::move(drawn.error()));
          continue;
        }
        state.pending_visible = true;
        state.pending_rect = pr.rect;
        state.pending_fit = pr.fit;
        state.touched_wire = true;
        if (submit_content) {
          state.pending_content = true;
          state.pending_extent =
              Extent{pr.image->width(), pr.image->height()};
        }
      }
      continue;
    }

    const bool submit_content = !state.content_ready || pr.content_dirty ||
                                state.recreate || extent_changed;
    if (!m_driver->supports_placement_fit(pr.fit)) {
      m_input.push_error(ErrorEvent{
          Severity::Warning, "app",
          "persistent pixel region: requested placement fit is unsupported "
          "by this driver"});
      continue;
    }
    if (pr.fit == PlacementFit::Exact) {
      const Extent needed = m_driver->image_cell_extent(next_extent);
      if (needed.w > pr.rect.w || needed.h > pr.rect.h) {
        m_input.push_error(ErrorEvent{
            Severity::Warning, "app",
            std::format("persistent pixel region: Exact placement needs "
                        "{}x{} cells for {}x{} pixels, but destination is "
                        "{}x{}",
                        needed.w, needed.h, next_extent.w, next_extent.h,
                        pr.rect.w, pr.rect.h)});
        continue;
      }
    }
    if ((state.recreate || extent_changed) && state.pin) {
      if (auto released = m_driver->unpin_image(state.pin); !released) {
        m_input.push_error(std::move(released.error()));
        continue;
      }
      state.pin = {};
      state.visible = false;
      state.touched_wire = true;
    }

    bool content_ok = true;
    if (!state.pin) {
      if (pr.image == nullptr) continue;
      auto pinned = m_driver->pin_image(*pr.image);
      if (!pinned) {
        m_input.push_error(std::move(pinned.error()));
        continue;
      }
      state.pin = *pinned;
      state.touched_wire = true;
    } else if (submit_content && pr.image != nullptr && !state.recreate &&
               !extent_changed) {
      if (auto replaced = m_driver->replace_pinned(state.pin, *pr.image);
          !replaced) {
        m_input.push_error(std::move(replaced.error()));
        content_ok = false;
      } else {
        state.touched_wire = true;
      }
    }
    if (!content_ok) {
      // Preserve the last accepted frame when replacement was refused. The
      // producer stays dirty, but the existing placement need not turn into a
      // hole while it waits for a retry.
      if (state.visible && state.rect == pr.rect && state.fit == pr.fit) {
        if (auto kept = m_driver->retain_pinned(pr.rect, state.pin, pr.fit);
            !kept) {
          m_input.push_error(std::move(kept.error()));
        }
      }
      continue;
    }

    const bool placement_changed = !state.visible || state.rect != pr.rect ||
                                   state.fit != pr.fit ||
                                   m_pixel_force_repaint || state.recreate ||
                                   extent_changed;
    // retain_pinned is a non-pure compatibility hook: Kitty's override is a
    // no-wire clock refresh, while an older driver inherits the honest
    // draw_pinned fallback and may append placement bytes. Treat either route
    // as touching driver state so a refused sink write retries conservatively.
    state.touched_wire = true;
    auto placed = placement_changed
                      ? m_driver->draw_pinned(pr.rect, state.pin, pr.fit)
                      : m_driver->retain_pinned(pr.rect, state.pin, pr.fit);
    if (!placed) {
      m_input.push_error(std::move(placed.error()));
      continue;
    }
    state.pending_visible = true;
    state.pending_rect = pr.rect;
    state.pending_fit = pr.fit;
    if (submit_content) {
      state.pending_content = true;
      state.pending_extent = next_extent;
    }
  }

  // An overlay suspends the underlying placement but deliberately keeps its
  // resident data. Outside that explicit suspension, omission ends the
  // persistent region's lifetime and returns its pin budget in this frame.
  for (auto it = m_persistent_pixels.begin();
       it != m_persistent_pixels.end();) {
    if (it->seen) {
      ++it;
      continue;
    }
    if (!m_overlays.empty()) {
      it->visible = false;
      ++it;
      continue;
    }
    if (it->pin) {
      if (auto released = m_driver->unpin_image(it->pin); !released) {
        m_input.push_error(std::move(released.error()));
        ++it;
        continue;
      }
    }
    it = m_persistent_pixels.erase(it);
  }

  // After the regions, so a same-rect collision resolves in the widget tree's
  // favour (see the hook's doc -- it is an emission order, not a claim about
  // what the terminal composites). Suppressed under an overlay for the FIRST of
  // the two reasons render_pixel_regions gives: images are emitted after the
  // cell diff and would paint through the dialog. Its second reason -- that
  // collecting also blanks the cells it covers -- has no analogue here, since a
  // direct driver draw touches no cell. The tie-breaker is that an app drawing
  // through both paths must not keep half its images and lose the other half.
  if (enhanced && m_overlays.empty()) on_pixels(*m_driver);

  // No flush here. On an enhanced tier this window runs on EVERY frame -- even
  // one with no regions and an empty on_pixels. On Kitty those draws also drive
  // per-frame collection cadence; on ANSI the empty call is simply free. The
  // write itself is frame_step's Renderer::flush(). Baseline has no image
  // window at all and is untouched at one write per frame.
}

auto App::finish_pixel_frame(bool output_accepted) -> void {
  const FrameBytes emitted = m_driver->last_frame_bytes();
  const bool image_wire =
      emitted.image_transmit != 0 || emitted.image_edit != 0;
  for (auto& state : m_persistent_pixels) {
    if (!output_accepted) {
      // Driver bookkeeping has already advanced past the refused sink write.
      // Recreate resident content on the next visible frame so its retry
      // cannot be suppressed by the driver's now-ahead content hash. A clean
      // Kitty retain advances only collection clocks and emits zero image
      // bytes; a refusal of an otherwise empty/cell-only frame must not turn
      // that accepted content dirty again. The per-frame meter is the exact
      // write-side answer, including for a legacy retain that delegated to a
      // placement draw.
      // On a resident tier, a clean retain can touch driver bookkeeping while
      // emitting no bytes, so the frame meter distinguishes it from a refused
      // image operation. A non-resident tier has no retain operation: when its
      // region says it touched wire, draw_image appended its in-band cell
      // raster and a refusal must retry it even though that traffic belongs to
      // the meter's cells bucket by construction.
      const bool refused_region_wire =
          state.touched_wire &&
          (m_driver->max_pinned_images() == 0 || image_wire);
      if (refused_region_wire) {
        state.visible = false;
        state.recreate = true;
      }
      state.pending_content = false;
      state.pending_visible = false;
      state.touched_wire = false;
      continue;
    }

    if (state.pending_visible) {
      state.visible = true;
      state.rect = state.pending_rect;
      state.fit = state.pending_fit;
    }
    if (state.pending_content) {
      state.content_ready = true;
      state.extent = state.pending_extent;
      state.recreate = false;
      // The key may be stale only after an unseen region was erased, and those
      // entries are removed before this walk. Seen owners remain alive for the
      // complete on_render -> flush -> acknowledgement window.
      state.owner->pixel_region_submitted(state.pending_rect);
    }
    state.pending_content = false;
    state.pending_visible = false;
    state.touched_wire = false;
  }
}

auto App::set_size(Size size) -> std::expected<void, ErrorEvent> {
  // Every guard runs before anything is stored, the way set_io's do (#179): a
  // caller forwarding a peer's window-change and dropping the result keeps the
  // size it had rather than half of the one it was sent.
  if (size.cols <= 0 || size.rows <= 0) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "app",
        std::format("set_size: cols and rows must be > 0, got {}x{}", size.cols,
                    size.rows)}};
  }
  // Zero is legal on either axis and is not a degradation: it is what tmux and
  // the Linux console report, and push_cell_pixel_size already reads it as
  // "unknown". Negative is not a measurement of anything.
  if (size.px_w < 0 || size.px_h < 0) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "app",
        std::format("set_size: pixel dimensions must be >= 0, got {}x{}",
                    size.px_w, size.px_h)}};
  }
  // A domain match, not an allocation guard -- see the header. Screen::resize
  // widens to size_t before multiplying, so there is nothing here to keep from
  // overflowing; what this refuses is a window no ioctl could have reported.
  //
  // All FOUR fields, not just the grid: ws_xpixel/ws_ypixel are unsigned shorts
  // too, and the pixel pair is the half with teeth. push_cell_pixel_size divides
  // it by the grid, so an unbounded pixel dimension over a 1x1 grid hands the
  // driver a cell of INT_MAX -- which makes preferred_pixel_extent's room
  // effectively infinite and stops PlacementFit::Exact refusing anything for the
  // rest of the session. Bounding cols/rows alone would leave the #173 lesson
  // half-applied on the very call that re-opened it.
  if (size.cols > kMaxPushedDim || size.rows > kMaxPushedDim ||
      size.px_w > kMaxPushedDim || size.px_h > kMaxPushedDim) {
    return std::unexpected{ErrorEvent{
        Severity::Warning, "app",
        std::format("set_size: no dimension may exceed {} ({}x{} cells, "
                    "{}x{} pixels)",
                    kMaxPushedDim, size.cols, size.rows, size.px_w,
                    size.px_h)}};
  }
  m_pushed_size = size;
  // A resize REQUEST, never a resize: the Screen, the renderer invalidation,
  // the cell geometry and the ResizeEvent are all frame_step()'s to produce,
  // off this one flag, in the order a SIGWINCH already produces them.
  request_resize();
  return {};
}

auto App::clear_size() noexcept -> void {
  m_pushed_size.reset();
  // Unconditional, and not merely for symmetry: the effective size may have
  // just changed by several tens of columns without anybody touching a window.
  request_resize();
}

auto App::has_pushed_size() const noexcept -> bool {
  return m_pushed_size.has_value();
}

auto App::current_size() const -> Size {
  // The push wins (#180). The peer is the only party that knows: a socket has
  // no window to interrogate at all, and a pty that does answer answers with
  // whatever was last written into its winsize -- a copy of the peer's number
  // at best, and stale the moment the peer drags a corner. Ordered FIRST rather
  // than used as a fallback, so no single measurement can consult both sources.
  if (m_pushed_size) return *m_pushed_size;
  winsize ws{};
  // Ask the stream this Terminal actually writes to, not STDOUT_FILENO (#179).
  // The two are the same thing for a program that owns its terminal, and are
  // not for a session whose fds were injected — where the old spelling reported
  // the *daemon's* window, silently and plausibly.
  //
  // A stream with no window (a socket, a pipe) answers ENOTTY and falls through
  // to the default below. That is correct-by-default rather than correct: the
  // real answer for a remote session arrives as a protocol message and has to
  // be pushed in, which is what the branch above is (#180). The guard is for the
  // -1 "no output stream" sentinel, and it buys a syscall rather than a
  // behaviour — ioctl(-1) fails into the same default.
  const int fd = m_term.io().out;
  if (fd >= 0 && ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0)
    return {ws.ws_col, ws.ws_row, ws.ws_xpixel, ws.ws_ypixel};
  return {80, 24};  // sane default if ioctl fails
}

auto App::push_cell_pixel_size(Size size) -> void {
  if (!m_driver) return;
  // Divide the text area by the cell grid. A terminal that reports no pixel
  // geometry (0 is common: tmux, the Linux console, several emulators) leaves
  // this at Extent{}, and the driver keeps its own nominal cell size — the
  // division is never attempted with a zero denominator, and "unknown" is
  // deliberately not an ErrorEvent: a nominal cell is a correctly-shaped
  // guess, not a degraded capability.
  Extent cell{};
  if (size.px_w > 0 && size.px_h > 0 && size.cols > 0 && size.rows > 0) {
    cell = Extent{size.px_w / size.cols, size.px_h / size.rows};
  }
  m_driver->set_cell_pixel_size(cell);
}

auto App::check_requirements_startup(Size size)
    -> std::expected<void, ErrorEvent> {
  const auto driver_caps = m_driver ? m_driver->capabilities() : Capabilities{};
  const auto facts = detail::make_requirement_facts(
      m_caps, driver_caps, input_capabilities(), size.cols, size.rows, size.px_w,
      size.px_h);
  auto result = detail::evaluate_requirements(m_requirements, facts,
                                               Severity::Error);
  m_requirements_met = result.has_value();
  return result;
}

auto App::update_requirements(Size size) -> void {
  if (detail::requirements_empty(m_requirements)) {
    m_requirements_met = true;
    return;
  }
  const auto driver_caps = m_driver ? m_driver->capabilities() : Capabilities{};
  const auto facts = detail::make_requirement_facts(
      m_caps, driver_caps, input_capabilities(), size.cols, size.rows, size.px_w,
      size.px_h);
  auto result = detail::evaluate_requirements(m_requirements, facts,
                                               Severity::Warning);
  const bool met = result.has_value();
  if (met == m_requirements_met) return;
  m_requirements_met = met;
  if (met) {
    m_input.push_error(ErrorEvent{
        Severity::Info, "requirements",
        "the declared AppRequirements floor is met again; enhanced submission "
        "resumed"});
  } else {
    m_input.push_error(std::move(result.error()));
  }
}

}  // namespace termforge
