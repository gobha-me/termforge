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

  std::filesystem::path root;
};

auto process_stat(int pid, std::string_view name, int utime, int stime,
                  int rss_pages) -> std::string {
  return std::to_string(pid) + " (" + std::string{name} +
         ") S 1 1 1 0 0 0 0 0 0 0 " + std::to_string(utime) + " " +
         std::to_string(stime) + " 0 0 20 0 1 0 10 4096 " +
         std::to_string(rss_pages) + "\n";
}

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
    if (tfsupport::key_value(apc, "a") == action)
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
  REQUIRE(first->processes.front().pid == second->processes.front().pid);
  REQUIRE(first->cpus.front().usage != second->cpus.front().usage);
}

TEST_CASE("forge-top /proc reader computes deltas and tolerates a vanished PID",
          "[forge-top][failure]") {
  TempProc proc;
  proc.write("stat", "cpu 100 0 100 800 0 0 0 0\n"
                     "cpu0 50 0 50 400 0 0 0 0\n"
                     "cpu1 50 0 50 400 0 0 0 0\n");
  proc.write("meminfo", "MemTotal: 1000 kB\nMemAvailable: 600 kB\n"
                        "SwapTotal: 200 kB\nSwapFree: 150 kB\n");
  proc.write("101/stat", process_stat(101, "name with spaces", 10, 5, 7));
  proc.write("202/stat", process_stat(202, "short-lived", 2, 1, 3));

  auto reader = make_proc_reader(proc.root);
  const auto first = reader->sample();
  REQUIRE(first.has_value());
  REQUIRE(first->cpus.size() == 2);
  REQUIRE(first->processes.size() == 2);
  REQUIRE(first->processes.front().cpu_percent == 0.0F);
  REQUIRE(first->memory.total_bytes == 1000 * 1024);

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

TEST_CASE("forge-top process view filters, sorts and preserves PID selection",
          "[forge-top]") {
  ProcessPanel panel;
  panel.set_geometry({0, 0, 80, 14});
  Screen screen{80, 14};
  panel.set_processes({{10, "alpha", 4.0F, 400},
                       {20, "beta", 80.0F, 200},
                       {30, "gamma", 20.0F, 900}});
  panel.draw(screen);
  REQUIRE(panel.visible_rows().front().pid == 20); // CPU descending

  panel.table().set_selected(1); // PID 30
  panel.set_processes({{30, "gamma", 21.0F, 900},
                       {20, "beta", 79.0F, 200},
                       {10, "alpha", 5.0F, 400}});
  REQUIRE(
      panel.visible_rows()[static_cast<std::size_t>(panel.table().selected())]
          .pid == 30);

  panel.set_filter("alp");
  REQUIRE(panel.visible_rows().size() == 1);
  REQUIRE(panel.visible_rows().front().pid == 10);
  panel.set_filter("");
  panel.choose_sort(ProcessSort::Pid);
  REQUIRE(panel.visible_rows().front().pid == 10);
  panel.choose_sort(ProcessSort::Pid);
  REQUIRE(panel.visible_rows().front().pid == 30);

  bool activated = false;
  panel.on_activate(
      [&](const ProcessRow &process) { activated = process.pid == 30; });
  panel.table().set_selected(0);
  REQUIRE(panel.activate_selected());
  REQUIRE(activated);

  panel.draw(screen);
  const bool before = panel.descending();
  const Rect table = panel.table().rect();
  REQUIRE(panel.handle_header_click(
      MouseEvent{table.x + panel.table().gutter_cols() + 1, table.y, 0, true}));
  REQUIRE(panel.sort_key() == ProcessSort::Pid);
  REQUIRE(panel.descending() != before);
}

TEST_CASE("forge-top detail graph acknowledges persistent content",
          "[forge-top]") {
  DetailPopup detail;
  const std::array<float, 4> history{5.0F, 40.0F, 90.0F, 20.0F};
  detail.set_process({42, "renderer", 20.0F, 4096}, history);
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

TEST_CASE("forge-top runs the real frame shape on every forced tier",
          "[forge-top][drivers]") {
  for (const DriverChoice choice :
       {DriverChoice::Fallback, DriverChoice::Ansi, DriverChoice::Kitty}) {
    ForgeTopApp app{make_fake_reader()};
    std::string wire;
    app.run_headless(2, 120, 40, &wire, choice);
    REQUIRE_FALSE(wire.empty());
    if (choice == DriverChoice::Fallback) {
      REQUIRE(wire.find("\033_G") == std::string::npos);
    } else if (choice == DriverChoice::Ansi) {
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
       {DriverChoice::Fallback, DriverChoice::Ansi, DriverChoice::Kitty}) {
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
