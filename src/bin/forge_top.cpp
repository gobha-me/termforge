#include "forge_top.hpp"

#include <algorithm>
#include <format>
#include <utility>
#include <variant>

#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"
#include "termforge/widgets/theme.hpp"

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

ForgeTopApp::ForgeTopApp(std::unique_ptr<SystemReader> reader)
    : m_reader(std::move(reader)),
      m_help{"forge-top help",
             "Tab moves focus. Arrow keys navigate the table and menus. "
             "Enter opens the selected process. Click a table header to "
             "sort. Escape closes a popup or exits."} {
  if (!m_reader)
    m_reader = make_fake_reader();
  set_frame_ms(33);

  m_processes.on_activate(
      [this](const ProcessRow &process) { open_detail(process); });
  m_detail.on_close([this] { pop_overlay(); });
  m_help.on_close([this] { pop_overlay(); });

  m_menu.set_menus({
      {"Sort",
       {{"CPU", [this] { m_processes.choose_sort(ProcessSort::Cpu); }},
        {"Memory", [this] { m_processes.choose_sort(ProcessSort::Memory); }},
        {"PID", [this] { m_processes.choose_sort(ProcessSort::Pid); }},
        {"Name", [this] { m_processes.choose_sort(ProcessSort::Name); }}}},
      {"View",
       {{"All panels", [this] { set_view(View::All); }},
        {"Processes", [this] { set_view(View::Processes); }},
        {"Graphs", [this] { set_view(View::Graphs); }}}},
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
  m_cpu.set_style(style);
  m_memory.set_style(style);
  m_processes.set_style(style);
  m_menu.set_style(style);
  m_detail.set_style(style);
  m_help.set_border_style(style);
}

auto ForgeTopApp::on_start() -> void {
  const auto caps = driver().capabilities();
  apply_style(!caps.kitty_graphics && !caps.truecolor);
}

auto ForgeTopApp::rebuild_focus() -> void {
  m_focus.clear();
  if (m_view != View::Graphs) {
    m_focus.add(&m_processes.filter());
    m_focus.add(&m_processes.table());
    m_focus.focus(&m_processes.table());
  }
  m_focus.add(&m_menu);
}

auto ForgeTopApp::set_view(View view) -> void {
  m_view = view;
  rebuild_focus();
  set_status(view == View::All      ? "View: all panels"
             : view == View::Graphs ? "View: graphs"
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
  m_cpu.set_samples(m_snapshot.cpus);
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
    if (m_view != View::Graphs && m_processes.handle_header_click(*mouse))
      return;
    if (mouse->pressed)
      m_focus.focus_at(mouse->x, mouse->y);
    if (m_view == View::Graphs) {
      (void)route_mouse(*mouse, {&m_menu});
    } else {
      (void)route_mouse(*mouse,
                        {&m_processes.filter(), &m_processes.table(), &m_menu});
    }
    return;
  }

  if (const auto *key = std::get_if<KeyEvent>(&event);
      key && key->key == Key::F1) {
    show_help();
    return;
  }
  if (m_focus.handle_key(event))
    return;
  if (const auto *key = std::get_if<KeyEvent>(&event);
      key && key->key == Key::Enter &&
      m_focus.current() == &m_processes.table() &&
      m_processes.activate_selected())
    return;
  App::on_event(event);
}

auto ForgeTopApp::on_tick(std::chrono::duration<double> dt) -> void {
  m_sample_elapsed += dt;
  if (m_sample_elapsed >= std::chrono::seconds{1}) {
    m_sample_elapsed -= std::chrono::seconds{1};
    if (m_sample_elapsed >= std::chrono::seconds{1})
      m_sample_elapsed = {};
    refresh();
  }
}

auto ForgeTopApp::on_render(Screen &screen) -> void {
  screen.clear();
  const int width = screen.cols();
  const int height = screen.rows();
  const Rect content{0, 1, width, std::max(0, height - 2)};

  if (m_view == View::Processes) {
    m_processes.set_geometry(content);
    m_processes.draw(screen);
  } else if (m_view == View::Graphs) {
    const int memory_h = std::min(4, content.h);
    m_cpu.set_geometry(
        {content.x, content.y, content.w, std::max(0, content.h - memory_h)});
    m_memory.set_geometry(
        {content.x, content.y + content.h - memory_h, content.w, memory_h});
    m_cpu.draw(screen);
    m_memory.draw(screen);
    render_pixel_regions(m_cpu);
  } else {
    const int graph_h =
        content.h <= 10 ? content.h / 2 : std::clamp(content.h * 3 / 5, 8, 18);
    const int memory_h = std::min(4, graph_h);
    m_cpu.set_geometry(
        {content.x, content.y, content.w, std::max(0, graph_h - memory_h)});
    m_memory.set_geometry(
        {content.x, content.y + graph_h - memory_h, content.w, memory_h});
    m_processes.set_geometry({content.x, content.y + graph_h, content.w,
                              std::max(0, content.h - graph_h)});
    m_cpu.draw(screen);
    m_memory.draw(screen);
    m_processes.draw(screen);
    render_pixel_regions(m_cpu);
  }

  if (height > 0)
    screen.write_text(0, height - 1, m_status, theme::kDim,
                      Rgb{0x10, 0x10, 0x20});
  m_menu.set_geometry({0, 0, width, height > 0 ? 1 : 0});
  m_menu.draw(screen); // dropdowns paint last
}

} // namespace termforge::forge_top
