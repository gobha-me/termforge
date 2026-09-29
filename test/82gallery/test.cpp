#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "gallery_app.hpp"
#include "gallery_capture.hpp"
#include "support/apc.hpp"
#include "support/terminal_grid.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"

using namespace termforge;
using namespace termforge::examples;

namespace {
const std::string next_category = "\033[9;5u";
const std::string previous_category = "\033[9;6u";
const std::string next_card = "\033[13~";
const std::string previous_card = "\033[12~";
const std::string state_key = "\033[14~";
const std::string help_key = "\033[17~";
const std::string ascii_key = "\033OP";

struct RecordingSink final : ByteSink {
  bool refuse{false};
  std::vector<std::string> frames;
  auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override {
    frames.emplace_back(bytes.begin(), bytes.end());
    if (refuse)
      return std::unexpected(
          ErrorEvent{Severity::Warning, "gallery-test", "refused"});
    return {};
  }
};

class Harness final : public GalleryApp {
 public:
  using GalleryApp::GalleryApp;
  int frame{0};
  std::vector<std::string> input, cells, results;
  std::vector<Screen> screens;
  std::vector<FrameBytes> traffic;
  std::vector<ImageResidency> residency;
  std::vector<Rect> rectangles;
  std::vector<int> categories, specimens, counters;
  std::function<void(int)> before, on_after;
  RecordingSink output;
  auto drive(int frames, int tier = 0, int cols = 80, int rows = 24) -> void {
    set_frame_observer([this](const FrameObservation&) {
      traffic.push_back(driver().last_frame_bytes());
      residency.push_back(driver().residency());
      if (on_after) on_after(frame - 1);
    });
    std::unique_ptr<TerminalDriver> selected;
    if (tier == 0) selected = std::make_unique<FallbackDriver>();
    if (tier == 1) selected = std::make_unique<AnsiRgbDriver>();
    if (tier == 2) selected = std::make_unique<KittyDriver>();
    test_run_frames(frames, cols, rows, &discard, std::move(selected));
  }
  auto on_render(Screen& screen) -> void override {
    driver().set_output(&output);
    GalleryApp::on_render(screen);
    cells.push_back(gallery_cells(screen));
    screens.push_back(screen);
    rectangles.push_back(specimen_rect());
    categories.push_back(category());
    specimens.push_back(specimen());
    results.push_back(status());
    counters.push_back(counter());
    ++frame;
  }
  auto on_tick(std::chrono::duration<double> dt) -> void override {
    if (before) before(frame);
    GalleryApp::on_tick(dt);
  }
  auto now_steady() const -> std::chrono::steady_clock::time_point override {
    return now;
  }
  auto wait_readable(int ms) -> bool override {
    now += std::chrono::milliseconds(ms);
    return false;
  }
  auto read_available(char* destination, int capacity) -> int override {
    if (read_frame != frame) {
      read_frame = frame;
      pending = frame < static_cast<int>(input.size())
                    ? input[static_cast<std::size_t>(frame)]
                    : "";
    }
    const int n = std::min(capacity, static_cast<int>(pending.size()));
    std::copy_n(pending.begin(), n, destination);
    pending.erase(0, static_cast<std::size_t>(n));
    return n;
  }

 private:
  std::string discard, pending;
  int read_frame{-1};
  std::chrono::steady_clock::time_point now{};
};

auto repeats(const std::string& key, int count) -> std::string {
  std::string result;
  for (int i = 0; i < count; ++i)
    result += key;
  return result;
}
auto click(int x, int y) -> std::string {
  return "\033[<0;" + std::to_string(x + 1) + ";" + std::to_string(y + 1) + "M";
}
auto delete_images(const std::string& wire) -> int {
  int count = 0;
  for (const auto& record : tfsupport::apcs(wire))
    if (record.keys.find("a=d,d=I") != std::string::npos) ++count;
  return count;
}

// Only the ASCII fallback frames exercised below: absolute cursor, SGR and
// erase-screen. Fail on any other escape or non-ASCII byte. This is a layout
// oracle for actual emitted modals, not a general terminal/image emulator.
auto emitted_cells(const Harness& app, int last, int cols = 24, int rows = 8)
    -> std::string {
  std::vector<std::string> grid(
      static_cast<std::size_t>(rows),
      std::string(static_cast<std::size_t>(cols), ' '));
  int x = 0, y = 0;
  for (int frame = 0; frame <= last; ++frame) {
    const auto& wire = app.output.frames.at(static_cast<std::size_t>(frame));
    for (std::size_t i = 0; i < wire.size();) {
      if (wire[i] == '\033') {
        if (i + 1 >= wire.size() || wire[i + 1] != '[')
          throw std::runtime_error("unexpected escape in ASCII layout oracle");
        const auto start = i + 2;
        i = start;
        while (i < wire.size() && (wire[i] < '@' || wire[i] > '~'))
          ++i;
        if (i == wire.size()) throw std::runtime_error("truncated CSI");
        const char final = wire[i++];
        const auto params = wire.substr(start, i - start - 1);
        if (final == 'H') {
          const auto split = params.find(';');
          if (split == std::string::npos)
            throw std::runtime_error("expected absolute cursor coordinates");
          y = std::stoi(params.substr(0, split)) - 1;
          x = std::stoi(params.substr(split + 1)) - 1;
        } else if (final == 'J' && params == "2") {
          for (auto& row : grid)
            row.assign(static_cast<std::size_t>(cols), ' ');
        } else if (final != 'm') {
          throw std::runtime_error("unexpected CSI in ASCII layout oracle");
        }
      } else {
        const auto c = static_cast<unsigned char>(wire[i++]);
        if (c < 32 || c >= 127)
          throw std::runtime_error("non-ASCII output in ASCII layout oracle");
        if (x >= 0 && x < cols && y >= 0 && y < rows)
          grid[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] =
              static_cast<char>(c);
        ++x;
      }
    }
  }
  std::string result;
  for (const auto& row : grid)
    result += row + '\n';
  return result;
}
} // namespace

