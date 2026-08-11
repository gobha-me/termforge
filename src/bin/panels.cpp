#include "panels.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <ranges>

#include "termforge/core/screen.hpp"
#include "termforge/widgets/theme.hpp"

namespace termforge::forge_top {
namespace {

auto used_fraction(std::uint64_t total, std::uint64_t available) -> float {
  if (total == 0)
    return 0.0F;
  return std::clamp(static_cast<float>(total - std::min(total, available)) /
                        static_cast<float>(total),
                    0.0F, 1.0F);
}

auto human_bytes(std::uint64_t bytes) -> std::string {
  constexpr std::uint64_t kib = 1024;
  constexpr std::uint64_t mib = kib * 1024;
  constexpr std::uint64_t gib = mib * 1024;
  if (bytes >= gib)
    return std::format("{:.1f}G", static_cast<double>(bytes) / gib);
  if (bytes >= mib)
    return std::format("{:.1f}M", static_cast<double>(bytes) / mib);
  if (bytes >= kib)
    return std::format("{:.1f}K", static_cast<double>(bytes) / kib);
  return std::format("{}B", bytes);
}

auto ascii_lower(std::string text) -> std::string {
  std::ranges::transform(text, text.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return text;
}

auto sort_name(ProcessSort sort) -> std::string_view {
  switch (sort) {
  case ProcessSort::Cpu:
    return "CPU";
  case ProcessSort::Memory:
    return "memory";
  case ProcessSort::Pid:
    return "PID";
  case ProcessSort::Name:
    return "name";
  }
  return "CPU";
}

} // namespace

CpuPanel::CpuPanel() { m_frame.set_style(BorderStyle::Rounded); }

auto CpuPanel::set_style(BorderStyle style) -> void {
  m_frame.set_style(style);
}

auto CpuPanel::set_samples(std::span<const CpuSample> samples) -> void {
  while (m_waves.size() < samples.size()) {
    auto wave = std::make_unique<WaveformWidget>(120);
    wave->set_range(0.0F, 1.0F);
    m_waves.push_back(std::move(wave));
  }
  if (m_waves.size() > samples.size())
    m_waves.resize(samples.size());
  m_names.resize(samples.size());
  m_usage.resize(samples.size());
  for (std::size_t i = 0; i < samples.size(); ++i) {
    m_names[i] = samples[i].name;
    m_usage[i] = samples[i].usage;
    m_waves[i]->push(samples[i].usage);
  }
  mark_dirty();
}

auto CpuPanel::draw(Screen &screen) -> void {
  m_frame.set_geometry(rect());
  m_frame.draw(screen);
  const Rect inner = m_frame.content_rect();
  if (inner.w <= 0 || inner.h <= 0 || m_waves.empty()) {
    for (auto &wave : m_waves)
      wave->set_geometry({0, 0, 0, 0});
    clear_dirty();
    return;
  }

  const int count = static_cast<int>(m_waves.size());
  const int max_rows = std::max(1, inner.h / 2);
  const int needed_columns = (count + max_rows - 1) / max_rows;
  const int max_columns = std::clamp(inner.w / 9, 1, count);
  const int ideal_columns = std::clamp(inner.w / 18, 1, max_columns);
  const int columns =
      std::min(max_columns, std::max(ideal_columns, needed_columns));
  const int rows = (count + columns - 1) / columns;
  const int tile_w = std::max(1, inner.w / columns);
  const int tile_h = std::max(2, inner.h / std::max(1, rows));
  const bool compact_labels = tile_w < 10;
  for (int i = 0; i < count; ++i) {
    const int col = i % columns;
    const int row = i / columns;
    const int x = inner.x + col * tile_w;
    const int y = inner.y + row * tile_h;
    const int w = col + 1 == columns ? inner.x + inner.w - x : tile_w;
    const int h = std::min(tile_h, inner.y + inner.h - y);
    if (w <= 0 || h <= 1 || y >= inner.y + inner.h) {
      m_waves[static_cast<std::size_t>(i)]->set_geometry({0, 0, 0, 0});
      continue;
    }
    std::string name = m_names[static_cast<std::size_t>(i)];
    if (compact_labels && name.starts_with("cpu"))
      name = "c" + name.substr(3);
    std::string label = std::format(
        "{} {:3.0f}%", name, m_usage[static_cast<std::size_t>(i)] * 100.0F);
    if (label.size() > static_cast<std::size_t>(w))
      label.resize(static_cast<std::size_t>(w));
    screen.write_text(x, y, label, theme::kDim, theme::kBg);
    auto &wave = *m_waves[static_cast<std::size_t>(i)];
    wave.set_geometry({x, y + 1, w, h - 1});
    wave.draw(screen);
  }
  clear_dirty();
}

auto CpuPanel::pixel_regions() -> std::vector<Rect> {
  std::vector<Rect> regions;
  regions.reserve(m_waves.size());
  for (const auto &wave : m_waves) {
    const Rect r = wave->rect();
    if (r.w > 0 && r.h > 0)
      regions.push_back(r);
  }
  return regions;
}

auto CpuPanel::draw_pixels(Rect region, Extent preferred) -> const Image * {
  for (auto &wave : m_waves)
    if (wave->rect() == region)
      return wave->draw_pixels(region, preferred);
  return nullptr;
}

MemoryPanel::MemoryPanel() {
  m_frame.set_style(BorderStyle::Rounded);
  m_memory.set_colors(Rgb{0x00, 0xD4, 0xFF}, Rgb{0x20, 0x28, 0x38}, theme::kFg);
  m_swap.set_colors(Rgb{0xC0, 0x80, 0xFF}, Rgb{0x20, 0x28, 0x38}, theme::kFg);
}

auto MemoryPanel::set_style(BorderStyle style) -> void {
  m_frame.set_style(style);
}

auto MemoryPanel::set_memory(const MemoryInfo &memory) -> void {
  const auto memory_used =
      memory.total_bytes - std::min(memory.total_bytes, memory.available_bytes);
  const auto swap_used =
      memory.swap_total_bytes -
      std::min(memory.swap_total_bytes, memory.swap_free_bytes);
  m_memory.set_value(used_fraction(memory.total_bytes, memory.available_bytes));
  m_memory.set_label(std::format("RAM {} / {}", human_bytes(memory_used),
                                 human_bytes(memory.total_bytes)));
  m_swap.set_value(
      used_fraction(memory.swap_total_bytes, memory.swap_free_bytes));
  m_swap.set_label(memory.swap_total_bytes == 0
                       ? "Swap disabled"
                       : std::format("Swap {} / {}", human_bytes(swap_used),
                                     human_bytes(memory.swap_total_bytes)));
  mark_dirty();
}

auto MemoryPanel::draw(Screen &screen) -> void {
  m_frame.set_geometry(rect());
  m_frame.draw(screen);
  const Rect inner = m_frame.content_rect();
  m_memory.set_geometry({inner.x, inner.y, inner.w, inner.h > 0 ? 1 : 0});
  m_swap.set_geometry({inner.x, inner.y + 1, inner.w, inner.h > 1 ? 1 : 0});
  m_memory.draw(screen);
  m_swap.draw(screen);
  clear_dirty();
}

ProcessPanel::ProcessPanel() {
  m_frame.set_style(BorderStyle::Rounded);
  m_filter.set_placeholder("héllo — filtér…");
  m_filter.on_change([this](const std::string &) { rebuild(); });
  m_table.set_columns({{"PID", Align::Right, 7},
                       {"CPU%", Align::Right, 7},
                       {"RSS", Align::Right, 10},
                       {"Command", Align::Left, 0}});
  m_table.on_select([this](int row, const std::vector<std::string> &) {
    if (row < 0 || row >= static_cast<int>(m_visible.size()))
      return;
    auto callback = m_on_activate;
    if (callback)
      callback(m_visible[static_cast<std::size_t>(row)]);
  });
}

auto ProcessPanel::set_processes(std::vector<ProcessRow> processes) -> void {
  m_processes = std::move(processes);
  rebuild();
}

auto ProcessPanel::set_filter(std::string text) -> void {
  m_filter.set_text(std::move(text));
  rebuild();
}

auto ProcessPanel::set_style(BorderStyle style) -> void {
  m_style = style;
  m_frame.set_style(style);
  m_table.set_style(style);
  rebuild();
}

auto ProcessPanel::choose_sort(ProcessSort key) -> void {
  if (m_sort == key) {
    m_descending = !m_descending;
  } else {
    m_sort = key;
    m_descending = key == ProcessSort::Cpu || key == ProcessSort::Memory;
  }
  rebuild();
}

auto ProcessPanel::on_activate(std::function<void(const ProcessRow &)> callback)
    -> void {
  m_on_activate = std::move(callback);
}

auto ProcessPanel::selected_pid() const -> std::optional<int> {
  const int selected = m_table.selected();
  if (selected < 0 || selected >= static_cast<int>(m_visible.size()))
    return std::nullopt;
  return m_visible[static_cast<std::size_t>(selected)].pid;
}

auto ProcessPanel::activate_selected() -> bool {
  const int selected = m_table.selected();
  if (selected < 0 || selected >= static_cast<int>(m_visible.size()))
    return false;
  auto callback = m_on_activate;
  if (callback)
    callback(m_visible[static_cast<std::size_t>(selected)]);
  return true;
}

auto ProcessPanel::handle_header_click(const MouseEvent &mouse) -> bool {
  const Rect r = m_table.rect();
  if (!mouse.pressed || mouse.button != 0 || mouse.y != r.y ||
      !r.contains(mouse.x, mouse.y))
    return false;
  int x = mouse.x - r.x - m_table.gutter_cols();
  if (x < 0)
    return true;
  if (x < 7)
    choose_sort(ProcessSort::Pid);
  else if ((x -= 8) < 7)
    choose_sort(ProcessSort::Cpu);
  else if ((x -= 8) < 10)
    choose_sort(ProcessSort::Memory);
  else
    choose_sort(ProcessSort::Name);
  return true;
}

auto ProcessPanel::rebuild() -> void {
  const auto keep = selected_pid();
  const std::string needle = ascii_lower(m_filter.text());
  m_visible.clear();
  for (const auto &process : m_processes) {
    const std::string haystack =
        ascii_lower(std::format("{} {}", process.pid, process.name));
    if (needle.empty() || haystack.find(needle) != std::string::npos)
      m_visible.push_back(process);
  }

  const auto less = [this](const ProcessRow &lhs, const ProcessRow &rhs) {
    int order = 0;
    switch (m_sort) {
    case ProcessSort::Cpu:
      order = lhs.cpu_percent < rhs.cpu_percent
                  ? -1
                  : (lhs.cpu_percent > rhs.cpu_percent ? 1 : 0);
      break;
    case ProcessSort::Memory:
      order = lhs.rss_bytes < rhs.rss_bytes
                  ? -1
                  : (lhs.rss_bytes > rhs.rss_bytes ? 1 : 0);
      break;
    case ProcessSort::Pid:
      order = lhs.pid < rhs.pid ? -1 : (lhs.pid > rhs.pid ? 1 : 0);
      break;
    case ProcessSort::Name:
      order = lhs.name < rhs.name ? -1 : (lhs.name > rhs.name ? 1 : 0);
      break;
    }
    if (order == 0 && lhs.pid != rhs.pid)
      order = lhs.pid < rhs.pid ? -1 : 1;
    return m_descending ? order > 0 : order < 0;
  };
  std::ranges::sort(m_visible, less);

  m_table.clear_rows();
  for (const auto &process : m_visible) {
    m_table.add_row({std::format("{}", process.pid),
                     std::format("{:.1f}", process.cpu_percent),
                     human_bytes(process.rss_bytes),
                     Screen::sanitize(process.name)});
  }
  if (keep) {
    const auto it = std::ranges::find(m_visible, *keep, &ProcessRow::pid);
    if (it != m_visible.end())
      m_table.set_selected(static_cast<int>(it - m_visible.begin()));
  }
  m_frame.set_title(std::format(
      "Processes {} {} {}", is_ascii(m_style) ? "-" : "—", sort_name(m_sort),
      m_descending ? (is_ascii(m_style) ? "v" : "↓")
                   : (is_ascii(m_style) ? "^" : "↑")));
  mark_dirty();
}

auto ProcessPanel::draw(Screen &screen) -> void {
  m_frame.set_geometry(rect());
  m_frame.draw(screen);
  const Rect inner = m_frame.content_rect();
  m_filter.set_geometry({inner.x, inner.y, inner.w, inner.h > 0 ? 1 : 0});
  m_table.set_geometry(
      {inner.x, inner.y + 1, inner.w, std::max(0, inner.h - 1)});
  m_filter.draw(screen);
  m_table.draw(screen);
  clear_dirty();
}

DetailPopup::DetailPopup() : Dialog{"Process detail"} {
  set_max_width(56);
  set_border_style(BorderStyle::Rounded);
}

auto DetailPopup::set_process(const ProcessRow &process,
                              std::span<const float> history) -> void {
  m_pid = process.pid;
  set_title(
      std::format("{} ({})", Screen::sanitize(process.name), process.pid));
  set_text(std::format("CPU {:.1f}%   RSS {}", process.cpu_percent,
                       human_bytes(process.rss_bytes)));
  rasterize(history);
}

auto DetailPopup::rasterize(std::span<const float> history) -> void {
  Image &image = m_graph.image();
  image.fill({0, 0, image.width(), image.height()},
             Pixel{0x0B, 0x10, 0x1C, 0xFF});
  if (history.empty())
    return;
  const int w = image.width();
  const int h = image.height();
  for (int x = 0; x < w; ++x) {
    const std::size_t index = history.size() <= 1
                                  ? 0
                                  : static_cast<std::size_t>(x) *
                                        (history.size() - 1) /
                                        static_cast<std::size_t>(w - 1);
    const float value = std::clamp(history[index] / 100.0F, 0.0F, 1.0F);
    const int top = h - 1 - static_cast<int>(value * (h - 1));
    for (int y = top; y < h; ++y)
      image.at(x, y) = Pixel{0x00, 0xD4, 0xFF, 0xFF};
  }
}

auto DetailPopup::layout_content(Rect area) -> void {
  m_graph.set_geometry({area.x, area.y, area.w, area.h});
}

auto DetailPopup::draw_content(Screen &screen) -> void { m_graph.draw(screen); }

auto DetailPopup::pixel_regions() -> std::vector<Rect> {
  const Rect region = m_graph.rect();
  return region.w > 0 && region.h > 0 ? std::vector<Rect>{region}
                                      : std::vector<Rect>{};
}

auto DetailPopup::draw_pixels(Rect region, Extent preferred) -> const Image * {
  return m_graph.draw_pixels(region, preferred);
}

auto DetailPopup::pixel_region_state(Rect region) const noexcept
    -> PixelRegionState {
  return m_graph.pixel_region_state(region);
}

auto DetailPopup::pixel_region_submitted(Rect region) noexcept -> void {
  m_graph.pixel_region_submitted(region);
}

auto DetailPopup::pixel_fit(Rect region) const noexcept -> PlacementFit {
  return m_graph.pixel_fit(region);
}

} // namespace termforge::forge_top
