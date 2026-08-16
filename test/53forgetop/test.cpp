// forge-top is the flagship binary rather than part of termforge::lib. This
// suite compiles its private components and drives the real App frame body so
// the fake-data and forced-tier promises are observable without a live TTY.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

#include "cli.hpp"
#include "forge_top.hpp"
#include "panels.hpp"
#include "proc_reader.hpp"
#include "support/apc.hpp"
#include "support/screen.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"

using namespace termforge;
using namespace termforge::forge_top;

namespace {

struct TempProc {
  TempProc() {
    std::array<char, 32> pattern{};
    const std::string base = "/tmp/termforge-proc-XXXXXX";
    std::ranges::copy(base, pattern.begin());
    const char *made = ::mkdtemp(pattern.data());
    REQUIRE(made != nullptr);
    root = made;
  }
  ~TempProc() { std::filesystem::remove_all(root); }

  auto write(std::string_view relative, std::string_view contents) -> void {
    const auto path = root / relative;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out{path};
    REQUIRE(out.good());
    out << contents;
  }

  auto write_binary(std::string_view relative, std::string_view contents)
      -> void {
    const auto path = root / relative;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out{path, std::ios::binary};
    REQUIRE(out.good());
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  }

  std::filesystem::path root;
};

auto process_stat(int pid, std::string_view name, int utime, int stime,
                  int rss_pages, char state = 'S') -> std::string {
  return std::to_string(pid) + " (" + std::string{name} +
         ") " + state + " 1 1 1 0 0 0 0 0 0 0 " + std::to_string(utime) + " " +
         std::to_string(stime) + " 0 0 20 0 1 0 10 4096 " +
         std::to_string(rss_pages) + "\n";
}

auto process_row(int pid, std::string name, float cpu, std::uint64_t rss,
                 std::string user = "user", char state = 'S',
                 float memory = 1.0F, double time = 1.0,
                 std::string command = {}) -> ProcessRow {
  if (command.empty())
    command = name;
  return {pid, std::move(name), cpu, rss, std::move(user), state, memory, time,
          std::move(command)};
}

auto screen_row(const Screen &screen, int y) -> std::string {
  std::string text;
  for (int x = 0; x < screen.cols(); ++x)
    text += screen.at(x, y).text;
  return text;
}

class CountingReader final : public SystemReader {
public:
  auto sample() -> std::expected<SystemSnapshot, ErrorEvent> override {
    ++calls;
    SystemSnapshot snapshot;
    snapshot.aggregate_cpu = {"cpu", 0.5F};
    snapshot.cpus = {{"cpu0", 0.5F}};
    snapshot.memory.total_bytes = 1024;
    snapshot.memory.available_bytes = 512;
    snapshot.processes = {process_row(7, "counter", 2.0F, 64)};
    snapshot.tasks = {1, 1, 0, 0, 0};
    return snapshot;
  }

  int calls{};
};

auto count(std::string_view text, std::string_view needle) -> int {
  int result = 0;
  for (std::size_t at = 0;
       (at = text.find(needle, at)) != std::string_view::npos;
       at += needle.size())
    ++result;
  return result;
}

auto action_count(std::string_view wire, std::string_view action) -> int {
  int result = 0;
  for (const auto &apc : tfsupport::apcs(wire))
    // Data-operation openers name i=. Since #259 every animation-frame
    // continuation repeats a=f, so counting action keys alone makes one update
    // look like one update per 4,096 encoded bytes.
    if (tfsupport::key_value(apc, "a") == action &&
        tfsupport::has_key(apc, "i"))
      ++result;
  return result;
}

auto transmit_extents(std::string_view wire)
    -> std::vector<std::pair<std::string, std::string>> {
  std::vector<std::pair<std::string, std::string>> result;
  for (const auto &apc : tfsupport::apcs(wire))
    if (tfsupport::key_value(apc, "a") == "t")
      result.emplace_back(tfsupport::key_value(apc, "s"),
                          tfsupport::key_value(apc, "v"));
  return result;
}

auto cpu_samples(int sample_count) -> std::vector<CpuSample> {
  std::vector<CpuSample> samples;
  samples.reserve(static_cast<std::size_t>(sample_count));
  for (int i = 0; i < sample_count; ++i)
    samples.push_back({std::format("cpu{}", i),
                       static_cast<float>(i + 1) /
                           static_cast<float>(sample_count + 1)});
  return samples;
}

auto contains(Rect rect, int x, int y) noexcept -> bool {
  return x >= rect.x && x < rect.x + rect.w && y >= rect.y &&
         y < rect.y + rect.h;
}

class SegmentSink final : public ByteSink {
public:
  auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override {
    attempts.emplace_back(bytes.begin(), bytes.end());
    if (fail_write > 0 &&
        attempts.size() == static_cast<std::size_t>(fail_write)) {
      return std::unexpected{
          ErrorEvent{Severity::Warning, "sink", "waveform frame refused"}};
    }
    accepted.insert(accepted.end(), bytes.begin(), bytes.end());
    return {};
  }

