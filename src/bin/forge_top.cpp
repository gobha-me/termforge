#include "forge_top.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <utility>
#include <variant>

#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"
#include "termforge/widgets/detail/width.hpp"
#include "termforge/widgets/theme.hpp"

namespace termforge::forge_top {
namespace {
constexpr Rgb kChrome{0x20, 0x20, 0x40};
constexpr Rgb kAccent{0x00, 0xD4, 0xFF};

auto ui_line(Screen& screen, Rect area, std::string_view text,
             Rgb color = theme::kFg) -> void {
  if (area.empty()) return;
  screen.fill_rect(area.x, area.y, area.w, area.h, color, kChrome);
  screen.write_text(area.x, area.y, detail::truncate_to_width(text, area.w),
                    color, kChrome);
}

auto sort_label(ProcessSort sort) -> std::string_view {
  switch (sort) {
    case ProcessSort::Cpu: return "CPU";
    case ProcessSort::Memory: return "MEM";
    case ProcessSort::Pid: return "PID";
    case ProcessSort::Time: return "TIME+";
    case ProcessSort::User: return "USER";
    case ProcessSort::State: return "S";
    case ProcessSort::Command: return "CMD";
  }
  return "CPU";
}

auto make_driver(DriverChoice choice) -> std::unique_ptr<TerminalDriver> {
  switch (choice) {
    case DriverChoice::Kitty: return std::make_unique<KittyDriver>();
    case DriverChoice::AnsiRgb: return std::make_unique<AnsiRgbDriver>();
    case DriverChoice::Fallback: return std::make_unique<FallbackDriver>();
    case DriverChoice::Automatic: return std::make_unique<FallbackDriver>();
  }
  return std::make_unique<FallbackDriver>();
}

} // namespace

HelpPopup::HelpPopup() : Dialog{"forge-top help"} {
  add_child(&m_document);
  set_context("Ready.");
  set_max_width(72);
}

auto HelpPopup::set_context(std::string context) -> void {
  m_document.clear();
  for (const auto& line : std::vector<std::string>{
           "Esc/q/h/?/F1: back",
           "PgUp/PgDn, arrows, wheel: scroll",
           "Home/End: top/bottom",
           "",
           "CURRENT CONTEXT",
           std::move(context),
           "",
           "TOP-COMPATIBLE KEYS",
           "q quit; h/?/F1 help; Space refresh",
           "P CPU; M memory; N PID; T TIME+; R reverse",
           "d/s: finite seconds >=0; 0 samples every frame",
           "1: aggregate/per-CPU; l/t/m: overview/CPU/memory",
           "c: command name/full command line",
           "",
           "NAVIGATION",
           "F2: All / Processes / Summary views",
           "F3: cycle summary/process balance on larger grids",
           "/: focus filter; Tab/Shift+Tab: focus",
           "Filter Enter/Esc: return to table; paste is unsupported",
           "Arrows, PgUp/PgDn, Home/End: table and menus",
           "Enter: process detail (unlike top); Esc/q: close detail",
           "Menus and table headers mirror sort/view actions",
           "",
           "Compact layouts prioritize processes; Summary shows the facts.",
           "ASCII graphs: # full bar, : half bar. Narrow core labels are IDs.",
           "DEMO means simulated data. A stale sample keeps last good rows.",
           "No kill/renice (k/r), filesystem writes or process-tree claims.",
           "End of help. Esc returns."})
    m_document.append(line);
}

auto HelpPopup::layout_content(Rect area) -> void {
  m_document.set_style(border_style());
  m_document.set_geometry(area);
}

auto HelpPopup::on_show() -> void {
  m_document.scroll(-std::numeric_limits<int>::max());
}

auto HelpPopup::on_escape() -> void {
  if (begin_result()) close();
}

auto HelpPopup::on_event(const Event& event) -> bool {
  if (const auto* key = std::get_if<KeyEvent>(&event);
      key && key->action == KeyAction::Press && key->key == Key::Char &&
      !key->ctrl && !key->alt &&
      (key->ch == U'q' || key->ch == U'h' || key->ch == U'?')) {
    on_escape();
    return true;
  }
  if (const auto* key = std::get_if<KeyEvent>(&event);
      key && key->action == KeyAction::Press && key->key == Key::F1) {
    on_escape();
    return true;
  }
  if (const auto* key = std::get_if<KeyEvent>(&event);
      key && key->action != KeyAction::Release) {
    switch (key->key) {
      case Key::Home: on_show(); return true;
      case Key::End: m_document.scroll_to_bottom(); return true;
      case Key::Up: m_document.scroll(-1); return true;
      case Key::Down: m_document.scroll(1); return true;
      default: break;
    }
  }
  return Dialog::on_event(event);
}

ForgeTopApp::ForgeTopApp(std::unique_ptr<SystemReader> reader, bool simulated)
    : m_reader(std::move(reader)), m_simulated(simulated) {
  if (!m_reader) {
    m_reader = make_fake_reader();
    m_simulated = true;
  }
  set_frame_ms(33);

  m_processes.on_activate(
      [this](const ProcessRow& process) { open_detail(process); });
  m_detail.on_close([this] { pop_overlay(); });
  m_help.on_close([this] { pop_overlay(); });
  m_delay_prompt.on_close([this] { pop_overlay(); });
  m_delay_prompt.on_submit(
      [this](std::string value) { apply_delay(std::move(value)); });
  m_delay_prompt.on_cancel([this] { set_status("Sampling delay unchanged."); });

  m_menu.set_menus({
      {"Sort",
       {{"%CPU (P)", [this] { sort_by(ProcessSort::Cpu); }},
        {"%MEM (M)", [this] { sort_by(ProcessSort::Memory); }},
        {"PID (N)", [this] { sort_by(ProcessSort::Pid); }},
        {"TIME+ (T)", [this] { sort_by(ProcessSort::Time); }},
        {"Command", [this] { sort_by(ProcessSort::Command); }},
        {"Reverse (R)", [this] { reverse_sort(); }}}},
      {"View",
       {{"All panels (F2)", [this] { set_preset(Preset::All); }},
        {"Processes (F2)", [this] { set_preset(Preset::Processes); }},
        {"Summary (F2)", [this] { set_preset(Preset::Summary); }},
        {"Summary balance (F3)", [this] { cycle_balance(); }},
        {"Aggregate/per-CPU (1)",
         [this] { (void)handle_global_key(KeyEvent{Key::Char, U'1'}); }},
        {"Command name/line (c)",
         [this] { (void)handle_global_key(KeyEvent{Key::Char, U'c'}); }}}},
      {"Help", {{"Keys", [this] { show_help(); }}}},
  });
  rebuild_focus();
  refresh();
}

auto ForgeTopApp::force_driver(DriverChoice choice)
    -> std::expected<void, ErrorEvent> {
  return set_builtin_driver(choice);
}

auto ForgeTopApp::run_headless(int frames, int cols, int rows,
                               std::string* sink, DriverChoice choice) -> void {
  if (choice == DriverChoice::Automatic) choice = DriverChoice::Fallback;
  apply_style(choice == DriverChoice::Fallback);
  test_run_frames(frames, cols, rows, sink, make_driver(choice));
}

auto ForgeTopApp::show_first_process_for_test() -> bool {
  if (m_snapshot.processes.empty()) return false;
  open_detail(m_snapshot.processes.front());
  return true;
}

auto ForgeTopApp::set_status(std::string status) -> void {
  m_status = Screen::sanitize(status);
}

auto ForgeTopApp::apply_style(bool ascii) -> void {
  const BorderStyle style = ascii ? BorderStyle::Ascii : BorderStyle::Rounded;
  m_overview.set_style(style);
  m_cpu.set_style(style);
  m_memory.set_style(style);
  m_processes.set_style(style);
  m_menu.set_style(style);
  m_detail.set_style(style);
  m_help.set_border_style(style);
  m_delay_prompt.set_border_style(style);
}

auto ForgeTopApp::on_start() -> void {
  const auto caps = driver().capabilities();
  apply_style(!caps.kitty_graphics && !caps.truecolor);
}

auto ForgeTopApp::rebuild_focus() -> void {
  auto* keep = m_focus.current();
  m_processes.filter().set_focused(false);
  m_processes.table().set_focused(false);
  m_menu.set_focused(false);
  m_focus.clear();
  if (m_show_processes && m_usable) {
    m_focus.add(&m_processes.filter());
    m_focus.add(&m_processes.table());
    m_focus.focus(&m_processes.table());
  }
  m_focus.add(&m_menu);
  if (keep) (void)m_focus.focus(keep);
}

auto ForgeTopApp::set_preset(Preset preset) -> void {
  m_show_overview = preset != Preset::Processes;
  m_show_cpu = preset != Preset::Processes;
  m_show_memory = preset != Preset::Processes;
  m_show_processes = preset != Preset::Summary;
  rebuild_focus();
  set_status(preset == Preset::All       ? "View: all panels"
             : preset == Preset::Summary ? "View: summary"
                                         : "View: processes");
  if (m_geometry_initialized) layout(m_grid.w, m_grid.h);
}

auto ForgeTopApp::preset_name() const -> std::string_view {
  if (m_show_processes && m_show_overview && m_show_cpu && m_show_memory)
    return "ALL";
  if (!m_show_processes && m_show_overview && m_show_cpu && m_show_memory)
    return "SUMMARY";
  if (m_show_processes && !m_show_overview && !m_show_cpu && !m_show_memory)
    return "PROCESSES";
  return "CUSTOM";
}

auto ForgeTopApp::cycle_preset() -> void {
  set_preset(preset_name() == "ALL"         ? Preset::Processes
             : preset_name() == "PROCESSES" ? Preset::Summary
                                            : Preset::All);
}

auto ForgeTopApp::sort_by(ProcessSort sort) -> void {
  m_processes.set_sort(sort);
  set_status(std::format("Sort: {}{}", sort_label(sort),
                         m_processes.descending() ? "v" : "^"));
}

auto ForgeTopApp::cycle_balance() -> void {
  m_summary_balance = (m_summary_balance + 1) % 3;
  set_status(std::format("Summary balance {} / 3{}", m_summary_balance + 1,
                         m_compact ? " (on larger grids)" : ""));
  if (m_geometry_initialized) layout(m_grid.w, m_grid.h);
}

auto ForgeTopApp::reverse_sort() -> void {
  m_processes.reverse_sort();
  set_status(std::format("Sort: {}{}", sort_label(m_processes.sort_key()),
                         m_processes.descending() ? "v" : "^"));
}

auto ForgeTopApp::focus_name() const -> std::string_view {
  if (m_focus.current() == &m_processes.filter()) return "FILTER";
  if (m_focus.current() == &m_processes.table()) return "TABLE";
  return "MENU";
}

auto ForgeTopApp::sample_status() const -> std::string {
  if (!m_have_sample) return "No successful sample";
  return std::format("{}sample #{} age {:.1f}s d {:.3g}s",
                     m_recovered ? "Recovered " : "", m_sample_number,
                     m_sample_age.count(), m_sample_delay.count());
}

auto ForgeTopApp::refresh() -> void {
  auto snapshot = m_reader->sample();
  if (!snapshot) {
    m_sample_error = Screen::sanitize(std::format(
        "{}: {}", snapshot.error().source, snapshot.error().message));
    m_recovered = false;
    return;
  }
  if (!m_sample_error.empty()) m_recovered = true;
  m_sample_error.clear();
  m_have_sample = true;
  m_sample_age = {};
  if (m_sample_number != std::numeric_limits<std::uint64_t>::max())
    ++m_sample_number;
  m_snapshot = std::move(*snapshot);
  m_overview.set_snapshot(m_snapshot.uptime_seconds, m_snapshot.load_average,
                          m_snapshot.tasks);
  m_cpu.set_samples(m_snapshot.cpus);
  m_cpu.set_aggregate_sample(m_snapshot.aggregate_cpu);
  m_memory.set_memory(m_snapshot.memory);

  std::unordered_map<ProcessIdentity, std::vector<float>, ProcessIdentityHash>
      next_history;
  next_history.reserve(m_snapshot.processes.size());
  for (const auto& process : m_snapshot.processes) {
    const ProcessIdentity identity = process.identity();
    auto history = std::move(m_history[identity]);
    history.push_back(process.cpu_percent);
    if (history.size() > 160) history.erase(history.begin());
    next_history.emplace(identity, std::move(history));
  }
  m_history = std::move(next_history);
  m_processes.set_processes(m_snapshot.processes);
  update_detail();
}

auto ForgeTopApp::open_detail(const ProcessRow& process) -> void {
  if (m_geometry_initialized && !m_usable) {
    set_status("Resize >=24x8 for detail; F1 help.");
    return;
  }
  const auto it = m_history.find(process.identity());
  const std::span<const float> history =
      it == m_history.end()
          ? std::span<const float>{}
          : std::span<const float>{it->second.data(), it->second.size()};
  m_detail.set_process(process, history);
  if (top_overlay() != &m_detail) push_overlay(m_detail);
}

auto ForgeTopApp::update_detail() -> void {
  const auto identity = m_detail.identity();
  if (top_overlay() != &m_detail || !identity) return;
  const auto process = std::ranges::find_if(
      m_snapshot.processes, [&](const ProcessRow& candidate) {
        return candidate.identity() == *identity;
      });
  if (process == m_snapshot.processes.end()) {
    const bool replaced = std::ranges::any_of(
        m_snapshot.processes, [&](const ProcessRow& candidate) {
          return candidate.pid == identity->pid;
        });
    pop_overlay();
    set_status(std::format("PID {} {}", identity->pid,
                           replaced ? "was replaced" : "exited"));
    return;
  }
  open_detail(*process);
}

auto ForgeTopApp::show_help() -> void {
  m_help.set_context(std::format(
      "Snapshot at open; sampling continues.\n{} / {} / focus {} / sort {}{} / "
      "{}\nResult: {}\n{}\nFilter: {}\n{}",
      preset_name(), layout_name(), focus_name(),
      sort_label(m_processes.sort_key()), m_processes.descending() ? "v" : "^",
      sample_status(), m_status,
      m_sample_error.empty() ? "Sample healthy" : "STALE: " + m_sample_error,
      m_processes.filter().text(),
      m_have_sample
          ? std::format(
                "CPU {:.0f}%; {} tasks; RAM {} bytes available / {} total",
                m_snapshot.aggregate_cpu.usage * 100.0F, m_snapshot.tasks.total,
                m_snapshot.memory.available_bytes,
                m_snapshot.memory.total_bytes)
          : "No successful sample; facts unavailable."));
  if (top_overlay() != &m_help) push_overlay(m_help);
}

auto ForgeTopApp::show_delay_prompt() -> void {
  if (m_geometry_initialized && !m_usable) {
    set_status("Resize >=24x8 for delay; F1 help.");
    return;
  }
  m_delay_prompt.set_text("Seconds >=0 (0 = every frame):");
  m_delay_prompt.set_value(std::format("{:.3g}", m_sample_delay.count()));
  if (top_overlay() != &m_delay_prompt) push_overlay(m_delay_prompt);
}

auto ForgeTopApp::apply_delay(std::string value) -> void {
  double seconds = 0.0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), seconds);
  if (error != std::errc{} || end != value.data() + value.size() ||
      !std::isfinite(seconds) || seconds < 0.0) {
    set_status("Delay must be a finite, non-negative number");
    m_delay_prompt.set_text("Finite seconds >=0:");
    m_delay_prompt.set_value(std::move(value));
    push_overlay(m_delay_prompt);
    return;
  }
  m_sample_delay = std::chrono::duration<double>{seconds};
  m_sample_elapsed = {};
  set_status(std::format("Sampling delay set to {:.3g}s", seconds));
}