TEST_CASE("Gallery keyboard journey edits persistent values and reports real "
          "actions") {
  Harness app;
  app.input = {
      "",           "\t", " kept\r",     next_card,         "\r",
      next_card,    " ",  next_category, previous_category, previous_card,
      previous_card};
  app.drive(static_cast<int>(app.input.size()));
  CHECK(app.draft() == "TermForge demo kept");
  CHECK(app.results[2] == "Committed demo name: TermForge demo kept");
  CHECK(app.counter() == 1);
  CHECK(app.checked());
  CHECK(app.categories[7] == 1);
  CHECK(app.categories[8] == 0);
  CHECK(app.specimen() == 0);
  CHECK(app.running());
}

TEST_CASE("Gallery deterministic cell captures match every supported tier") {
  for (const auto size :
       {Extent{24, 8}, Extent{80, 24}, Extent{40, 16}, Extent{120, 32}})
    for (int tier = 0; tier < 3; ++tier) {
      INFO(size.w << 'x' << size.h << " tier=" << tier);
      Harness app;
      app.drive(1, tier, size.w, size.h);
      const auto path =
          std::filesystem::path{GALLERY_CAPTURE_DIR} /
          ("after-tier" + std::to_string(tier) + "-" + std::to_string(size.w) +
           "x" + std::to_string(size.h) + ".txt");
      std::ifstream file{path, std::ios::binary};
      REQUIRE(file.is_open());
      const std::string expected{std::istreambuf_iterator<char>{file}, {}};
      REQUIRE(app.cells.size() == 1);
      CHECK(app.cells.front() == expected);
    }
}

TEST_CASE("Gallery reports unsupported TextInput paste and keeps Composer "
          "paste real") {
  Harness app;
  app.input = {"", "\t", "\033[200~not inserted\033[201~",
               repeats(next_category, 2) + next_card,
               "\033[200~ pasted\033[201~"};
  app.drive(5);
  CHECK(app.draft() == "TermForge demo");
  CHECK(app.results[2] ==
        "TextInput demo: type to edit; Composer supports paste.");
  CHECK(app.composer_text().find("pasted") != std::string::npos);
}

TEST_CASE("Gallery makes every specimen reachable on tiny narrow normal and "
          "wide grids") {
  for (const auto size :
       {Extent{24, 8}, Extent{40, 16}, Extent{80, 24}, Extent{120, 32}}) {
    Harness app;
    const std::array counts{9, 4, 3, 1, 6};
    app.input.push_back("");
    for (std::size_t category = 0; category < counts.size(); ++category) {
      for (int card = 1; card < counts[category]; ++card)
        app.input.push_back(next_card);
      if (category + 1 < counts.size()) app.input.push_back(next_category);
    }
    app.drive(static_cast<int>(app.input.size()), 0, size.w, size.h);
    for (std::size_t frame = 0; frame < app.rectangles.size(); ++frame) {
      INFO(size.w << 'x' << size.h << " frame=" << frame);
      const auto r = app.rectangles[frame];
      CHECK_FALSE(r.empty());
      CHECK(r.x >= 0);
      CHECK(r.y >= 3);
      CHECK(r.x + r.w <= size.w);
      CHECK(r.y + r.h <= size.h - 2);
      CHECK(std::all_of(app.cells[frame].begin(), app.cells[frame].end(),
                        [](unsigned char c) { return c < 128; }));
    }
    CHECK(app.category() == 4);
    CHECK(app.specimen() == 5);
  }
}

TEST_CASE(
    "Gallery menu and dropdown click priority agree with painted controls") {
  Harness app;
  app.input = {"",
               repeats(next_card, 4) + "\t",
               "\r",
               "\033[B\r",
               previous_card + previous_card + previous_card,
               ""};
  app.before = [&](int frame) {
    if (frame == 4) {
      const auto r = app.specimen_rect();
      app.input[5] = click(r.x + 2, r.y);
    }
  };
  app.drive(6);
  CHECK(app.results[3] == "Select: Simulated remote");
  CHECK(app.counter() == 1);
  CHECK(app.results.back() == "Counter: 1");
}

TEST_CASE(
    "Gallery disabled state blocks activation without disabling navigation") {
  Harness app;
  app.input = {"",
               next_card + "\t",
               repeats(state_key, 4),
               "\r",
               next_card,
               next_category,
               previous_category,
               repeats(previous_card, 1),
               state_key,
               "\r"};
  app.drive(static_cast<int>(app.input.size()));
  CHECK(app.counters[3] == 0);
  CHECK(app.specimens[4] == 2);
  CHECK(app.categories[5] == 1);
  CHECK(app.categories[6] == 0);
  CHECK(app.counter() == 1);
}