  int fail_write{0};
  std::vector<std::string> attempts;
  std::string accepted;
};

class CpuPanelApp final : public App {
public:
  CpuPanelApp() {
    samples.reserve(20);
    for (int i = 0; i < 20; ++i) {
      samples.push_back(
          CpuSample{std::format("cpu{}", i), static_cast<float>(i + 1) / 24});
    }
    panel.set_samples(samples);
  }

  auto on_render(Screen &screen) -> void override {
    driver().set_output(&sink);
    for (const int update : update_frames)
      if (update == frame) {
        for (auto &sample : samples)
          sample.usage = sample.usage > 0.75F ? sample.usage - 0.4F
                                             : sample.usage + 0.2F;
        panel.set_samples(samples);
      }
    for (const auto &[at, size] : resize_frames)
      if (at == frame)
        resize_results.push_back(set_size(size).has_value());

    screen.clear();
    panel.set_geometry({0, 0, screen.cols(), screen.rows()});
    panel.draw(screen);
    render_pixel_regions(panel);
    ++frame;
  }

  auto run(std::unique_ptr<TerminalDriver> selected, int frames) -> void {
    test_run_frames(frames, 120, 40, nullptr, std::move(selected));
  }

  [[nodiscard]] auto all_regions_clean() -> bool {
    const auto regions = panel.pixel_regions();
    return regions.size() == 20 &&
           std::ranges::all_of(regions, [&](Rect region) {
             const auto state = panel.pixel_region_state(region);
             return state.mode == PixelRegionMode::Persistent &&
                    !state.content_dirty;
           });
  }

  CpuPanel panel;
  SegmentSink sink;
  std::vector<CpuSample> samples;
  std::vector<int> update_frames;
  std::vector<std::pair<int, App::Size>> resize_frames;
  std::vector<bool> resize_results;
  std::vector<FrameBytes> observed_frames;

protected:
  [[nodiscard]] auto now_steady() const
      -> std::chrono::steady_clock::time_point override {
    return now;
  }
  auto wait_readable(int timeout_ms) -> bool override {
    observed_frames.push_back(driver().last_frame_bytes());
    now += std::chrono::milliseconds(timeout_ms);
    return false;
  }
  auto read_available(char *, int) -> int override { return 0; }

private:
  int frame{0};
  std::chrono::steady_clock::time_point now{};
};

} // namespace

TEST_CASE("forge-top CLI selects fake data and one forced tier",
          "[forge-top]") {
  REQUIRE(std::string_view{usage()}.starts_with("Usage: forge-top "));

  std::array<char, 1> a0{'x'};
  std::array<char, 7> a1{'-', '-', 'f', 'a', 'k', 'e', '\0'};
  std::array<char, 15> a2{'-', '-', 'd', 'r', 'i', 'v', 'e', 'r',
                          '=', 'k', 'i', 't', 't', 'y', '\0'};
  char *argv[]{a0.data(), a1.data(), a2.data()};
  const auto parsed = parse_options(3, argv);
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->fake);
  REQUIRE(parsed->driver == DriverChoice::Kitty);

  std::array<char, 13> bad{'-', '-', 'd', 'r', 'i', 'v', 'e',
                           'r', '=', 's', 'v', 'g', '\0'};
  char *bad_argv[]{a0.data(), bad.data()};
  REQUIRE_FALSE(parse_options(2, bad_argv).has_value());

  std::array<char, 14> ansi{'-', '-', 'd', 'r', 'i', 'v', 'e',
                            'r', '=', 'a', 'n', 's', 'i', '\0'};
  char *conflicting[]{a0.data(), a2.data(), ansi.data()};
  REQUIRE_FALSE(parse_options(3, conflicting).has_value());
}

TEST_CASE("forge-top fake reader is deterministic and exercises 20 regions",
          "[forge-top]") {
  auto reader = make_fake_reader();
  const auto first = reader->sample();
  const auto second = reader->sample();
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  REQUIRE(first->cpus.size() == 20);
  REQUIRE(first->processes.size() == 48);
  REQUIRE(first->aggregate_cpu.name == "cpu");
  REQUIRE(first->uptime_seconds == 3.0 * 24.0 * 60.0 * 60.0);
  REQUIRE(first->tasks.total == 48);
  REQUIRE(first->tasks.running > 0);
  REQUIRE(first->tasks.zombie > 0);
  REQUIRE_FALSE(first->processes.front().user.empty());
  REQUIRE_FALSE(first->processes.front().command.empty());
  REQUIRE(first->processes.front().pid == second->processes.front().pid);
  REQUIRE(first->cpus.front().usage != second->cpus.front().usage);
  REQUIRE(first->aggregate_cpu.usage != second->aggregate_cpu.usage);
}