auto ForgeTopApp::handle_global_key(const KeyEvent& key) -> bool {
  if (key.action != KeyAction::Press) return false;
  if (key.key == Key::F1) {
    show_help();
    return true;
  }
  if (key.key == Key::F2) {
    cycle_preset();
    return true;
  }
  if (key.key == Key::F3) {
    cycle_balance();
    return true;
  }
  if (key.key != Key::Char || key.ctrl || key.alt) return false;
  switch (key.ch) {
    case U'/':
      if (m_show_processes && m_usable) {
        (void)m_focus.focus(&m_processes.filter());
        set_status("Filtering by name, PID or user; Tab returns to the table.");
      }
      return true;
    case U'q': quit(); return true;
    case U'h':
    case U'?': show_help(); return true;
    case U'P': sort_by(ProcessSort::Cpu); return true;
    case U'M': sort_by(ProcessSort::Memory); return true;
    case U'N': sort_by(ProcessSort::Pid); return true;
    case U'T': sort_by(ProcessSort::Time); return true;
    case U'R': reverse_sort(); return true;
    case U'd':
    case U's': show_delay_prompt(); return true;
    case U'1':
      m_cpu.set_per_cpu(!m_cpu.per_cpu());
      set_status(m_cpu.per_cpu() ? "CPU: per-core" : "CPU: aggregate");
      return true;
    case U'l':
      m_show_overview = !m_show_overview;
      set_status(m_show_overview ? "Overview shown" : "Overview hidden");
      if (m_geometry_initialized) layout(m_grid.w, m_grid.h);
      return true;
    case U't':
      m_show_cpu = !m_show_cpu;
      set_status(m_show_cpu ? "CPU shown" : "CPU hidden");
      if (m_geometry_initialized) layout(m_grid.w, m_grid.h);
      return true;
    case U'm':
      m_show_memory = !m_show_memory;
      set_status(m_show_memory ? "Memory shown" : "Memory hidden");
      if (m_geometry_initialized) layout(m_grid.w, m_grid.h);
      return true;
    case U'c':
      m_processes.set_command_line(!m_processes.command_line());
      set_status(m_processes.command_line() ? "COMMAND: full command line"
                                            : "COMMAND: program name");
      return true;
    case U' ':
      refresh();
      m_sample_elapsed = {};
      return true;
    default: return false;
  }
}