TEST_CASE("Gallery retained text accepts Composer submissions and modal input "
          "stays modal") {
  Harness app;
  app.input = {"",
               repeats(next_category, 2) + next_card,
               "\t",
               "\033[200~ hello\033[201~\r",
               repeats(next_category, 2),
               "\r",
               "hidden",
               "\033",
               previous_category + previous_category};
  app.drive(static_cast<int>(app.input.size()));
  CHECK(app.results[3].find("Composer sent") != std::string::npos);
  CHECK(app.composer_text().empty());
  CHECK(app.results[5].find("Modal open") != std::string::npos);
  CHECK(app.categories[6] == 4);
  CHECK(app.running());
  CHECK(app.overlay_count() == 0);
  CHECK(app.specimen() == 1);
}

TEST_CASE("Gallery resize recovers from a physically unusable grid") {
  Harness app;
  app.input = {"", "", "", "\t resized\r"};
  app.before = [&](int frame) {
    if (frame == 0) REQUIRE(app.set_size({8, 3}));
    if (frame == 1) REQUIRE(app.set_size({24, 8}));
  };
  app.drive(4);
  CHECK(app.cells[1].find("Resize") != std::string::npos);
  CHECK(app.rectangles[2].w == 24);
  CHECK_FALSE(app.rectangles[2].empty());
  CHECK(app.draft().find("resized") != std::string::npos);
}

TEST_CASE(
    "Gallery hidden controls and dropdowns cannot receive input after shrink") {
  Harness app;
  app.input = {"", repeats(next_card, 4) + "\t\r",
               "", "hidden\r" + click(3, 8),
               "", "\t"};
  app.before = [&](int frame) {
    if (frame == 1) REQUIRE(app.set_size({8, 3}));
    if (frame == 3) REQUIRE(app.set_size({24, 8}));
  };
  app.drive(6);
  CHECK(app.rectangles[2].empty());
  CHECK(app.rectangles[3].empty());
  CHECK(app.draft() == "TermForge demo");
  CHECK(app.counter() == 0);
  CHECK(app.overlay_count() == 0);
  CHECK(app.running());
}

TEST_CASE(
    "Gallery focus traverses both boundaries without trapping navigation") {
  Harness app;
  app.input = {"", "\t", "\033[Z", "\033[C", "\033[Z", "\t", "\033[C"};
  app.drive(7);
  CHECK(app.categories[3] == 1);
  CHECK(app.categories[6] == 2);
  CHECK(app.cells[4].find("Focus: menu") != std::string::npos);
  CHECK(app.cells[5].find("Focus: tabs") != std::string::npos);
}

TEST_CASE("Gallery decoded modal mouse capture survives a resize") {
  Harness app;
  app.input = {repeats(next_category, 4), "\t\r", click(0, 0), "", ""};
  app.before = [&](int frame) {
    if (frame == 2) REQUIRE(app.set_size({40, 16}));
    if (frame == 3) {
      REQUIRE(app.top_overlay() != nullptr);
      // This geometry is replaced during frame 3's modal render. Schedule the
      // click after that render in the next read_available call instead.
    }
  };
  app.on_after = [&](int frame) {
    if (frame == 3) {
      const auto r = app.top_overlay()->rect();
      // MessageDialog's one button is right-aligned, not centered.
      app.input[4] = click(r.x + r.w - 4, r.y + r.h - 2);
    }
  };
  app.drive(5);
  CHECK(app.counter() == 0);
  CHECK(app.status() == "Message acknowledged.");
  CHECK(app.overlay_count() == 0);
  CHECK(app.running());
}

TEST_CASE(
    "Gallery modal results are real and file selection never opens a file") {
  const int cols = GENERATE(24, 80);
  const int rows = cols == 24 ? 8 : 24;
  SECTION("Message") {
    Harness app;
    app.input = {"", repeats(next_category, 4), "\t\r", "\r"};
    app.drive(4, 0, cols, rows);
    CHECK(app.overlay_count() == 0);
    CHECK(app.status() == "Message acknowledged.");
  }
  SECTION("Confirm clears the Composer only on confirmation") {
    Harness app;
    app.input = {"", repeats(next_category, 4) + next_card, "\t\r", "n", "\r",
                 "y"};
    app.drive(6, 0, cols, rows);
    CHECK(app.results[3] == "Composer draft kept.");
    CHECK(app.composer_text().empty());
    CHECK(app.status() == "Composer draft cleared.");
  }
  SECTION("Prompt changes the actual input value") {
    Harness app;
    app.input = {"", repeats(next_category, 4) + repeats(next_card, 2), "\t\r",
                 "Renamed\r"};
    app.drive(4, 0, cols, rows);
    CHECK(app.draft() == "Renamed");
    CHECK(app.status() == "TextInput renamed: Renamed");
  }
  SECTION("Choice submits checked preferences") {
    Harness app;
    app.input = {"", repeats(next_category, 4) + repeats(next_card, 3), "\t\r",
                 " \t\t\t\r"};
    app.drive(4, 0, cols, rows);
    CHECK(app.status() == "Choice submitted: 1 selections");
    CHECK(app.overlay_count() == 0);
  }
  SECTION("Wizard reports only the final submission") {
    Harness app;
    app.input = {"", repeats(next_category, 4) + repeats(next_card, 4), "\t\r",
                 "\t\r", "\t\t\r"};
    app.drive(5, 0, cols, rows);
    CHECK(app.results[3].find("Modal open") != std::string::npos);
    CHECK(app.status() == "Wizard submitted: 2 pages");
    CHECK(app.overlay_count() == 0);
  }
  SECTION("FilePicker selects an existing source path without writes") {
    const auto source = std::filesystem::absolute(__FILE__);
    Harness app{source.parent_path()};
    // Initial list focus -> Shift+Tab path field -> Home, delete the seeded
    // directory, type an existing file. This is real widget navigation.
    app.input = {"", repeats(next_category, 4) + repeats(next_card, 5), "\t\r",
                 "\033[Z\033[H" + repeats("\033[3~", 1000) + source.string() +
                     "\r"};
    app.drive(4, 0, cols, rows);
    CHECK(app.status() == "Path selected (not opened): " + source.string());
    CHECK(app.overlay_count() == 0);
    CHECK(std::filesystem::exists(source));
  }
}