TEST_CASE("forge-top /proc reader computes deltas and tolerates a vanished PID",
          "[forge-top][failure]") {
  TempProc proc;
  proc.write("stat", "cpu 100 0 100 800 0 0 0 0\n"
                     "cpu0 50 0 50 400 0 0 0 0\n"
                     "cpu1 50 0 50 400 0 0 0 0\n");
  proc.write("meminfo", "MemTotal: 1000 kB\nMemAvailable: 600 kB\n"
                        "SwapTotal: 200 kB\nSwapFree: 150 kB\n");
  proc.write("uptime", "12345.00 100.00\n");
  proc.write("loadavg", "1.25 0.75 0.50 1/10 101\n");
  proc.write("101/stat", process_stat(101, "name with spaces", 10, 5, 7));
  proc.write("101/status", "Name:\tname\nUid:\t0\t0\t0\t0\n");
  proc.write_binary("101/cmdline",
                    std::string_view{"tool\0--flag\0value\0", 18});
  proc.write("202/stat", process_stat(202, "short-lived", 2, 1, 3));

  auto reader = make_proc_reader(proc.root);
  const auto first = reader->sample();
  REQUIRE(first.has_value());
  REQUIRE(first->cpus.size() == 2);
  REQUIRE(first->processes.size() == 2);
  REQUIRE(std::ranges::all_of(first->processes, [](const ProcessRow &row) {
    return row.cpu_percent == 0.0F;
  }));
  REQUIRE(first->memory.total_bytes == 1000 * 1024);
  REQUIRE(first->uptime_seconds == 12345.0);
  REQUIRE(first->load_average[0] == 1.25);

  proc.write("stat", "cpu 150 0 150 900 0 0 0 0\n"
                     "cpu0 75 0 75 450 0 0 0 0\n"
                     "cpu1 75 0 75 450 0 0 0 0\n");
  proc.write("101/stat", process_stat(101, "name with spaces", 20, 5, 7));
  std::filesystem::remove(proc.root / "202/stat");
  const auto second = reader->sample();
  REQUIRE(second.has_value());
  REQUIRE(second->processes.size() == 1);
  REQUIRE(second->processes.front().pid == 101);
  REQUIRE(second->processes.front().name == "name with spaces");
  REQUIRE(second->processes.front().cpu_percent == 10.0F);
  REQUIRE(second->processes.front().state == 'S');
  REQUIRE(second->processes.front().user == "root");
  REQUIRE(second->processes.front().command == "tool --flag value");
  REQUIRE(second->processes.front().cpu_seconds == 0.25);
  REQUIRE(second->aggregate_cpu.usage == 0.5F);
}

TEST_CASE("forge-top /proc reader rejects malformed root data",
          "[forge-top][failure]") {
  TempProc proc;
  proc.write("stat", "cpu broken\n");
  proc.write("meminfo", "MemTotal: 1000 kB\n");
  auto reader = make_proc_reader(proc.root);
  const auto result = reader->sample();
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().severity == Severity::Warning);
  REQUIRE(result.error().source == "forge-top");
}

TEST_CASE("forge-top /proc reader pins top fields and ancillary failures",
          "[forge-top][failure]") {
  TempProc proc;
  proc.write("stat", "cpu 20 0 10 70 0 0 0 0\n"
                     "cpu0 20 0 10 70 0 0 0 0\n");
  proc.write("meminfo", "MemTotal: 0 kB\nMemAvailable: 0 kB\n"
                        "SwapTotal: 0 kB\nSwapFree: 0 kB\n");
  proc.write("uptime", "90061.50 0.0\n");
  proc.write("loadavg", "3.00 2.00 1.00 1/4 404\n");

  proc.write("101/stat", process_stat(101, "name (with) parens", 12, 3, 7, 'R'));
  proc.write("101/status",
             "Name:\tname\nUid:\t4294967294\t4294967294\t4294967294\t4294967294\n");
  const std::string controlled{"tool\0--bad\0\x1b[31mred\0", 21};
  proc.write_binary("101/cmdline", controlled);
  proc.write("202/stat", process_stat(202, "empty", 2, 1, 3, 'Z'));
  proc.write("202/cmdline", "");
  proc.write("303/stat", process_stat(303, "unreadable", 2, 1, 3, 'T'));
  proc.write("404/stat", process_stat(404, "disk sleep", 2, 1, 3, 'D'));

  auto reader = make_proc_reader(
      proc.root, ProcReaderConfig{.page_size = 4096, .clock_ticks = 100});
  const auto result = reader->sample();
  REQUIRE(result.has_value());
  REQUIRE(result->uptime_seconds == 90061.5);
  REQUIRE((result->load_average == std::array<double, 3>{3.0, 2.0, 1.0}));
  REQUIRE(result->tasks.total == 4);
  REQUIRE(result->tasks.running == 1);
  REQUIRE(result->tasks.sleeping == 1);
  REQUIRE(result->tasks.stopped == 1);
  REQUIRE(result->tasks.zombie == 1);

  const auto find_pid = [&](int pid) -> const ProcessRow & {
    const auto it = std::ranges::find(result->processes, pid, &ProcessRow::pid);
    REQUIRE(it != result->processes.end());
    return *it;
  };
  const auto &rich = find_pid(101);
  REQUIRE(rich.name == "name (with) parens");
  REQUIRE(rich.state == 'R');
  REQUIRE(rich.user == "4294967294");
  REQUIRE(rich.memory_percent == 0.0F);
  REQUIRE(rich.cpu_seconds == 0.15);
  REQUIRE(rich.command == "tool --bad \x1b[31mred");
  REQUIRE(find_pid(202).command == "[empty]");
  REQUIRE(find_pid(303).command == "[unreadable]");

  auto bad_ticks = make_proc_reader(
      proc.root, ProcReaderConfig{.page_size = 4096, .clock_ticks = 0});
  const auto failed = bad_ticks->sample();
  REQUIRE_FALSE(failed.has_value());
  REQUIRE(failed.error().severity == Severity::Warning);
  REQUIRE(failed.error().message.find("clock ticks") != std::string::npos);
}