auto ForgeTopApp::on_event(const Event& event) -> void {
  if (const auto* key = std::get_if<KeyEvent>(&event);
      key && key->action == KeyAction::Press && key->ctrl && key->ch == U'c') {
    App::on_event(event);
    return;
  }
  if (const auto* size = std::get_if<ResizeEvent>(&event)) {
    m_menu.close_dropdown();
    layout(size->cols, size->rows);
    cancel_small_forms();
    return;
  }
  if (const auto* error = std::get_if<ErrorEvent>(&event)) {
    set_status(std::format("{}: {}", error->source, error->message));
    return;
  }
  if (!m_usable) {
    if (const auto* key = std::get_if<KeyEvent>(&event);
        key && key->action == KeyAction::Press) {
      if (key->key == Key::F1 || key->ch == U'h' || key->ch == U'?')
        show_help();
      else if (key->key == Key::Escape || key->ch == U'q' ||
               (key->ctrl && key->ch == U'c'))
        App::on_event(event);
      if (key->ch == U'q') quit();
    }
    return;
  }

  if (const auto* mouse = std::get_if<MouseEvent>(&event)) {
    if (m_menu.dropdown_open() && m_menu.hit_test(mouse->x, mouse->y)) {
      if (mouse->pressed) m_focus.focus(&m_menu);
      (void)m_menu.on_event(event);
      return;
    }
    if (mouse->pressed && m_menu.dropdown_open()) m_menu.close_dropdown();
    if (m_show_processes && m_processes.handle_header_click(*mouse)) {
      set_status(std::format("Sort: {}{}", sort_label(m_processes.sort_key()),
                             m_processes.descending() ? "v" : "^"));
      return;
    }
    if (mouse->pressed) m_focus.focus_at(mouse->x, mouse->y);
    if (!m_show_processes) {
      (void)route_mouse(*mouse, {&m_menu});
    } else {
      (void)route_mouse(*mouse,
                        {&m_processes.filter(), &m_processes.table(), &m_menu});
    }
    return;
  }

  if (const auto* key = std::get_if<KeyEvent>(&event);
      key && key->action == KeyAction::Press && key->key == Key::Char &&
      key->ch == U'q' && !key->ctrl && !key->alt && m_menu.dropdown_open()) {
    m_menu.close_dropdown();
    return;
  }
  if (m_focus.current() == &m_processes.filter()) {
    if (std::holds_alternative<PasteEvent>(event)) {
      set_status("Filter paste unsupported; type a name, PID or user.");
      return;
    }
    if (const auto* key = std::get_if<KeyEvent>(&event);
        key && key->action != KeyAction::Release &&
        (key->key == Key::Enter || key->key == Key::Escape)) {
      (void)m_focus.focus(&m_processes.table());
      set_status(std::format("Filter: {} matches; table focused.",
                             m_processes.visible_rows().size()));
      return;
    }
  }
  const auto previous_filter = m_processes.filter().text();
  if (m_focus.handle_key(event)) {
    if (previous_filter != m_processes.filter().text())
      set_status(std::format("Filter: {} matches.",
                             m_processes.visible_rows().size()));
    return;
  }
  if (const auto* key = std::get_if<KeyEvent>(&event);
      key && handle_global_key(*key))
    return;
  if (const auto* key = std::get_if<KeyEvent>(&event);
      key && key->action != KeyAction::Release && key->key == Key::Enter &&
      m_focus.current() == &m_processes.table() &&
      m_processes.activate_selected())
    return;
  App::on_event(event);
}