TEST_CASE("Gallery short help is a reachable scrolling document on emitted "
          "ASCII frames") {
  Harness app;
  app.input = {"",        help_key,        "\033[6~", "\033[F",
               "\033[H",  "\033[<65;4;4M", "\033[A",  "\033[B",
               "\033[5~", help_key,        help_key,  "\033",
               ""};
  app.drive(static_cast<int>(app.input.size()), 0, 24, 8);
  CHECK(emitted_cells(app, 1).find("Esc/F6: back") != std::string::npos);
  CHECK(emitted_cells(app, 2) != emitted_cells(app, 1));
  CHECK(emitted_cells(app, 3).find("End of help.") != std::string::npos);
  CHECK(emitted_cells(app, 4) == emitted_cells(app, 1));
  CHECK(emitted_cells(app, 5) != emitted_cells(app, 4));
  CHECK(emitted_cells(app, 6) != emitted_cells(app, 5));
  CHECK(emitted_cells(app, 7) == emitted_cells(app, 5));
  CHECK(emitted_cells(app, 8) == emitted_cells(app, 1));
  CHECK(emitted_cells(app, 10) == emitted_cells(app, 1));
  CHECK(app.overlay_count() == 0);
  CHECK(app.running());
  CHECK(app.draft() == "TermForge demo");
}

TEST_CASE("Gallery short choice and both wizard pages paint actionable "
          "preferences") {
  SECTION("Choice keyboard and mouse operate visible options") {
    Harness app;
    app.input = {"", repeats(next_category, 4) + repeats(next_card, 3), "\t\r",
                 click(4, 1), "\t\t\t\r"};
    app.drive(5, 0, 24, 8);
    const auto painted = emitted_cells(app, 2);
    CHECK(painted.find("Mouse") != std::string::npos);
    CHECK(painted.find("Motion") != std::string::npos);
    CHECK(painted.find("Detail") != std::string::npos);
    CHECK(painted.find("Submit") != std::string::npos);
    CHECK(app.status() == "Choice submitted: 1 selections");
    CHECK(app.overlay_count() == 0);
  }
  SECTION("Wizard keeps both pages and their navigation visible") {
    Harness app;
    app.input = {"", repeats(next_category, 4) + repeats(next_card, 4), "\t\r",
                 "\t\r", "\t\t\r"};
    app.drive(5, 0, 24, 8);
    CHECK(emitted_cells(app, 2).find("Compact") != std::string::npos);
    CHECK(emitted_cells(app, 2).find("Detailed") != std::string::npos);
    CHECK(emitted_cells(app, 2).find("[ > ]") != std::string::npos);
    CHECK(emitted_cells(app, 2).find("[ Esc ]") != std::string::npos);
    CHECK(emitted_cells(app, 3).find("Quiet") != std::string::npos);
    CHECK(emitted_cells(app, 3).find("Verbose") != std::string::npos);
    CHECK(emitted_cells(app, 3).find("[ < ]") != std::string::npos);
    CHECK(emitted_cells(app, 3).find("[ OK ]") != std::string::npos);
    CHECK(emitted_cells(app, 3).find("[ Esc ]") != std::string::npos);
    CHECK(app.status() == "Wizard submitted: 2 pages");
  }
}

TEST_CASE("Gallery modal floor refuses hidden forms and cancels them on "
          "shrink while help still reflows") {
  SECTION("Refuse a form below the floor, then reopen after resize") {
    const int cols = GENERATE(12, 24);
    Harness app;
    app.input = {"", repeats(next_category, 4), "\t\r", "", "\r", "\r"};
    app.before = [&](int frame) {
      if (frame == 2) REQUIRE(app.set_size({24, 8}));
    };
    app.drive(6, 0, cols, 7);
    CHECK(app.results[2].find("Resize >=24x8") != std::string::npos);
    CHECK(emitted_cells(app, 2, cols, 7).find("Resize>=24x8") !=
          std::string::npos);
    CHECK(app.status() == "Message acknowledged.");
    CHECK(app.overlay_count() == 0);
  }
  SECTION("Shrink cancels a prompt without committing the typed draft") {
    Harness app;
    app.input = {"", repeats(next_category, 4) + repeats(next_card, 2), "\t\r",
                 "uncommitted", "\r"};
    app.before = [&](int frame) {
      if (frame == 3) REQUIRE(app.set_size({24, 7}));
    };
    app.drive(5, 0, 24, 8);
    CHECK(app.overlay_count() == 0);
    CHECK(app.draft() == "TermForge demo");
    CHECK(app.status().find("Resize >=24x8") != std::string::npos);
    CHECK(emitted_cells(app, 4, 24, 7).find("Resize>=24x8") !=
          std::string::npos);
  }
  SECTION("Help remains reachable after shrinking and expanding its viewport") {
    Harness app;
    app.input = {"", help_key, "", "\033[F", "", "\033[H", "\033", ""};
    app.before = [&](int frame) {
      if (frame == 1) REQUIRE(app.set_size({12, 6}));
      if (frame == 3) REQUIRE(app.set_size({40, 16}));
    };
    app.drive(8, 0, 24, 8);
    CHECK(emitted_cells(app, 3, 12, 6).find("returns.") != std::string::npos);
    CHECK(emitted_cells(app, 5, 40, 16).find("Esc/F6: back") !=
          std::string::npos);
    CHECK(app.overlay_count() == 0);
    CHECK(app.running());
  }
}

