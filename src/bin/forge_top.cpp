#include "forge_top.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <utility>
#include <variant>

#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"
#include "termforge/widgets/theme.hpp"
#include "termforge/widgets/detail/width.hpp"

namespace termforge::forge_top {
namespace {

auto capabilities_for(DriverChoice choice) -> Capabilities {
  Capabilities caps;
  if (choice == DriverChoice::Kitty) {
    caps.kitty_graphics = true;
    caps.truecolor = true;
    caps.color_levels = 24;
  } else if (choice == DriverChoice::Ansi) {
    caps.truecolor = true;
    caps.color_levels = 24;
  }
  return caps;
}

auto make_driver(DriverChoice choice) -> std::unique_ptr<TerminalDriver> {
  switch (choice) {
  case DriverChoice::Kitty:
    return std::make_unique<KittyDriver>();
  case DriverChoice::Ansi:
    return std::make_unique<AnsiRgbDriver>();
  case DriverChoice::Fallback:
    return std::make_unique<FallbackDriver>();
  case DriverChoice::Automatic:
    return std::make_unique<FallbackDriver>();
  }
  return std::make_unique<FallbackDriver>();
}

} // namespace

HelpPopup::HelpPopup() : Dialog{"forge-top help"} {
  set_text(
      "top-compatible keys\n"
      "  q quit · h/?/F1 help · Space refresh\n"
      "  P %CPU · M %MEM · N PID · T TIME+ · R reverse\n"
      "  d/s sampling delay · 1 aggregate/per-CPU\n"
      "  l overview · t CPU · m memory · c command line\n\n"
      "forge-top navigation\n"
      "  Tab changes focus; arrows, Page Up/Down, Home/End navigate.\n"
      "  Enter opens process detail (unlike top); Escape closes a popup.\n"
      "  Menus and table headers mirror the same actions. k/r are omitted.");
  set_max_width(72);
}

auto HelpPopup::on_event(const Event &event) -> bool {
  if (const auto *key = std::get_if<KeyEvent>(&event);
      key && key->action == KeyAction::Press && key->key == Key::Char &&
      !key->ctrl && !key->alt &&
      (key->ch == U'q' || key->ch == U'h' || key->ch == U'?')) {
    close();
    return true;
  }
  if (const auto *key = std::get_if<KeyEvent>(&event);
      key && key->action == KeyAction::Press && key->key == Key::F1) {
    close();
    return true;
  }
  return Dialog::on_event(event);
}

ForgeTopApp::ForgeTopApp(std::unique_ptr<SystemReader> reader)
    : m_reader(std::move(reader)) {
  if (!m_reader)
    m_reader = make_fake_reader();
  set_frame_ms(33);

  m_processes.on_activate(
      [this](const ProcessRow &process) { open_detail(process); });
  m_detail.on_close([this] { pop_overlay(); });
  m_help.on_close([this] { pop_overlay(); });
  m_delay_prompt.on_close([this] { pop_overlay(); });
  m_delay_prompt.on_submit(
      [this](std::string value) { apply_delay(std::move(value)); });

  m_menu.set_menus({
      {"Sort",
       {{"%CPU (P)", [this] { m_processes.set_sort(ProcessSort::Cpu); }},
        {"%MEM (M)", [this] { m_processes.set_sort(ProcessSort::Memory); }},
        {"PID (N)", [this] { m_processes.set_sort(ProcessSort::Pid); }},
        {"TIME+ (T)", [this] { m_processes.set_sort(ProcessSort::Time); }},
        {"Command", [this] { m_processes.set_sort(ProcessSort::Command); }},
        {"Reverse (R)", [this] { m_processes.reverse_sort(); }}}},
      {"View",
       {{"All panels", [this] { set_preset(Preset::All); }},
        {"Processes", [this] { set_preset(Preset::Processes); }},
        {"Summary", [this] { set_preset(Preset::Summary); }},
        {"Aggregate/per-CPU (1)",
         [this] { m_cpu.set_per_cpu(!m_cpu.per_cpu()); }},
        {"Command name/line (c)", [this] {
           m_processes.set_command_line(!m_processes.command_line());
         }}}},
      {"Help", {{"Keys", [this] { show_help(); }}}},
  });
  rebuild_focus();
  refresh();
}

auto ForgeTopApp::force_driver(DriverChoice choice)
    -> std::expected<void, ErrorEvent> {
  if (choice == DriverChoice::Automatic)
    return {};
  return terminal().set_capabilities(capabilities_for(choice));
}

auto ForgeTopApp::run_headless(int frames, int cols, int rows,
                               std::string *sink, DriverChoice choice) -> void {
  if (choice == DriverChoice::Automatic)
    choice = DriverChoice::Fallback;
  apply_style(choice == DriverChoice::Fallback);
  test_run_frames(frames, cols, rows, sink, make_driver(choice));
}

auto ForgeTopApp::show_first_process_for_test() -> bool {
  if (m_snapshot.processes.empty())
    return false;
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
  m_focus.clear();
  if (m_show_processes) {
    m_focus.add(&m_processes.filter());
    m_focus.add(&m_processes.table());
    m_focus.focus(&m_processes.table());
  }
  m_focus.add(&m_menu);
}

auto ForgeTopApp::set_preset(Preset preset) -> void {
  m_show_overview = preset != Preset::Processes;
  m_show_cpu = preset != Preset::Processes;
  m_show_memory = preset != Preset::Processes;
  m_show_processes = preset != Preset::Summary;
  rebuild_focus();
  set_status(preset == Preset::All        ? "View: all panels"
             : preset == Preset::Summary ? "View: summary"
                                         : "View: processes");
}

auto ForgeTopApp::refresh() -> void {
  auto snapshot = m_reader->sample();
  if (!snapshot) {
    set_status(std::format("{}: {}", snapshot.error().source,
                           snapshot.error().message));
    return;
  }
  m_snapshot = std::move(*snapshot);
  m_overview.set_snapshot(m_snapshot.uptime_seconds, m_snapshot.load_average,
                          m_snapshot.tasks);
  m_cpu.set_samples(m_snapshot.cpus);
  m_cpu.set_aggregate_sample(m_snapshot.aggregate_cpu);
  m_memory.set_memory(m_snapshot.memory);

  std::unordered_map<int, std::vector<float>> next_history;
  next_history.reserve(m_snapshot.processes.size());
  for (const auto &process : m_snapshot.processes) {
    auto history = std::move(m_history[process.pid]);
    history.push_back(process.cpu_percent);
    if (history.size() > 160)
      history.erase(history.begin());
    next_history.emplace(process.pid, std::move(history));
  }
  m_history = std::move(next_history);
  m_processes.set_processes(m_snapshot.processes);
  set_status(std::format("{} cores · {} processes · sample ready",
                         m_snapshot.cpus.size(), m_snapshot.processes.size()));
  update_detail();
}

auto ForgeTopApp::open_detail(const ProcessRow &process) -> void {
  const auto it = m_history.find(process.pid);
  const std::span<const float> history =
      it == m_history.end()
          ? std::span<const float>{}
          : std::span<const float>{it->second.data(), it->second.size()};
  m_detail.set_process(process, history);
  if (top_overlay() != &m_detail)
    push_overlay(m_detail);
}

auto ForgeTopApp::update_detail() -> void {
  if (top_overlay() != &m_detail || m_detail.pid() < 0)
    return;
  const auto process =
      std::ranges::find(m_snapshot.processes, m_detail.pid(), &ProcessRow::pid);
  if (process == m_snapshot.processes.end()) {
    pop_overlay();
    set_status(std::format("PID {} exited", m_detail.pid()));
    return;
  }
  open_detail(*process);
}

auto ForgeTopApp::show_help() -> void {
  if (top_overlay() != &m_help)
    push_overlay(m_help);
}

auto ForgeTopApp::show_delay_prompt() -> void {
  m_delay_prompt.set_text("Seconds between samples (0 = every frame):");
  m_delay_prompt.set_value(std::format("{:.3g}", m_sample_delay.count()));
  if (top_overlay() != &m_delay_prompt)
    push_overlay(m_delay_prompt);
}

auto ForgeTopApp::apply_delay(std::string value) -> void {
  double seconds = 0.0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), seconds);
  if (error != std::errc{} || end != value.data() + value.size() ||
      !std::isfinite(seconds) || seconds < 0.0) {
    set_status("Delay must be a finite, non-negative number");
    m_delay_prompt.set_text(
        "Invalid delay. Enter finite seconds >= 0 (0 = every frame):");
    m_delay_prompt.set_value(std::move(value));
    push_overlay(m_delay_prompt);
    return;
  }
  m_sample_delay = std::chrono::duration<double>{seconds};
  m_sample_elapsed = {};
  set_status(std::format("Sampling delay set to {:.3g}s", seconds));
}