auto ForgeTopApp::on_tick(std::chrono::duration<double> dt) -> void {
  if (!std::isfinite(dt.count()) || dt.count() < 0.0) return;
  m_sample_age = std::chrono::duration<double>{
      std::min(1.0e9, m_sample_age.count() + dt.count())};
  if (m_sample_delay <= std::chrono::duration<double>::zero()) {
    refresh();
    m_sample_elapsed = {};
    return;
  }
  m_sample_elapsed += dt;
  if (m_sample_elapsed >= m_sample_delay) {
    m_sample_elapsed -= m_sample_delay;
    if (m_sample_elapsed >= m_sample_delay) m_sample_elapsed = {};
    refresh();
  }
}

auto ForgeTopApp::layout(int cols, int rows) -> void {
  m_grid = {std::max(0, cols), std::max(0, rows)};
  m_geometry_initialized = true;
  const bool was_usable = m_usable;
  m_usable = cols >= 24 && rows >= 8;
  m_compact = cols < 60 || rows < 20;
  m_wide = !m_compact && cols >= 110 && rows >= 24;
  m_overview.set_compact(m_compact);
  m_memory.set_compact(m_compact);
  m_processes.set_compact(m_compact);
  m_overview.set_geometry({});
  m_memory.set_geometry({});
  m_cpu.set_geometry({});
  m_processes.set_geometry({});
  m_menu.set_geometry(m_usable ? Rect{0, 0, cols, 1} : Rect{});
  const Rect content{0, 2, m_grid.w, std::max(0, m_grid.h - 4)};
  if (m_usable) {
    if (!m_have_sample) {
      if (m_show_processes) m_processes.set_geometry(content);
    } else if (m_compact && m_show_processes) {
      m_processes.set_geometry(content);
    } else if (m_wide && m_show_processes &&
               (m_show_overview || m_show_cpu || m_show_memory)) {
      const int sidebar = std::min(36 + 8 * m_summary_balance, cols / 2);
      int y = content.y;
      if (m_show_overview) {
        m_overview.set_geometry({0, y, sidebar, 4});
        y += 4;
      }
      if (m_show_memory) {
        m_memory.set_geometry({0, y, sidebar, 4});
        y += 4;
      }
      if (m_show_cpu)
        m_cpu.set_geometry(
            {0, y, sidebar, std::max(0, content.y + content.h - y)});
      m_processes.set_geometry(
          {sidebar + 1, content.y, cols - sidebar - 1, content.h});
    } else {
      const int budget =
          m_show_processes ? std::max(0, content.h - 5) : content.h;
      int y = content.y, remaining = budget;
      const int summary_rows = m_compact ? 2 : 4;
      if (!m_compact && m_show_overview && m_show_memory) {
        const int h = std::min(summary_rows, remaining);
        const int left = (cols - 1) / 2;
        m_overview.set_geometry({0, y, left, h});
        m_memory.set_geometry({left + 1, y, cols - left - 1, h});
        y += h;
        remaining -= h;
      } else {
        if (m_show_overview) {
          const int h = std::min(summary_rows, remaining);
          m_overview.set_geometry({0, y, cols, h});
          y += h;
          remaining -= h;
        }
        if (m_show_memory) {
          const int h = std::min(summary_rows, remaining);
          m_memory.set_geometry({0, y, cols, h});
          y += h;
          remaining -= h;
        }
      }
      if (m_show_cpu) {
        const int want =
            m_show_processes ? 5 + 3 * m_summary_balance : remaining;
        const int h = std::min(want, remaining);
        m_cpu.set_geometry({0, y, cols, h});
        y += h;
      }
      if (m_show_processes)
        m_processes.set_geometry(
            {0, y, cols, std::max(0, content.y + content.h - y)});
    }
  }
  m_processes.layout();
  if (was_usable != m_usable) rebuild_focus();
}