TEST_CASE(
    "Gallery menu mouse actions and category/card clicks change real state") {
  Harness app;
  app.input = {"", click(2, 1), "", "", "", ""};
  app.before = [&](int frame) {
    if (frame == 1) app.input[2] = click(4, 2);  // Demo > Reset values
    if (frame == 2) app.input[3] = click(14, 2); // Data category
    if (frame == 3) app.input[4] = click(78, 4); // next specimen
    if (frame == 4) {
      const auto r = app.specimen_rect();
      app.input[5] = click(r.x + 2, r.y + 2); // second table data row
    }
  };
  app.drive(6);
  CHECK(app.results[2].find("Demo reset") != std::string::npos);
  CHECK(app.category() == 1);
  CHECK(app.specimen() == 1);
  CHECK(app.status() == "Table row: 2");
}

TEST_CASE("Gallery persistent pixels retain then retire through actual App "
          "tier paths") {
  for (int tier = 0; tier < 3; ++tier) {
    Harness app;
    app.input = {repeats(next_category, 3), "", help_key, "\033",
                 next_category};
    app.drive(5, tier, 40, 16);
    CHECK(app.category() == 4);
    CHECK(app.overlay_count() == 0);
    CHECK(app.running());
    if (tier == 0) {
      CHECK(app.pixel_submissions() == 0);
      CHECK(app.ascii());
    } else {
      CHECK(app.pixel_submissions() == 1);
      if (tier == 2) {
        REQUIRE(app.output.frames.size() >= 5);
        CHECK(app.output.frames[1].find("a=t") == std::string::npos);
        CHECK(delete_images(app.output.frames[2]) == 0);
        CHECK(app.output.frames[2].find("d=i") != std::string::npos);
        CHECK(delete_images(app.output.frames[4]) == 1);
      }
    }
  }
}

TEST_CASE("Gallery sink refusal retries retirement and presentation changes "
          "keep drafts") {
  Harness app;
  app.input = {
      repeats(next_category, 3),     "",       next_category, "", ascii_key,
      repeats(previous_category, 4), "\t safe"};
  app.before = [&](int frame) { app.output.refuse = frame == 2; };
  app.drive(7, 2, 40, 16);
  REQUIRE(app.output.frames.size() >= 4);
  CHECK(delete_images(app.output.frames[2]) == 1);
  CHECK(delete_images(app.output.frames[3]) == 1);
  CHECK(app.ascii());
  CHECK(app.draft().find("safe") != std::string::npos);
}

TEST_CASE("Gallery real palette journeys pin styles and actual frames on every "
          "tier") {
  std::string evidence;
  constexpr std::array names{"initial",  "focus", "category", "selection",
                             "disabled", "error", "help"};
  for (const auto palette :
       {GalleryPalette::Dark, GalleryPalette::HighContrast})
    for (int tier = 0; tier < 3; ++tier)
      for (const bool ascii : {false, true}) {
        INFO(static_cast<int>(palette)
             << " tier=" << tier << " ascii=" << ascii);
        Harness app;
        app.set_palette(palette);
        if (ascii) app.set_ascii(true);
        app.input = gallery_theme_journey();
        app.drive(7, tier);
        const auto value = app.presentation_theme();
        CHECK(app.palette() == palette);
        CHECK(app.ascii() == (ascii || tier == 0));
        CHECK(value.glyphs ==
              (app.ascii() ? BorderStyle::Ascii : BorderStyle::Rounded));
        if (palette == GalleryPalette::HighContrast) {
          CHECK(value.content_fg == Rgb{255, 255, 255});
          CHECK(value.content_bg == Rgb{0, 0, 0});
          CHECK(value.focus_bg == Rgb{255, 255, 0});
          CHECK(value.selection_bg == Rgb{0, 255, 255});
        }
        const auto focus = app.rectangles[1];
        bool cursor = false;
        for (int x = focus.x; x < focus.x + focus.w; ++x) {
          const auto& cell = app.screens[1].at(x, focus.y);
          cursor |= cell.fg == value.focus_fg && cell.bg == value.focus_bg &&
                    (cell.attrs & Attr::Reverse) != Attr::None;
        }
        CHECK(cursor);
        const auto selected = app.rectangles[3];
        const auto& marker = app.screens[3].at(selected.x, selected.y);
        CHECK(marker.fg == value.selection_fg);
        CHECK(marker.bg == value.selection_bg);
        CHECK((marker.attrs & Attr::Bold) != Attr::None);
        CHECK_FALSE(app.screens[3].text_at(selected.x, selected.y).empty());
        CHECK(app.cells[4].find("[disabled]") != std::string::npos);
        const auto disabled = app.rectangles[4];
        for (int y = disabled.y; y < disabled.y + disabled.h; ++y)
          for (int x = disabled.x; x < disabled.x + disabled.w; ++x)
            CHECK((app.screens[4].at(x, y).attrs & Attr::Dim) != Attr::None);
        CHECK(app.results[4] == "Demo data: disabled (simulated).");
        const auto& status = app.screens[5].at(0, 22);
        CHECK(status.fg == value.error);
        CHECK(status.bg == value.surface_bg);
        CHECK((status.attrs & Attr::Bold) != Attr::None);
        const auto error = app.rectangles[5];
        CHECK(app.screens[5].at(error.x, error.y).fg == value.error);
        CHECK(app.cells[5].find("Simulated error") != std::string::npos);
        REQUIRE(app.output.frames.size() >= 7);
        if (tier == 0) {
          CHECK(app.output.frames[1].find("38;2;") == std::string::npos);
          CHECK(app.output.frames[3].find("[1m") != std::string::npos);
          CHECK(app.output.frames[5].find("[1m") != std::string::npos);
        }
        if (tier > 0 && app.ascii()) {
          tfsupport::TerminalGrid terminal{80, 24};
          for (int i = 0; i < 7; ++i)
            terminal.feed(app.output.frames[static_cast<std::size_t>(i)]);
          REQUIRE(app.top_overlay());
          const auto dialog = app.top_overlay()->rect();
          // This is the emitted modal, not the restored backdrop Screen.
          const auto& cell = terminal.at(dialog.x + 1, dialog.y + 1);
          const auto rgb = [](Rgb color) {
            return (color.r << 16) | (color.g << 8) | color.b;
          };
          CHECK(cell.fg == rgb(value.content_fg));
          CHECK(cell.bg == rgb(value.content_bg));
        }
        evidence += std::format("# palette={} tier={} mode={}\n",
                                palette == GalleryPalette::Dark ? "dark" : "hc",
                                tier, ascii ? "ascii" : "native");
        for (std::size_t i = 0; i < names.size(); ++i)
          evidence += std::format(
              "{} {} wire={} bytes={}\n", names[i],
              gallery_theme_record(app.screens[i], app.rectangles[i]),
              gallery_fingerprint(app.output.frames[i]),
              app.output.frames[i].size());
      }
  const auto path =
      std::filesystem::path{GALLERY_CAPTURE_DIR} / "theme-evidence.txt";
  std::ifstream file{path, std::ios::binary};
  REQUIRE(file.is_open());
  const std::string expected{std::istreambuf_iterator<char>{file}, {}};
  CHECK(evidence == expected);
}

