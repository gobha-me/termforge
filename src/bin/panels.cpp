#include "panels.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <limits>
#include <ranges>

#include "termforge/core/screen.hpp"
#include "termforge/widgets/theme.hpp"

namespace termforge::forge_top {
namespace {

auto used_fraction(std::uint64_t total, std::uint64_t available) -> float {
  if (total == 0) return 0.0F;
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

struct ByteScale {
  std::uint64_t divisor;
  std::string_view unit;
};

auto byte_scale(std::uint64_t total) -> ByteScale {
  constexpr std::uint64_t kib = 1024;
  constexpr std::uint64_t mib = kib * 1024;
  constexpr std::uint64_t gib = mib * 1024;
  if (total >= gib) return {gib, "GiB"};
  if (total >= mib) return {mib, "MiB"};
  if (total >= kib) return {kib, "KiB"};
  return {1, "B"};
}

auto scaled_bytes(std::uint64_t bytes, ByteScale scale) -> std::string {
  return std::format("{:.1f}", static_cast<double>(bytes) / scale.divisor);
}

auto uptime_text(double seconds) -> std::string {
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  const long double bounded =
      std::isfinite(seconds)
          ? std::clamp(static_cast<long double>(seconds), 0.0L,
                       static_cast<long double>(maximum))
          : 0.0L;
  const auto whole = static_cast<std::uint64_t>(bounded);
  const auto days = whole / 86400;
  const auto hours = whole % 86400 / 3600;
  const auto minutes = whole % 3600 / 60;
  if (days > 0) return std::format("{}d {:02}:{:02}", days, hours, minutes);
  return std::format("{:02}:{:02}", hours, minutes);
}

auto ascii_lower(std::string text) -> std::string {
  std::ranges::transform(text, text.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return text;
}

auto sort_name(ProcessSort sort) -> std::string_view {
  switch (sort) {
    case ProcessSort::Cpu: return "CPU";
    case ProcessSort::Memory: return "memory";
    case ProcessSort::Pid: return "PID";
    case ProcessSort::Time: return "TIME+";
    case ProcessSort::User: return "USER";
    case ProcessSort::State: return "S";
    case ProcessSort::Command: return "COMMAND";
  }
  return "CPU";
}

struct GridTrack {
  int origin{};
  int extent{};
};

// Split one axis into content tracks separated by one-cell gutters. Remainder
// cells go to the leading tracks so an odd-sized panel stays deterministic and
// the final track still ends exactly at the inner edge.
auto grid_tracks(int origin, int extent, int count) -> std::vector<GridTrack> {
  std::vector<GridTrack> tracks;
  if (extent <= 0 || count <= 0) return tracks;

  tracks.reserve(static_cast<std::size_t>(count));
  const int content = std::max(0, extent - (count - 1));
  const int base = content / count;
  const int remainder = content % count;
  int at = origin;
  for (int i = 0; i < count; ++i) {
    const int size = base + (i < remainder ? 1 : 0);
    tracks.push_back({at, size});
    at += size + 1;
  }
  return tracks;
}

struct CpuGrid {
  int columns{};
  int rows{};
  std::vector<Rect> tiles;
};

auto cpu_grid(Rect inner, int count) -> CpuGrid {
  CpuGrid result;
  if (inner.w <= 0 || inner.h <= 0 || count <= 0) return result;

  // A useful tile needs one label row plus one waveform row. Account for the
  // gutter while retaining the old 9-column compact and 18-column ideal
  // targets. Height is the hard constraint: add columns when that is what
  // keeps every graph two rows tall. Width may force a totalized degenerate
  // layout, which draw() handles by suppressing invalid tiles.
  const int max_rows = std::max(1, (inner.h + 1) / 3);
  const int needed_columns = (count + max_rows - 1) / max_rows;
  const int max_columns = std::clamp((inner.w + 1) / 2, 1, count);
  const int comfortable_columns =
      std::clamp((inner.w + 1) / 10, 1, max_columns);
  const int ideal_columns =
      std::clamp((inner.w + 1) / 19, 1, comfortable_columns);
  result.columns =
      std::min(max_columns, std::max(ideal_columns, needed_columns));
  result.rows = (count + result.columns - 1) / result.columns;

  const auto x_tracks = grid_tracks(inner.x, inner.w, result.columns);
  const auto y_tracks = grid_tracks(inner.y, inner.h, result.rows);
  result.tiles.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    const auto& x = x_tracks[static_cast<std::size_t>(i % result.columns)];
    const auto& y = y_tracks[static_cast<std::size_t>(i / result.columns)];
    result.tiles.push_back({x.origin, y.origin, x.extent, y.extent});
  }
  return result;
}

auto valid_cpu_tile(Rect tile) noexcept -> bool {
  return tile.w >= 1 && tile.h >= 2;
}

auto populated_columns(const CpuGrid& grid, int row) -> int {
  int result = 0;
  for (int col = 0; col < grid.columns; ++col) {
    const int index = row * grid.columns + col;
    if (index >= static_cast<int>(grid.tiles.size()) ||
        !valid_cpu_tile(grid.tiles[static_cast<std::size_t>(index)]))
      break;
    ++result;
  }
  return result;
}

auto draw_cpu_grid(Screen& screen, const CpuGrid& grid, BorderStyle style)
    -> void {
  const GridGlyphs glyphs = grid_glyphs(style);

  // Vertical separators exist only where two populated tiles are adjacent in
  // that row. A partial final row therefore has no trailing line advertising
  // a tile that does not exist.
  for (int row = 0; row < grid.rows; ++row) {
    const int populated = populated_columns(grid, row);
    for (int col = 0; col + 1 < populated; ++col) {
      const Rect left =
          grid.tiles[static_cast<std::size_t>(row * grid.columns + col)];
      const int x = left.x + left.w;
      for (int y = left.y; y < left.y + left.h; ++y)
        screen.write_text(x, y, glyphs.vertical, theme::kDim, theme::kBg);
    }
  }

  // A row divider spans only the columns populated on both sides. At a
  // partial final row it ends with that row's last real tile instead of
  // framing empty space. Vertical lines from the row above terminate through
  // the gutter; lines that continue below become junctions.
  for (int row = 0; row + 1 < grid.rows; ++row) {
    const int upper = populated_columns(grid, row);
    const int lower = populated_columns(grid, row + 1);
    if (upper == 0 || lower == 0) continue;

    const Rect upper_first =
        grid.tiles[static_cast<std::size_t>(row * grid.columns)];
    const Rect upper_last = grid.tiles[static_cast<std::size_t>(
        row * grid.columns + std::min(upper, lower) - 1)];
    const int y = upper_first.y + upper_first.h;
    const int x_end = upper_last.x + upper_last.w;
    for (int x = upper_first.x; x < x_end; ++x)
      screen.write_text(x, y, glyphs.horizontal, theme::kDim, theme::kBg);

    for (int col = 0; col + 1 < upper; ++col) {
      const Rect left =
          grid.tiles[static_cast<std::size_t>(row * grid.columns + col)];
      const int x = left.x + left.w;
      screen.write_text(x, y,
                        col + 1 < lower ? glyphs.junction : glyphs.vertical,
                        theme::kDim, theme::kBg);
    }
  }
}

} // namespace

auto format_cpu_time(double seconds) -> std::string {
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  const long double rounded =
      std::isfinite(seconds)
          ? std::round(static_cast<long double>(seconds) * 100.0L)
          : 0.0L;
  const long double hundredths_value =
      std::clamp(rounded, 0.0L, static_cast<long double>(maximum));
  const auto hundredths = static_cast<std::uint64_t>(hundredths_value);
  const auto total_seconds = hundredths / 100;
  const auto minutes = total_seconds / 60;
  if (minutes < 1000) {
    return std::format("{}:{:02}.{:02}", minutes, total_seconds % 60,
                       hundredths % 100);
  }
  return std::format("{}:{:02}:{:02}", total_seconds / 3600,
                     total_seconds % 3600 / 60, total_seconds % 60);
}

OverviewPanel::OverviewPanel() {
  m_frame.set_style(BorderStyle::Rounded);
}

auto OverviewPanel::set_style(BorderStyle style) -> void {
  m_frame.set_style(style);
}

auto OverviewPanel::set_snapshot(double uptime, std::array<double, 3> load,
                                 TaskCounts tasks) -> void {
  m_uptime = uptime;
  m_load = load;
  m_tasks = tasks;
  mark_dirty();
}

auto OverviewPanel::draw(Screen& screen) -> void {
  m_frame.set_geometry(rect());
  m_frame.draw(screen);
  const Rect inner = m_frame.content_rect();
  if (inner.h > 0) {
    screen.write_text(inner.x, inner.y,
                      std::format("up {} · load {:.2f} {:.2f} {:.2f}",
                                  uptime_text(m_uptime), m_load[0], m_load[1],
                                  m_load[2]),
                      theme::kFg, theme::kBg);
  }
  if (inner.h > 1) {
    screen.write_text(
        inner.x, inner.y + 1,
        std::format("Tasks {} total · {} running · {} sleeping · {} stopped · "
                    "{} zombie",
                    m_tasks.total, m_tasks.running, m_tasks.sleeping,
                    m_tasks.stopped, m_tasks.zombie),
        theme::kDim, theme::kBg);
  }
  clear_dirty();
}

CpuPanel::CpuPanel() : m_aggregate_wave(std::make_unique<WaveformWidget>(120)) {
  m_frame.set_style(BorderStyle::Rounded);
  m_aggregate_wave->set_range(0.0F, 1.0F);
}

auto CpuPanel::set_style(BorderStyle style) -> void {
  m_frame.set_style(style);
}

auto CpuPanel::set_samples(std::span<const CpuSample> samples) -> void {
  while (m_waves.size() < samples.size()) {
    auto wave = std::make_unique<WaveformWidget>(120);
    wave->set_range(0.0F, 1.0F);
    m_waves.push_back(std::move(wave));
  }
  if (m_waves.size() > samples.size()) m_waves.resize(samples.size());
  m_names.resize(samples.size());
  m_usage.resize(samples.size());
  for (std::size_t i = 0; i < samples.size(); ++i) {
    m_names[i] = samples[i].name;
    m_usage[i] = samples[i].usage;
    m_waves[i]->push(samples[i].usage);
  }
  mark_dirty();
}

auto CpuPanel::set_aggregate_sample(const CpuSample& sample) -> void {
  m_aggregate_name = sample.name;
  m_aggregate_usage = sample.usage;
  m_aggregate_wave->push(sample.usage);
  mark_dirty();
}

auto CpuPanel::set_per_cpu(bool per_cpu) -> void {
  if (m_per_cpu == per_cpu) return;
  m_per_cpu = per_cpu;
  m_active_invalidated.assign(per_cpu ? m_waves.size() : 1, true);
  m_frame.set_title(per_cpu ? "CPU cores" : "CPU aggregate");
  mark_dirty();
}

auto CpuPanel::active_waves() const -> std::vector<WaveformWidget*> {
  if (!m_per_cpu && m_aggregate_wave->sample_count() > 0)
    return {m_aggregate_wave.get()};
  if (!m_per_cpu) return {};
  std::vector<WaveformWidget*> waves;
  waves.reserve(m_waves.size());
  for (const auto& wave : m_waves)
    waves.push_back(wave.get());
  return waves;
}

auto CpuPanel::draw(Screen& screen) -> void {
  m_frame.set_geometry(rect());
  m_frame.draw(screen);
  const Rect inner = m_frame.content_rect();
  const auto waves = active_waves();
  if (inner.w > 0 && inner.h > 0)
    screen.fill_rect(inner.x, inner.y, inner.w, inner.h, theme::kFg,
                     theme::kBg);
  if (inner.w <= 0 || inner.h <= 0 || waves.empty()) {
    for (auto* wave : waves)
      wave->set_geometry({0, 0, 0, 0});
    clear_dirty();
    return;
  }

  const int count = static_cast<int>(waves.size());
  const bool divided = m_per_cpu && count > 1;
  CpuGrid grid;
  if (divided) {
    grid = cpu_grid(inner, count);
    draw_cpu_grid(screen, grid, m_frame.style());
  } else {
    grid = {1, 1, {inner}};
  }

  for (int i = 0; i < count; ++i) {
    const Rect tile = grid.tiles[static_cast<std::size_t>(i)];
    if (!valid_cpu_tile(tile)) {
      waves[static_cast<std::size_t>(i)]->set_geometry({0, 0, 0, 0});
      continue;
    }
    const bool compact_label = tile.w < 10;
    std::string name =
        m_per_cpu ? m_names[static_cast<std::size_t>(i)] : m_aggregate_name;
    if (compact_label && name.starts_with("cpu")) name = "c" + name.substr(3);
    std::string label = std::format(
        "{} {:3.0f}%", name,
        (m_per_cpu ? m_usage[static_cast<std::size_t>(i)] : m_aggregate_usage) *
            100.0F);
    if (label.size() > static_cast<std::size_t>(tile.w))
      label.resize(static_cast<std::size_t>(tile.w));
    screen.write_text(tile.x, tile.y, label, theme::kDim, theme::kBg);
    auto* wave = waves[static_cast<std::size_t>(i)];
    wave->set_geometry({tile.x, tile.y + 1, tile.w, tile.h - 1});
    wave->draw(screen);
  }
  clear_dirty();
}

auto CpuPanel::pixel_regions() -> std::vector<Rect> {
  std::vector<Rect> regions;
  const auto waves = active_waves();
  regions.reserve(waves.size());
  for (const auto* wave : waves) {
    const Rect r = wave->rect();
    if (r.w > 0 && r.h > 0) regions.push_back(r);
  }
  return regions;
}

auto CpuPanel::draw_pixels(Rect region, Extent preferred) -> const Image* {
  for (auto* wave : active_waves())
    if (wave->rect() == region) return wave->draw_pixels(region, preferred);
  return nullptr;
}

auto CpuPanel::pixel_region_state(Rect region) const noexcept
    -> PixelRegionState {
  const auto waves = active_waves();
  for (std::size_t i = 0; i < waves.size(); ++i)
    if (waves[i]->rect() == region) {
      auto state = waves[i]->pixel_region_state(region);
      if (i < m_active_invalidated.size() && m_active_invalidated[i])
        state.content_dirty = true;
      return state;
    }
  return {};
}

auto CpuPanel::pixel_region_submitted(Rect region) noexcept -> void {
  const auto waves = active_waves();
  for (std::size_t i = 0; i < waves.size(); ++i)
    if (waves[i]->rect() == region) {
      waves[i]->pixel_region_submitted(region);
      if (i < m_active_invalidated.size()) m_active_invalidated[i] = false;
      return;
    }
}

MemoryPanel::MemoryPanel() {
  m_frame.set_style(BorderStyle::Rounded);
  m_memory.set_colors(Rgb{0x00, 0xD4, 0xFF}, Rgb{0x20, 0x28, 0x38}, theme::kFg);
  m_swap.set_colors(Rgb{0xC0, 0x80, 0xFF}, Rgb{0x20, 0x28, 0x38}, theme::kFg);
}

auto MemoryPanel::set_style(BorderStyle style) -> void {
  m_frame.set_style(style);
}

auto MemoryPanel::set_memory(const MemoryInfo& memory) -> void {
  const auto memory_used =
      memory.total_bytes - std::min(memory.total_bytes, memory.available_bytes);
  const auto swap_used =
      memory.swap_total_bytes -
      std::min(memory.swap_total_bytes, memory.swap_free_bytes);
  m_memory.set_value(used_fraction(memory.total_bytes, memory.available_bytes));
  const auto memory_scale = byte_scale(memory.total_bytes);
  m_memory.set_label(std::format(
      "RAM {} used / {} total · {} available {}",
      scaled_bytes(memory_used, memory_scale),
      scaled_bytes(memory.total_bytes, memory_scale),
      scaled_bytes(memory.available_bytes, memory_scale), memory_scale.unit));
  m_swap.set_value(
      used_fraction(memory.swap_total_bytes, memory.swap_free_bytes));
  m_swap.set_label(memory.swap_total_bytes == 0 ? "Swap disabled" : [&] {
    const auto scale = byte_scale(memory.swap_total_bytes);
    return std::format("Swap {} used / {} total · {} free {}",
                       scaled_bytes(swap_used, scale),
                       scaled_bytes(memory.swap_total_bytes, scale),
                       scaled_bytes(memory.swap_free_bytes, scale), scale.unit);
  }());
  mark_dirty();
}

auto MemoryPanel::draw(Screen& screen) -> void {
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
  m_filter.on_change([this](const std::string&) { rebuild(); });
  update_columns(80);
  m_table.on_select([this](int row, const std::vector<std::string>&) {
    if (row < 0 || row >= static_cast<int>(m_visible.size())) return;
    auto callback = m_on_activate;
    if (callback) callback(m_visible[static_cast<std::size_t>(row)]);
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

auto ProcessPanel::set_sort(ProcessSort key) -> void {
  m_sort = key;
  m_descending = key == ProcessSort::Cpu || key == ProcessSort::Memory ||
                 key == ProcessSort::Time;
  rebuild();
}

auto ProcessPanel::reverse_sort() -> void {
  m_descending = !m_descending;
  rebuild();
}

auto ProcessPanel::set_command_line(bool enabled) -> void {
  if (m_command_line == enabled) return;
  m_command_line = enabled;
  rebuild();
}

auto ProcessPanel::on_activate(std::function<void(const ProcessRow&)> callback)
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
  if (callback) callback(m_visible[static_cast<std::size_t>(selected)]);
  return true;
}

auto ProcessPanel::handle_header_click(const MouseEvent& mouse) -> bool {
  const Rect r = m_table.rect();
  if (!mouse.pressed || mouse.button != 0 || mouse.y != r.y ||
      !r.contains(mouse.x, mouse.y))
    return false;
  int x = mouse.x - r.x - m_table.gutter_cols();
  if (x < 0) return true;
  const auto width = [](ProcessColumn column) {
    switch (column) {
      case ProcessColumn::Pid:
      case ProcessColumn::Cpu:
      case ProcessColumn::Memory: return 5;
      case ProcessColumn::User:
      case ProcessColumn::Time:
      case ProcessColumn::Resident: return 9;
      case ProcessColumn::State: return 1;
      case ProcessColumn::Command: return 0;
    }
    return 0;
  };
  const auto sort_for = [](ProcessColumn column) {
    switch (column) {
      case ProcessColumn::Pid: return ProcessSort::Pid;
      case ProcessColumn::User: return ProcessSort::User;
      case ProcessColumn::State: return ProcessSort::State;
      case ProcessColumn::Cpu: return ProcessSort::Cpu;
      case ProcessColumn::Memory:
      case ProcessColumn::Resident: return ProcessSort::Memory;
      case ProcessColumn::Time: return ProcessSort::Time;
      case ProcessColumn::Command: return ProcessSort::Command;
    }
    return ProcessSort::Cpu;
  };
  for (const auto column : m_columns) {
    const int span = width(column) > 0 ? width(column) : std::max(1, r.w - x);
    if (x < span) {
      set_sort(sort_for(column));
      return true;
    }
    x -= span;
    if (x == 0) return true;
    --x; // one-column inter-column gap
  }
  return true;
}

auto ProcessPanel::update_columns(int width) -> void {
  if (m_table_width == width) return;
  m_table_width = width;
  const int usable = std::max(0, width - m_table.gutter_cols());
  int needed = 5 + 5 + 5 + 8 + 3; // required widths, command floor, gaps
  bool user = false, state = false, time = false, resident = false;
  const auto add = [&](int cost, bool& enabled) {
    if (usable >= needed + cost) {
      enabled = true;
      needed += cost;
    }
  };
  add(10, user);
  add(2, state);
  add(10, time);
  add(10, resident);

  m_columns.clear();
  m_columns.push_back(ProcessColumn::Pid);
  if (user) m_columns.push_back(ProcessColumn::User);
  if (state) m_columns.push_back(ProcessColumn::State);
  m_columns.push_back(ProcessColumn::Cpu);
  m_columns.push_back(ProcessColumn::Memory);
  if (time) m_columns.push_back(ProcessColumn::Time);
  if (resident) m_columns.push_back(ProcessColumn::Resident);
  m_columns.push_back(ProcessColumn::Command);

  std::vector<Column> columns;
  columns.reserve(m_columns.size());
  for (const auto column : m_columns) {
    switch (column) {
      case ProcessColumn::Pid:
        columns.push_back({"PID", Align::Right, 5});
        break;
      case ProcessColumn::User:
        columns.push_back({"USER", Align::Left, 9});
        break;
      case ProcessColumn::State:
        columns.push_back({"S", Align::Center, 1});
        break;
      case ProcessColumn::Cpu:
        columns.push_back({"%CPU", Align::Right, 5});
        break;
      case ProcessColumn::Memory:
        columns.push_back({"%MEM", Align::Right, 5});
        break;
      case ProcessColumn::Time:
        columns.push_back({"TIME+", Align::Right, 9});
        break;
      case ProcessColumn::Resident:
        columns.push_back({"RES", Align::Right, 9});
        break;
      case ProcessColumn::Command:
        columns.push_back({"COMMAND", Align::Left, 0});
        break;
    }
  }
  m_table.set_columns(std::move(columns));
  rebuild();
}

auto ProcessPanel::rebuild() -> void {
  const auto keep = selected_pid();
  const std::string needle = ascii_lower(m_filter.text());
  m_visible.clear();
  for (const auto& process : m_processes) {
    const std::string haystack = ascii_lower(
        std::format("{} {} {} {}", process.pid, process.user, process.name,
                    m_command_line ? process.command : process.name));
    if (needle.empty() || haystack.find(needle) != std::string::npos)
      m_visible.push_back(process);
  }

  const auto less = [this](const ProcessRow& lhs, const ProcessRow& rhs) {
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
      case ProcessSort::Time:
        order = lhs.cpu_seconds < rhs.cpu_seconds
                    ? -1
                    : (lhs.cpu_seconds > rhs.cpu_seconds ? 1 : 0);
        break;
      case ProcessSort::User:
        order = lhs.user < rhs.user ? -1 : (lhs.user > rhs.user ? 1 : 0);
        break;
      case ProcessSort::State:
        order = lhs.state < rhs.state ? -1 : (lhs.state > rhs.state ? 1 : 0);
        break;
      case ProcessSort::Command: {
        const auto& left = m_command_line ? lhs.command : lhs.name;
        const auto& right = m_command_line ? rhs.command : rhs.name;
        order = left < right ? -1 : (left > right ? 1 : 0);
        break;
      }
    }
    if (order == 0 && lhs.pid != rhs.pid) order = lhs.pid < rhs.pid ? -1 : 1;
    return m_descending ? order > 0 : order < 0;
  };
  std::ranges::sort(m_visible, less);

  m_table.clear_rows();
  for (const auto& process : m_visible) {
    std::vector<std::string> row;
    row.reserve(m_columns.size());
    for (const auto column : m_columns) {
      switch (column) {
        case ProcessColumn::Pid:
          row.push_back(std::format("{}", process.pid));
          break;
        case ProcessColumn::User:
          row.push_back(Screen::sanitize(process.user));
          break;
        case ProcessColumn::State:
          row.push_back(std::string(1, process.state));
          break;
        case ProcessColumn::Cpu:
          row.push_back(std::format("{:.1f}", process.cpu_percent));
          break;
        case ProcessColumn::Memory:
          row.push_back(std::format("{:.1f}", process.memory_percent));
          break;
        case ProcessColumn::Time:
          row.push_back(format_cpu_time(process.cpu_seconds));
          break;
        case ProcessColumn::Resident:
          row.push_back(human_bytes(process.rss_bytes));
          break;
        case ProcessColumn::Command:
          row.push_back(Screen::sanitize(m_command_line ? process.command
                                                        : process.name));
          break;
      }
    }
    m_table.add_row(std::move(row));
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

auto ProcessPanel::draw(Screen& screen) -> void {
  m_frame.set_geometry(rect());
  m_frame.draw(screen);
  const Rect inner = m_frame.content_rect();
  m_filter.set_geometry({inner.x, inner.y, inner.w, inner.h > 0 ? 1 : 0});
  m_table.set_geometry(
      {inner.x, inner.y + 1, inner.w, std::max(0, inner.h - 1)});
  update_columns(inner.w);
  m_filter.draw(screen);
  m_table.draw(screen);
  clear_dirty();
}

DetailPopup::DetailPopup() : Dialog{"Process detail"} {
  set_max_width(56);
  set_border_style(BorderStyle::Rounded);
}

auto DetailPopup::on_event(const Event& event) -> bool {
  if (const auto* key = std::get_if<KeyEvent>(&event);
      key && key->action == KeyAction::Press && key->key == Key::Char &&
      key->ch == U'q' && !key->ctrl && !key->alt) {
    close();
    return true;
  }
  return Dialog::on_event(event);
}

auto DetailPopup::set_process(const ProcessRow& process,
                              std::span<const float> history) -> void {
  m_pid = process.pid;
  set_title(
      std::format("{} ({})", Screen::sanitize(process.name), process.pid));
  set_text(std::format("CPU {:.1f}%   RSS {}", process.cpu_percent,
                       human_bytes(process.rss_bytes)));
  rasterize(history);
}

auto DetailPopup::rasterize(std::span<const float> history) -> void {
  Image& image = m_graph.image();
  image.fill({0, 0, image.width(), image.height()},
             Pixel{0x0B, 0x10, 0x1C, 0xFF});
  if (history.empty()) return;
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

auto DetailPopup::draw_content(Screen& screen) -> void {
  m_graph.draw(screen);
}

auto DetailPopup::pixel_regions() -> std::vector<Rect> {
  const Rect region = m_graph.rect();
  return region.w > 0 && region.h > 0 ? std::vector<Rect>{region}
                                      : std::vector<Rect>{};
}

auto DetailPopup::draw_pixels(Rect region, Extent preferred) -> const Image* {
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