auto ForgeTopApp::cancel_small_forms() -> void {
  if (m_usable || !top_overlay() || top_overlay() == &m_help) return;
  while (auto* widget = top_overlay()) {
    (void)widget->on_event(KeyEvent{.key = Key::Escape});
    if (top_overlay() == widget) pop_overlay();
  }
  set_status("Resize >=24x8; form cancelled.");
}

auto ForgeTopApp::on_render(Screen& screen) -> void {
  screen.clear();
  const int width = screen.cols(), height = screen.rows();
  layout(width, height);
  cancel_small_forms();
  // Draw even hidden panels: their producers and child geometry cannot retain
  // yesterday's rectangles. Only visible CPU regions are collected below.
  m_overview.draw(screen);
  m_memory.draw(screen);
  m_cpu.draw(screen);
  m_processes.draw(screen);
  if (!m_usable) {
    m_menu.close_dropdown();
    m_menu.draw(screen);
    ui_line(screen, {0, 0, width, std::min(1, height)}, "Resize >=24x8",
            kAccent);
    if (height > 1) ui_line(screen, {0, 1, width, 1}, "F1 help; q/Esc quit");
    return;
  }
  if (!m_menu.dropdown_open() && !m_cpu.rect().empty())
    render_pixel_regions(m_cpu);
  if (!m_have_sample) {
    const auto table = m_processes.table().rect();
    ui_line(screen, {0, m_show_processes ? table.y + 1 : 2, width, 1},
            "No sample; Space retries, F1 help.");
  } else if (!m_show_processes && m_overview.rect().empty() &&
             m_memory.rect().empty() && m_cpu.rect().empty())
    ui_line(screen, {0, 2, width, 1}, "Nothing visible. F2 restores ALL.");

  const auto sort = std::format("{}{}", sort_label(m_processes.sort_key()),
                                m_processes.descending() ? "v" : "^");
  const auto shown = m_processes.visible_rows().size();
  std::string heading;
  if (m_compact) {
    heading = std::format("{}(F2)", preset_name());
    if (!m_have_sample)
      heading += " no sample";
    else {
      if (m_show_cpu)
        heading +=
            std::format(" CPU{:.0f}%", m_snapshot.aggregate_cpu.usage * 100.0F);
      if (m_show_memory) {
        const double used =
            m_snapshot.memory.total_bytes == 0
                ? 0.0
                : static_cast<double>(
                      m_snapshot.memory.total_bytes -
                      std::min(m_snapshot.memory.total_bytes,
                               m_snapshot.memory.available_bytes)) *
                      100.0 /
                      static_cast<double>(m_snapshot.memory.total_bytes);
        heading += std::format(" RAM{:.0f}%", used);
      }
      if (!m_show_cpu && !m_show_memory)
        heading +=
            std::format(" {}/{} rows", shown, m_snapshot.processes.size());
    }
  } else {
    heading = std::format(
        "{} / {} | {}/{} processes | {} | balance {}/3", preset_name(),
        layout_name(), shown, m_snapshot.processes.size(),
        m_cpu.per_cpu() ? "per-core" : "aggregate", m_summary_balance + 1);
    if (!m_have_sample) heading = "No successful sample | Space retries";
  }
  ui_line(screen, {0, 1, width, 1}, heading, kAccent);
  const auto feedback =
      m_sample_error.empty()
          ? m_status
          : (m_have_sample ? "STALE: " : "NO SAMPLE: ") + m_sample_error;
  const auto feedback_color =
      m_sample_error.empty() ? theme::kFg : Rgb{255, 160, 112};
  if (m_compact) {
    ui_line(screen, {0, height - 2, width, 1},
            m_sample_error.empty() && m_status == "Ready." ? sample_status()
                                                           : feedback,
            feedback_color);
    ui_line(screen, {0, height - 1, width, 1},
            std::format("{} {} d{:.3g}s F1? /", focus_name(), sort,
                        m_sample_delay.count()),
            theme::kDim);
  } else {
    const auto sample = sample_status();
    const int sample_cols = std::min(detail::display_width(sample), width / 2);
    ui_line(screen, {0, height - 2, width, 1}, "");
    ui_line(screen, {0, height - 2, width - sample_cols - 1, 1}, feedback,
            feedback_color);
    ui_line(screen, {width - sample_cols, height - 2, sample_cols, 1}, sample,
            theme::kDim);
    ui_line(screen, {0, height - 1, width, 1},
            std::format(
                "{} | {} | {}/{} | F1? F2view F3balance /filter Tabfocus qquit",
                focus_name(), sort, shown, m_snapshot.processes.size()),
            theme::kDim);
  }
  m_menu.draw(
      screen); // dropdowns paint last, with no enhanced graphs through them
  const auto brand = width >= 40
                         ? (m_simulated ? "FORGE TOP / DEMO" : "FORGE TOP")
                         : (m_simulated ? "DEMO" : "");
  const int brand_cols = detail::display_width(brand);
  if (brand_cols > 0)
    ui_line(screen, {width - brand_cols, 0, brand_cols, 1}, brand, kAccent);
}

} // namespace termforge::forge_top