TEST_CASE("Gallery palette changes preserve models focus history and source "
          "identity") {
  const int tier = GENERATE(0, 1, 2);
  Harness app;
  app.input = {"", "\t retained", "", "", "", ""};
  TextEntryHandle stream;
  std::size_t transcript_bytes = 0, block_bytes = 0;
  const Pixel* source = nullptr;
  std::vector<Pixel> rgba;
  app.before = [&](int frame) {
    if (frame == 1) {
      stream = app.live_stream();
      REQUIRE(stream);
      transcript_bytes = app.transcript().retained_bytes();
      block_bytes = app.document_blocks().retained_bytes();
      source = app.framebuffer().pixels().data();
      rgba.assign(app.framebuffer().pixels().begin(),
                  app.framebuffer().pixels().end());
    }
    if (frame >= 2)
      app.set_palette(frame % 2 == 0 ? GalleryPalette::HighContrast
                                     : GalleryPalette::Dark);
  };
  app.drive(6, tier);
  CHECK(app.draft() == "TermForge demo retained");
  CHECK(app.results[1] == "Draft changed; Enter commits.");
  for (std::size_t frame = 2; frame < app.results.size(); ++frame) {
    CHECK(app.results[frame] == app.results[1]);
    // The palette header changes; glyph/text content below it does not.
    CHECK(app.cells[frame].substr(app.cells[frame].find('\n') + 1) ==
          app.cells[1].substr(app.cells[1].find('\n') + 1));
  }
  CHECK(app.live_stream() == stream);
  CHECK(app.transcript().retained_bytes() == transcript_bytes);
  CHECK(app.document_blocks().retained_bytes() == block_bytes);
  CHECK(app.document_blocks().block_count() == 1);
  CHECK(app.framebuffer().pixels().data() == source);
  CHECK(
      std::equal(rgba.begin(), rgba.end(), app.framebuffer().pixels().begin()));
  CHECK(app.counter() == 0);
  CHECK_FALSE(app.checked());
  CHECK(app.composer().history_size() == 0);
}

TEST_CASE(
    "Gallery View palette actions preserve draft and existing menu indices") {
  Harness app;
  // F10 focuses Demo; Right selects View. Down enters its first item;
  // four further Down steps reach the appended high-contrast action.
  app.input = {"", "\t kept",
               "\033[21~\033[C\033[B" + repeats("\033[B", 4) + "\r",
               "\033[B" + repeats("\033[B", 3) + "\r"};
  app.drive(4);
  CHECK(app.palette() == GalleryPalette::Dark);
  CHECK(app.draft() == "TermForge demo kept");
  CHECK(app.results[2] == app.results[1]);
  CHECK(app.screens[2].at(0, 0).fg == Rgb{255, 255, 0});
  CHECK(app.screens[3].at(0, 0).fg == Rgb{112, 190, 230});
  CHECK(app.ascii());
}