auto ForgeTopApp::handle_global_key(const KeyEvent &key) -> bool {
  if (key.action != KeyAction::Press)
    return false;
  if (key.key == Key::F1) {
    show_help();
    return true;
  }
  if (key.key != Key::Char || key.ctrl || key.alt)
    return false;
  switch (key.ch) {
  case U'q':
    quit();
    return true;
  case U'h':
  case U'?':
    show_help();
    return true;
  case U'P':
    m_processes.set_sort(ProcessSort::Cpu);
    return true;
  case U'M':
    m_processes.set_sort(ProcessSort::Memory);
    return true;
  case U'N':
    m_processes.set_sort(ProcessSort::Pid);
    return true;
  case U'T':
    m_processes.set_sort(ProcessSort::Time);
    return true;
  case U'R':
    m_processes.reverse_sort();
    return true;
  case U'd':
  case U's':
    show_delay_prompt();
    return true;
  case U'1':
    m_cpu.set_per_cpu(!m_cpu.per_cpu());
    set_status(m_cpu.per_cpu() ? "CPU: per-core" : "CPU: aggregate");
    return true;
  case U'l':
    m_show_overview = !m_show_overview;
    set_status(m_show_overview ? "Overview shown" : "Overview hidden");
    return true;
  case U't':
    m_show_cpu = !m_show_cpu;
    set_status(m_show_cpu ? "CPU shown" : "CPU hidden");
    return true;
  case U'm':
    m_show_memory = !m_show_memory;
    set_status(m_show_memory ? "Memory shown" : "Memory hidden");
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
  default:
    return false;
  }
}