TEST_CASE("forge-top TIME+ scales long-running processes", "[forge-top]") {
  REQUIRE(format_cpu_time(0.01) == "0:00.01");
  REQUIRE(format_cpu_time(3723.45) == "62:03.45");
  REQUIRE(format_cpu_time(60'000.0) == "16:40:00");
}

TEST_CASE("forge-top summary presents load tasks and one unit per memory line",
          "[forge-top]") {
  Screen screen{100, 8};
  OverviewPanel overview;
  overview.set_geometry({0, 0, 100, 4});
  overview.set_snapshot(90061.0, {3.0, 2.0, 1.0}, {9, 1, 5, 2, 1});
  overview.draw(screen);
  CHECK(screen_row(screen, 1).find("up 1d 01:01 · load 3.00 2.00 1.00") !=
        std::string::npos);
  CHECK(screen_row(screen, 2).find(
            "Tasks 9 total · 1 running · 5 sleeping · 2 stopped · 1 zombie") !=
        std::string::npos);

  MemoryPanel memory;
  memory.set_geometry({0, 4, 100, 4});
  memory.set_memory({8ULL * 1024 * 1024 * 1024, 3ULL * 1024 * 1024 * 1024,
                     2ULL * 1024 * 1024 * 1024, 1ULL * 1024 * 1024 * 1024});
  memory.draw(screen);
  CHECK(screen_row(screen, 5).find(
            "RAM 5.0 used / 8.0 total · 3.0 available GiB") !=
        std::string::npos);
  CHECK(screen_row(screen, 6).find(
            "Swap 1.0 used / 2.0 total · 1.0 free GiB") !=
        std::string::npos);
}

TEST_CASE("forge-top process view filters, sorts and preserves PID selection",
          "[forge-top]") {
  ProcessPanel panel;
  panel.set_geometry({0, 0, 80, 14});
  Screen screen{80, 14};
  panel.set_processes({process_row(10, "alpha", 4.0F, 400),
                       process_row(20, "beta", 80.0F, 200),
                       process_row(30, "gamma", 20.0F, 900)});
  panel.draw(screen);
  REQUIRE(panel.visible_rows().front().pid == 20); // CPU descending

  panel.table().set_selected(1); // PID 30
  panel.set_processes({process_row(30, "gamma", 21.0F, 900),
                       process_row(20, "beta", 79.0F, 200),
                       process_row(10, "alpha", 5.0F, 400)});
  REQUIRE(
      panel.visible_rows()[static_cast<std::size_t>(panel.table().selected())]
          .pid == 30);

  panel.set_filter("alp");
  REQUIRE(panel.visible_rows().size() == 1);
  REQUIRE(panel.visible_rows().front().pid == 10);
  panel.set_filter("");
  panel.set_sort(ProcessSort::Pid);
  REQUIRE(panel.visible_rows().front().pid == 10);
  panel.reverse_sort();
  REQUIRE(panel.visible_rows().front().pid == 30);

  bool activated = false;
  panel.on_activate(
      [&](const ProcessRow &process) { activated = process.pid == 30; });
  panel.table().set_selected(0);
  REQUIRE(panel.activate_selected());
  REQUIRE(activated);

  panel.draw(screen);
  const Rect table = panel.table().rect();
  REQUIRE(panel.handle_header_click(
      MouseEvent{table.x + panel.table().gutter_cols() + 1, table.y, 0, true}));
  REQUIRE(panel.sort_key() == ProcessSort::Pid);
  REQUIRE_FALSE(panel.descending());
}

TEST_CASE("forge-top process columns elide by priority and sanitize COMMAND",
          "[forge-top][failure]") {
  const auto row = process_row(42, "worker", 75.0F, 64 * 1024 * 1024,
                               "alice", 'R', 12.5F, 3723.45,
                               "worker --token \x1b[31munsafe");

  ProcessPanel wide;
  wide.set_geometry({0, 0, 120, 8});
  wide.set_processes({row});
  wide.set_command_line(true);
  Screen wide_screen{120, 8};
  wide.draw(wide_screen);
  const std::string wide_header = screen_row(wide_screen, 2);
  CHECK(wide_header.find("PID") != std::string::npos);
  CHECK(wide_header.find("USER") != std::string::npos);
  CHECK(wide_header.find("S") != std::string::npos);
  CHECK(wide_header.find("%CPU") != std::string::npos);
  CHECK(wide_header.find("%MEM") != std::string::npos);
  CHECK(wide_header.find("TIME+") != std::string::npos);
  CHECK(wide_header.find("RES") != std::string::npos);
  CHECK(wide_header.find("COMMAND") != std::string::npos);
  const std::string wide_data = screen_row(wide_screen, 3);
  CHECK(wide_data.find("worker --token unsafe") != std::string::npos);
  CHECK(wide_data.find("[31m") == std::string::npos);
  CHECK(wide_data.find('\x1b') == std::string::npos);

  ProcessPanel narrow;
  narrow.set_geometry({0, 0, 31, 8});
  narrow.set_processes({row});
  Screen narrow_screen{31, 8};
  narrow.draw(narrow_screen);
  const std::string narrow_header = screen_row(narrow_screen, 2);
  CHECK(narrow_header.find("PID") != std::string::npos);
  CHECK(narrow_header.find("%CPU") != std::string::npos);
  CHECK(narrow_header.find("%MEM") != std::string::npos);
  CHECK(narrow_header.find("COMMAND") != std::string::npos);
  CHECK(narrow_header.find("USER") == std::string::npos);
  CHECK(narrow_header.find("TIME+") == std::string::npos);
  CHECK(narrow_header.find("RES") == std::string::npos);
}

TEST_CASE("forge-top sort selection is deterministic and R alone reverses",
          "[forge-top]") {
  ProcessPanel panel;
  panel.set_processes({process_row(10, "a", 2.0F, 400, "u", 'S', 2.0F, 5.0),
                       process_row(20, "b", 1.0F, 200, "u", 'R', 1.0F, 9.0)});
  panel.set_sort(ProcessSort::Pid);
  REQUIRE_FALSE(panel.descending());
  panel.reverse_sort();
  REQUIRE(panel.descending());
  panel.set_sort(ProcessSort::Pid);
  REQUIRE_FALSE(panel.descending());
  panel.set_sort(ProcessSort::Time);
  REQUIRE(panel.descending());
  REQUIRE(panel.visible_rows().front().pid == 20);
}

TEST_CASE("forge-top CPU mode changes invalidate the replacement regions",
          "[forge-top][persistent]") {
  CpuPanel panel;
  const std::array samples{CpuSample{"cpu0", 0.25F},
                           CpuSample{"cpu1", 0.75F}};
  panel.set_samples(samples);
  panel.set_aggregate_sample({"cpu", 0.5F});
  panel.set_geometry({0, 0, 60, 10});
  Screen screen{60, 10};
  panel.draw(screen);

  const auto cores = panel.pixel_regions();
  REQUIRE(cores.size() == 2);
  for (const Rect region : cores)
    panel.pixel_region_submitted(region);
  REQUIRE(std::ranges::none_of(cores, [&](Rect region) {
    return panel.pixel_region_state(region).content_dirty;
  }));

  panel.set_per_cpu(false);
  panel.draw(screen);
  const auto aggregate = panel.pixel_regions();
  REQUIRE(aggregate.size() == 1);
  REQUIRE(panel.pixel_region_state(aggregate.front()).content_dirty);
  panel.pixel_region_submitted(aggregate.front());
  REQUIRE_FALSE(panel.pixel_region_state(aggregate.front()).content_dirty);

  panel.set_per_cpu(true);
  panel.draw(screen);
  const auto restored = panel.pixel_regions();
  REQUIRE(restored.size() == 2);
  CHECK(std::ranges::all_of(restored, [&](Rect region) {
    return panel.pixel_region_state(region).content_dirty;
  }));
}

TEST_CASE("forge-top CPU grid reserves dividers in odd partial layouts",
          "[forge-top][pixels][layout]") {
  CpuPanel panel;
  const auto samples = cpu_samples(5);
  panel.set_samples(samples);
  panel.set_geometry({0, 0, 32, 11});
  Screen screen{32, 11};
  panel.draw(screen);

  // Inner 30x9 content becomes two columns and three rows. One-cell gutters
  // are excluded from every waveform destination; odd remainders belong to
  // the leading tracks, so the exact row-major geometry is deterministic.
  const std::vector<Rect> expected{{1, 2, 15, 2}, {17, 2, 14, 2},
                                   {1, 6, 15, 1}, {17, 6, 14, 1},
                                   {1, 9, 15, 1}};
  const auto regions = panel.pixel_regions();
  REQUIRE(regions == expected);

  CHECK(screen.at(16, 1).text == "│");
  CHECK(screen.at(16, 4).text == "┼");
  CHECK(screen.at(16, 5).text == "│");
  CHECK(screen.at(15, 7).text == "─");
  CHECK(screen.at(16, 7).text == "│");
  CHECK(screen.at(17, 7).text.empty());
  CHECK(screen.at(16, 8).text.empty());
  CHECK(tfsupport::row_text(screen, 5, 17, 14).starts_with("cpu3  67%"));

  for (const Rect region : regions) {
    CHECK_FALSE(contains(region, 16, 1));
    CHECK_FALSE(contains(region, 16, 4));
    CHECK_FALSE(contains(region, 15, 7));
    CHECK_FALSE(contains(region, 16, 7));
  }
}

TEST_CASE("forge-top CPU grid follows the ASCII border style",
          "[forge-top][pixels][layout][fallback]") {
  CpuPanel panel;
  const auto samples = cpu_samples(5);
  panel.set_style(BorderStyle::Ascii);
  panel.set_samples(samples);
  panel.set_geometry({0, 0, 32, 11});
  Screen screen{32, 11};
  panel.draw(screen);

  CHECK(screen.at(16, 1).text == "|");
  CHECK(screen.at(16, 4).text == "+");
  CHECK(screen.at(15, 4).text == "-");
  CHECK(screen.at(16, 8).text.empty());
}

TEST_CASE("forge-top aggregate CPU keeps the full graph and clears dividers",
          "[forge-top][pixels][layout]") {
  CpuPanel panel;
  const auto samples = cpu_samples(5);
  panel.set_samples(samples);
  panel.set_aggregate_sample({"cpu", 0.5F});
  panel.set_geometry({0, 0, 32, 11});
  Screen screen{32, 11};

  panel.draw(screen);
  REQUIRE(screen.at(16, 4).text == "┼");
  panel.set_per_cpu(false);
  panel.draw(screen);

  REQUIRE(panel.pixel_regions() == std::vector<Rect>{{1, 2, 30, 8}});
  for (int y = 1; y < 10; ++y)
    for (int x = 1; x < 31; ++x) {
      CHECK(screen.at(x, y).text != "─");
      CHECK(screen.at(x, y).text != "│");
      CHECK(screen.at(x, y).text != "┼");
    }
}

TEST_CASE("forge-top detail graph acknowledges persistent content",
          "[forge-top]") {
  DetailPopup detail;
  const std::array<float, 4> history{5.0F, 40.0F, 90.0F, 20.0F};
  detail.set_process(process_row(42, "renderer", 20.0F, 4096), history);
  Screen screen{80, 24};
  detail.draw(screen);
  const auto regions = detail.pixel_regions();
  REQUIRE(regions.size() == 1);
  REQUIRE(detail.pixel_region_state(regions.front()).mode ==
          PixelRegionMode::Persistent);
  REQUIRE(detail.pixel_region_state(regions.front()).content_dirty);
  REQUIRE(detail.draw_pixels(regions.front(), {320, 128}) != nullptr);
  detail.pixel_region_submitted(regions.front());
  REQUIRE_FALSE(detail.pixel_region_state(regions.front()).content_dirty);
}

TEST_CASE("forge-top global keys defer to filter and open menu",
          "[forge-top][input]") {
  ForgeTopApp filter_app{make_fake_reader()};
  std::string filter_wire;
  filter_app.run_headless(1, 80, 20, &filter_wire, DriverChoice::Fallback);
  filter_app.test_pump(
      {"\t\tq?hPMNTRds1ltmc "}); // table -> menu -> filter, then type
  REQUIRE(filter_app.process_panel_for_test().sort_key() == ProcessSort::Cpu);
  REQUIRE(filter_app.process_panel_for_test().filter().text() ==
          "q?hPMNTRds1ltmc ");
  REQUIRE(filter_app.running());
  REQUIRE(filter_app.cpu_per_cpu_for_test());
  REQUIRE((filter_app.section_state_for_test() ==
           std::array<bool, 4>{true, true, true, true}));

  ForgeTopApp menu_app{make_fake_reader()};
  menu_app.test_pump({"\t\r"}); // focus menu and open its dropdown
  menu_app.test_pump({"q"});     // close menu, do not quit
  menu_app.test_pump({"N"});
  REQUIRE(menu_app.process_panel_for_test().sort_key() == ProcessSort::Pid);
}

TEST_CASE("forge-top top keys compose sections without losing selection",
          "[forge-top][input]") {
  ForgeTopApp app{make_fake_reader()};
  app.process_panel_for_test().table().set_selected(3);
  const int selected = app.process_panel_for_test().table().selected();
  app.test_pump({"ltm1c"});
  REQUIRE((app.section_state_for_test() ==
           std::array<bool, 4>{false, false, false, true}));
  REQUIRE_FALSE(app.cpu_per_cpu_for_test());
  REQUIRE(app.process_panel_for_test().command_line());
  REQUIRE(app.process_panel_for_test().table().selected() == selected);

  app.test_pump({"M"});
  REQUIRE(app.process_panel_for_test().sort_key() == ProcessSort::Memory);
  REQUIRE(app.process_panel_for_test().descending());
  app.test_pump({"R"});
  REQUIRE_FALSE(app.process_panel_for_test().descending());
  app.test_pump({"M"});
  REQUIRE(app.process_panel_for_test().descending());
  app.test_pump({"T"});
  REQUIRE(app.process_panel_for_test().sort_key() == ProcessSort::Time);
  app.test_pump({"P"});
  REQUIRE(app.process_panel_for_test().sort_key() == ProcessSort::Cpu);
  app.test_pump({"N"});
  REQUIRE(app.process_panel_for_test().sort_key() == ProcessSort::Pid);
}

TEST_CASE("forge-top sampling delay validates and controls sample cadence",
          "[forge-top][input][failure]") {
  auto reader = std::make_unique<CountingReader>();
  auto *counting = reader.get();
  ForgeTopApp app{std::move(reader)};
  REQUIRE(counting->calls == 1); // constructor's initial sample

  app.test_pump({"d"});
  app.test_pump({"\x7f", "0.25\r"});
  REQUIRE(app.sample_delay_for_test() == std::chrono::duration<double>{0.25});
  app.on_tick(std::chrono::duration<double>{0.24});
  REQUIRE(counting->calls == 1);
  app.on_tick(std::chrono::duration<double>{0.01});
  REQUIRE(counting->calls == 2);
  app.test_pump({" "});
  REQUIRE(counting->calls == 3);
  app.on_tick(std::chrono::duration<double>{0.24});
  REQUIRE(counting->calls == 3); // manual refresh reset the accumulator

  app.test_pump({"s"});
  app.test_pump({"\x7f\x7f\x7f\x7f-1\r"});
  REQUIRE(app.sample_delay_for_test() == std::chrono::duration<double>{0.25});

  auto zero_reader = std::make_unique<CountingReader>();
  auto *zero_counting = zero_reader.get();
  ForgeTopApp zero_app{std::move(zero_reader)};
  zero_app.test_pump({"d"});
  zero_app.test_pump({"\x7f" "0\r"});
  REQUIRE(zero_app.sample_delay_for_test() ==
          std::chrono::duration<double>{0.0});
  zero_app.on_tick(std::chrono::duration<double>::zero());
  REQUIRE(zero_counting->calls == 2); // zero means once per rendered frame
}

TEST_CASE("forge-top popups close before q quits and global releases do nothing",
          "[forge-top][input][failure]") {
  ForgeTopApp help{make_fake_reader()};
  std::string wire;
  help.run_headless(1, 100, 28, &wire, DriverChoice::Fallback);
  REQUIRE(help.running());
  help.test_pump({"h"});
  help.test_pump({"q"});
  REQUIRE(help.running());
  help.test_pump({"q"});
  REQUIRE_FALSE(help.running());

  ForgeTopApp question_help{make_fake_reader()};
  std::string question_wire;
  question_help.run_headless(1, 100, 28, &question_wire,
                             DriverChoice::Fallback);
  question_help.test_pump({"?"});
  question_help.test_pump({"q"});
  REQUIRE(question_help.running());

  ForgeTopApp f1_help{make_fake_reader()};
  std::string f1_wire;
  f1_help.run_headless(1, 100, 28, &f1_wire, DriverChoice::Fallback);
  f1_help.on_event(KeyEvent{Key::F1});
  f1_help.test_pump({"q"});
  REQUIRE(f1_help.running());

  ForgeTopApp detail{make_fake_reader()};
  std::string detail_wire;
  detail.run_headless(1, 100, 28, &detail_wire, DriverChoice::Fallback);
  REQUIRE(detail.show_first_process_for_test());
  detail.test_pump({"q"});
  REQUIRE(detail.running());
  detail.test_pump({"q"});
  REQUIRE_FALSE(detail.running());

  ForgeTopApp actions{make_fake_reader()};
  std::string actions_wire;
  actions.run_headless(1, 80, 20, &actions_wire, DriverChoice::Fallback);
  actions.on_event(KeyEvent{Key::Char, U'q', false, false, false,
                            KeyAction::Repeat});
  actions.on_event(KeyEvent{Key::Char, U'q', false, false, false,
                            KeyAction::Release});
  REQUIRE(actions.running());
  actions.on_event(KeyEvent{Key::Char, U'q'});
  REQUIRE_FALSE(actions.running());
}

TEST_CASE("forge-top runs the real frame shape on every forced tier",
          "[forge-top][drivers]") {
  for (const DriverChoice choice :
       {DriverChoice::Fallback, DriverChoice::AnsiRgb, DriverChoice::Kitty}) {
    ForgeTopApp app{make_fake_reader()};
    std::string wire;
    app.run_headless(2, 120, 40, &wire, choice);
    REQUIRE_FALSE(wire.empty());
    if (choice == DriverChoice::Fallback) {
      REQUIRE(wire.find("\033_G") == std::string::npos);
    } else if (choice == DriverChoice::AnsiRgb) {
      REQUIRE(wire.find("\033[38;2;") != std::string::npos);
      REQUIRE(wire.find("\033_G") == std::string::npos);
    } else {
      REQUIRE(count(wire, "a=t") >= 20);
    }
  }
}

TEST_CASE(
    "forge-top detail uses one persistent Kitty upload across clean frames",
    "[forge-top][drivers]") {
  ForgeTopApp app{make_fake_reader()};
  REQUIRE(app.show_first_process_for_test());
  std::string wire;
  app.run_headless(3, 100, 28, &wire, DriverChoice::Kitty);
  REQUIRE(count(wire, "a=t") == 1);
  REQUIRE(count(wire, "a=f,r=1") == 0);
  REQUIRE(wire.find("a=d,d=I") == std::string::npos);
}

TEST_CASE("forge-top tiny layouts stay total on every tier",
          "[forge-top][failure]") {
  for (const DriverChoice choice :
       {DriverChoice::Fallback, DriverChoice::AnsiRgb, DriverChoice::Kitty}) {
    ForgeTopApp app{make_fake_reader()};
    std::string wire;
    REQUIRE_NOTHROW(app.run_headless(1, 1, 1, &wire, choice));
  }
}

TEST_CASE("forge-top compact layout retains the 20-region Kitty workload",
          "[forge-top][drivers]") {
  ForgeTopApp app{make_fake_reader()};
  std::string wire;
  app.run_headless(1, 70, 22, &wire, DriverChoice::Kitty);
  REQUIRE(count(wire, "a=t") >= 20);
}

TEST_CASE("forge-top waveforms retain 20 Kitty images across clean frames",
          "[forge-top][drivers][persistent]") {
  CpuPanelApp app;
  app.update_frames = {4};
  app.run(std::make_unique<KittyDriver>(), 8);

  REQUIRE(app.sink.attempts.size() == 9); // eight frames plus shutdown
  CHECK(action_count(app.sink.attempts[0], "t") == 20);
  for (int frame : {1, 2, 3}) {
    CHECK(action_count(app.sink.attempts[static_cast<std::size_t>(frame)],
                       "t") == 0);
    CHECK(action_count(app.sink.attempts[static_cast<std::size_t>(frame)],
                       "f") == 0);
  }
  CHECK(action_count(app.sink.attempts[4], "f") == 20);
  for (int frame : {5, 6, 7})
    CHECK(action_count(app.sink.attempts[static_cast<std::size_t>(frame)],
                       "f") == 0);
  CHECK(app.all_regions_clean());
}

TEST_CASE("forge-top waveforms leave clean ANSI frames without image traffic",
          "[forge-top][drivers][persistent]") {
  CpuPanelApp app;
  app.update_frames = {4};
  app.run(std::make_unique<AnsiRgbDriver>(), 8);

  REQUIRE(app.observed_frames.size() == 8);
  CHECK(app.observed_frames[0].total() > 0);
  CHECK(app.sink.attempts[0].find("\xE2\x96\x80") != std::string::npos);
  for (int frame : {1, 2, 3})
    CHECK(app.observed_frames[static_cast<std::size_t>(frame)].total() == 0);
  CHECK(app.observed_frames[4].total() > 0);
  CHECK(app.sink.attempts[4].find("\xE2\x96\x80") != std::string::npos);
  for (int frame : {5, 6, 7})
    CHECK(app.observed_frames[static_cast<std::size_t>(frame)].total() == 0);
  CHECK(app.all_regions_clean());
}

TEST_CASE("forge-top waveform rejection retries before acknowledgement",
          "[forge-top][drivers][persistent][failure]") {
  CpuPanelApp app;
  app.sink.fail_write = 1;
  app.run(std::make_unique<KittyDriver>(), 2);

  REQUIRE(app.sink.attempts.size() == 3); // two frames plus shutdown
  CHECK(action_count(app.sink.attempts[0], "t") == 20);
  CHECK(action_count(app.sink.attempts[1], "t") == 20);
  CHECK(action_count(app.sink.accepted, "t") == 20);
  CHECK(app.all_regions_clean());
}

TEST_CASE("forge-top ANSI repaint rejection retries the clean raster",
          "[forge-top][drivers][persistent][failure][resize]") {
  CpuPanelApp app;
  app.sink.fail_write = 3;
  app.resize_frames = {{1, App::Size{120, 40, 1200, 1000}}};
  app.run(std::make_unique<AnsiRgbDriver>(), 4);

  REQUIRE(app.sink.attempts.size() == 4);
  CHECK(app.sink.attempts[2].find("\xE2\x96\x80") != std::string::npos);
  CHECK(app.sink.attempts[3].find("\xE2\x96\x80") != std::string::npos);
  CHECK(app.all_regions_clean());
}

TEST_CASE("forge-top waveforms recreate at round-trip preferred extents",
          "[forge-top][drivers][persistent][resize]") {
  CpuPanelApp app;
  app.resize_frames = {
      {2, App::Size{120, 40, 1200, 1000}},
      {4, App::Size{120, 40, 960, 640}},
  };
  app.run(std::make_unique<KittyDriver>(), 7);

  REQUIRE(app.resize_results == std::vector<bool>{true, true});
  REQUIRE(app.sink.attempts.size() == 8); // seven frames plus shutdown
  const auto initial = transmit_extents(app.sink.attempts[0]);
  const auto enlarged = transmit_extents(app.sink.attempts[3]);
  const auto restored = transmit_extents(app.sink.attempts[5]);
  REQUIRE(initial.size() == 20);
  REQUIRE(enlarged.size() == 20);
  REQUIRE(restored.size() == 20);
  CHECK(enlarged.front() != initial.front());
  CHECK(restored.front() == initial.front());
  CHECK(action_count(app.sink.attempts[6], "t") == 0);
  CHECK(action_count(app.sink.attempts[6], "f") == 0);
  CHECK(app.all_regions_clean());
}

TEST_CASE("forge-top fallback keeps the authored waveform cells",
          "[forge-top][drivers][fallback]") {
  CpuPanelApp app;
  app.run(std::make_unique<FallbackDriver>(), 1);

  REQUIRE_FALSE(app.sink.attempts.empty());
  CHECK(app.sink.attempts.front().find("\xE2\x96\x88") != std::string::npos);
  CHECK(app.sink.attempts.front().find("\033_G") == std::string::npos);
}