TEST_CASE("Gallery appended Slider and NumericInput have real edits and "
          "diagnostics") {
  const int tier = GENERATE(0, 1, 2);
  Harness app;
  app.set_palette(GalleryPalette::HighContrast);
  app.input = {"",
               repeats(next_card, 7) + "\t\033[C",
               next_card,
               "\033[H" + repeats("\033[3~", 3) + "27\r",
               "\033[H" + repeats("\033[3~", 3) + "bad\r",
               "",
               "\033[27u"};
  app.before = [&](int frame) {
    if (frame == 5) app.set_palette(GalleryPalette::Dark);
  };
  app.drive(7, tier);
  CHECK(app.slider_value() == 45);
  CHECK(app.results[1] == "Slider: 45");
  CHECK(app.results[3] == "NumericInput: 27");
  CHECK(std::get<std::int64_t>(app.numeric_value()) == 27);
  CHECK(app.numeric_draft() == "27");
  CHECK(app.cells[4].find("bad") != std::string::npos);
  CHECK(app.cells[4].find('!') != std::string::npos);
  CHECK(app.cells[5].find("bad") != std::string::npos);
  bool warning = false;
  const auto body = app.rectangles[4];
  for (int x = body.x; x < body.x + body.w; ++x) {
    const auto& cell = app.screens[4].at(x, body.y);
    warning |=
        cell.fg == Rgb{255, 192, 0} && (cell.attrs & Attr::Bold) != Attr::None;
  }
  CHECK(warning);
  CHECK(app.running());
}

TEST_CASE(
    "Gallery palette changes keep list and table selections with scroll") {
  const bool table = GENERATE(false, true);
  const int tier = GENERATE(0, 1, 2);
  Harness app;
  app.input = {"",
               next_category + (table ? next_card : ""),
               "\t" + repeats("\033[B", 20),
               "",
               "",
               ""};
  int selected = -1, scroll = -1;
  app.before = [&](int frame) {
    if (frame == 2) {
      selected =
          table ? app.demo_table().selected() : app.demo_list().selected();
      scroll = table ? app.demo_table().scroll_offset()
                     : app.demo_list().scroll_offset();
      REQUIRE(selected == 20);
      REQUIRE(scroll > 0);
    }
    if (frame == 3) app.set_palette(GalleryPalette::HighContrast);
    if (frame == 4) app.set_palette(GalleryPalette::Dark);
  };
  app.drive(6, tier, 40, 16);
  CHECK((table ? app.demo_table().selected() : app.demo_list().selected()) ==
        selected);
  CHECK((table ? app.demo_table().scroll_offset()
               : app.demo_list().scroll_offset()) == scroll);
  CHECK(app.results[3] == app.results[2]);
  CHECK(app.results[4] == app.results[2]);
  CHECK(app.demo_table().row_count() == 24);
}

TEST_CASE(
    "Gallery palettes preserve Composer history and authored block styles") {
  const int tier = GENERATE(0, 1, 2);
  Harness app;
  app.input = {
      "",       repeats(next_category, 2) + next_card, "\t\r", "\033[A", "", "",
      next_card};
  app.before = [&](int frame) {
    if (frame == 4) app.set_palette(GalleryPalette::HighContrast);
  };
  app.drive(7, tier);
  CHECK(app.composer().history_size() == 1);
  CHECK(app.composer_text() ==
        "Edit this draft; Enter sends it to the transcript.");
  CHECK(app.results[4] == app.results[3]);
  CHECK(app.document_blocks().block_count() == 1);
  bool authored = false, inherited = false;
  const auto& screen = app.screens[6];
  const auto body = app.rectangles[6];
  for (int y = body.y; y < body.y + body.h; ++y)
    for (int x = body.x; x < body.x + body.w; ++x) {
      if (screen.text_at(x, y).empty()) continue;
      const auto& cell = screen.at(x, y);
      authored |= cell.fg == theme::kFg && cell.bg == theme::kBg;
      inherited |= cell.fg == Rgb{255, 255, 255} && cell.bg == Rgb{0, 0, 0};
    }
  CHECK(authored);
  CHECK(inherited);
}

TEST_CASE(
    "Gallery slider routes drag release outside its hit area and cancels") {
  const bool cancel = GENERATE(false, true);
  Harness app;
  app.input = {"", repeats(next_card, 7) + "\t", "", "", ""};
  app.before = [&](int frame) {
    const auto body = app.specimen_rect();
    if (frame == 1) app.input[2] = click(body.x + body.w / 2, body.y);
    if (frame == 2) {
      app.set_palette(GalleryPalette::HighContrast);
      app.input[3] = std::format("\033[<32;{};1M", body.x + body.w + 1);
    }
    if (frame == 3)
      app.input[4] = cancel ? "\033[27u"
                            : std::format("\033[<0;{};1m", body.x + body.w + 1);
  };
  app.drive(5);
  CHECK(app.slider_value() == (cancel ? 40 : 100));
  CHECK(app.running());
}

TEST_CASE(
    "Gallery palette changes preserve open selection popup and modal draft") {
  const int tier = GENERATE(0, 1, 2);
  SECTION("Selection popup remains open and commits the original choice") {
    Harness app;
    app.input = {"", repeats(next_card, 4) + "\t\r", "", "\033[B\r"};
    app.before = [&](int frame) {
      if (frame == 2) app.set_palette(GalleryPalette::HighContrast);
    };
    app.drive(4, tier);
    CHECK(app.cells[2].find("Simulated remote") != std::string::npos);
    CHECK(app.status() == "Select: Simulated remote");
  }
  SECTION("Prompt uses the new snapshot without discarding uncommitted text") {
    Harness app;
    app.input = {"",     repeats(next_category, 4) + repeats(next_card, 2),
                 "\t\r", "Kept",
                 "",     "\r"};
    app.before = [&](int frame) {
      if (frame == 4) app.set_palette(GalleryPalette::HighContrast);
    };
    app.on_after = [&](int frame) {
      if (frame == 4) {
        REQUIRE(app.top_overlay());
        CHECK(app.top_overlay()->theme_snapshot() == app.presentation_theme());
      }
    };
    app.drive(6, tier);
    CHECK(app.draft() == "Kept");
    CHECK(app.status() == "TextInput renamed: Kept");
  }
}