auto ForgeTopApp::on_event(const Event &event) -> void {
  if (const auto *error = std::get_if<ErrorEvent>(&event)) {
    set_status(std::format("{}: {}", error->source, error->message));
    return;
  }

  if (const auto *mouse = std::get_if<MouseEvent>(&event)) {
    if (m_menu.dropdown_open() && m_menu.hit_test(mouse->x, mouse->y)) {
      if (mouse->pressed)
        m_focus.focus(&m_menu);
      (void)m_menu.on_event(event);
      return;
    }
    if (mouse->pressed && m_menu.dropdown_open())
      m_menu.close_dropdown();
    if (m_show_processes && m_processes.handle_header_click(*mouse))
      return;
    if (mouse->pressed)
      m_focus.focus_at(mouse->x, mouse->y);
    if (!m_show_processes) {
      (void)route_mouse(*mouse, {&m_menu});
    } else {
      (void)route_mouse(*mouse,
                        {&m_processes.filter(), &m_processes.table(), &m_menu});
    }
    return;
  }

  if (const auto *key = std::get_if<KeyEvent>(&event);
      key && key->action == KeyAction::Press && key->key == Key::Char &&
      key->ch == U'q' && !key->ctrl && !key->alt && m_menu.dropdown_open()) {
    m_menu.close_dropdown();
    return;
  }
  if (m_focus.handle_key(event))
    return;
  if (const auto *key = std::get_if<KeyEvent>(&event);
      key && handle_global_key(*key))
    return;
  if (const auto *key = std::get_if<KeyEvent>(&event);
      key && key->action != KeyAction::Release && key->key == Key::Enter &&
      m_focus.current() == &m_processes.table() &&
      m_processes.activate_selected())
    return;
  App::on_event(event);
}