TEST_CASE(
    "Gallery clean authored pixels survive palette changes and refused cells") {
  const int tier = GENERATE(1, 2);
  const bool refusal = GENERATE(false, true);
  Harness app;
  app.input = {repeats(next_category, 3), "", "", ""};
  const Pixel* source = nullptr;
  std::vector<Pixel> rgba;
  app.before = [&](int frame) {
    if (frame == 1) {
      source = app.framebuffer().pixels().data();
      rgba.assign(app.framebuffer().pixels().begin(),
                  app.framebuffer().pixels().end());
      app.set_palette(GalleryPalette::HighContrast);
    }
    app.output.refuse = refusal && frame == 1;
  };
  app.drive(4, tier, 40, 16);
  CHECK(app.pixel_submissions() == 1);
  CHECK(app.framebuffer().pixels().data() == source);
  CHECK(
      std::equal(rgba.begin(), rgba.end(), app.framebuffer().pixels().begin()));
  REQUIRE(app.traffic.size() == 4);
  for (std::size_t frame = 1; frame < 4; ++frame) {
    CHECK(app.traffic[frame].image_transmit == 0);
    CHECK(delete_images(app.output.frames[frame]) == 0);
    if (tier == 2) {
      CHECK(app.residency[frame].pinned_images == 1);
      CHECK(app.residency[frame].source_payload_bytes ==
            std::uint64_t{32} * 16U * 4U);
    }
  }
  if (refusal) CHECK(app.results[2].find("refused") != std::string::npos);
}

TEST_CASE(
    "Gallery disabled pixels use marked cells and retire accepted roots") {
  const int tier = GENERATE(0, 1, 2);
  Harness app;
  app.input = {repeats(next_category, 3), repeats(state_key, 4), ""};
  app.drive(3, tier, 24, 8);
  CHECK(app.cells[1].find("[disabled]") != std::string::npos);
  const auto body = app.rectangles[1];
  for (int y = body.y; y < body.y + body.h; ++y)
    for (int x = body.x; x < body.x + body.w; ++x)
      CHECK((app.screens[1].at(x, y).attrs & Attr::Dim) != Attr::None);
  CHECK(app.traffic[1].image_transmit == 0);
  if (tier == 2) {
    CHECK(delete_images(app.output.frames[1]) == 1);
    CHECK(app.residency[1].pinned_images == 0);
  }
}

TEST_CASE(
    "Gallery style runs retain every RGB attribute and wide continuation") {
  Screen screen{4, 1};
  screen.clear({1, 2, 3}, {4, 5, 6});
  screen.write_text(0, 0, "日本", {1, 2, 3}, {4, 5, 6}, Attr::Bold);
  CHECK(gallery_cells(screen) == "日本\n");
  CHECK(gallery_styles(screen) == "0:0+4 fg=010203 bg=040506 a=1\n");
  const auto before = gallery_fingerprint(gallery_styles(screen));
  screen.at(1, 0).attrs = Attr::Reverse;
  CHECK(gallery_fingerprint(gallery_styles(screen)) != before);
  CHECK(gallery_cells(screen) == "日本\n");
  CHECK(gallery_styles(screen) == "0:0+1 fg=010203 bg=040506 a=1\n"
                                  "0:1+1 fg=010203 bg=040506 a=16\n"
                                  "0:2+2 fg=010203 bg=040506 a=1\n");
}

TEST_CASE(
    "Gallery modal entry stops Slider capture without restoring its value") {
  Harness app;
  app.input = {"", repeats(next_card, 7) + "\t", "", help_key, "", "\033[27u",
               ""};
  app.before = [&](int frame) {
    const auto body = app.specimen_rect();
    if (frame == 1) app.input[2] = click(body.x + body.w / 2, body.y);
    if (frame == 3)
      app.input[4] = std::format("\033[<0;{};1m", body.x + body.w + 1);
    if (frame == 5)
      app.input[6] = std::format("\033[<32;{};1M", body.x + body.w + 1);
  };
  app.drive(7);
  CHECK(app.overlay_count() == 0);
  CHECK(app.slider_value() == 0);
  CHECK(app.running());
}

TEST_CASE(
    "Gallery disabling Slider stops capture before the next batched release") {
  Harness app;
  app.input = {"", repeats(next_card, 7) + "\t", "", ""};
  app.before = [&](int frame) {
    const auto body = app.specimen_rect();
    if (frame == 1) app.input[2] = click(body.x + body.w / 2, body.y);
    if (frame == 2)
      app.input[3] = repeats(state_key, 4) +
                     std::format("\033[<0;{};1m", body.x + body.w + 1);
  };
  app.drive(4);
  CHECK(app.slider_value() == 0);
  CHECK(app.results[3] == "Demo data: disabled (simulated).");
  CHECK(app.cells[3].find("[disabled]") != std::string::npos);
}

TEST_CASE("Gallery tiny fallback states keep readable diagnostic and disabled "
          "markers") {
  Harness app;
  app.input = {"", repeats(state_key, 4), state_key + repeats(state_key, 3)};
  app.drive(3, 0, 12, 6);
  CHECK(app.cells[1].find("[disabled]") != std::string::npos);
  CHECK(app.cells[2].find("[error]") != std::string::npos);
  CHECK((app.screens[2].at(0, 4).attrs & Attr::Bold) != Attr::None);
  CHECK(app.output.frames[2].find("[1m") != std::string::npos);
}