auto ForgeTopApp::on_tick(std::chrono::duration<double> dt) -> void {
  if (m_sample_delay <= std::chrono::duration<double>::zero()) {
    refresh();
    m_sample_elapsed = {};
    return;
  }
  m_sample_elapsed += dt;
  if (m_sample_elapsed >= m_sample_delay) {
    m_sample_elapsed -= m_sample_delay;
    if (m_sample_elapsed >= m_sample_delay)
      m_sample_elapsed = {};
    refresh();
  }
}

auto ForgeTopApp::on_render(Screen &screen) -> void {
  screen.clear();
  const int width = screen.cols();
  const int height = screen.rows();
  const Rect content{0, 1, width, std::max(0, height - 2)};

  m_overview.set_geometry({0, 0, 0, 0});
  m_cpu.set_geometry({0, 0, 0, 0});
  m_memory.set_geometry({0, 0, 0, 0});
  m_processes.set_geometry({0, 0, 0, 0});

  const int overview_want = m_show_overview ? 4 : 0;
  const int memory_want = m_show_memory ? 4 : 0;
  const int cpu_want = m_show_cpu
                           ? (m_show_processes
                                  ? std::clamp(content.h * 2 / 5, 8, 14)
                                  : std::max(3, content.h - overview_want -
                                                    memory_want))
                           : 0;
  const int summary_want = overview_want + cpu_want + memory_want;
  const int summary_budget =
      m_show_processes
          ? std::min(summary_want, std::max(0, content.h - 4))
          : std::min(summary_want, content.h);
  int remaining = summary_budget;
  const int overview_h = std::min(overview_want, remaining);
  remaining -= overview_h;
  const int memory_h = std::min(memory_want, remaining);
  const int cpu_h = std::max(0, remaining - memory_h);

  int y = content.y;
  if (overview_h > 0) {
    m_overview.set_geometry({content.x, y, content.w, overview_h});
    m_overview.draw(screen);
    y += overview_h;
  }
  if (cpu_h > 0) {
    m_cpu.set_geometry({content.x, y, content.w, cpu_h});
    m_cpu.draw(screen);
    render_pixel_regions(m_cpu);
    y += cpu_h;
  }
  if (memory_h > 0) {
    m_memory.set_geometry({content.x, y, content.w, memory_h});
    m_memory.draw(screen);
    y += memory_h;
  }
  if (m_show_processes) {
    m_processes.set_geometry(
        {content.x, y, content.w, std::max(0, content.y + content.h - y)});
    m_processes.draw(screen);
  }

  if (height > 0) {
    const std::string delay = std::format("delay {:.3g}s", m_sample_delay.count());
    const int delay_cols = detail::display_width(delay);
    const int left_cols = std::max(0, width - delay_cols - 1);
    screen.write_text(0, height - 1,
                      detail::truncate_to_width(m_status, left_cols),
                      theme::kDim, Rgb{0x10, 0x10, 0x20});
    if (delay_cols <= width)
      screen.write_text(width - delay_cols, height - 1, delay, theme::kDim,
                        Rgb{0x10, 0x10, 0x20});
  }
  m_menu.set_geometry({0, 0, width, height > 0 ? 1 : 0});
  m_menu.draw(screen); // dropdowns paint last
}

} // namespace termforge::forge_top
