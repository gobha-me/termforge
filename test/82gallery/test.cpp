#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "gallery_app.hpp"
#include "gallery_capture.hpp"
#include "support/apc.hpp"
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
  std::vector<Rect> rectangles;
  std::vector<int> categories, specimens, counters;
  std::function<void(int)> before, on_after;
  RecordingSink output;
  auto drive(int frames, int tier = 0, int cols = 80, int rows = 24) -> void {
    if (on_after)
      set_frame_observer(
          [this](const FrameObservation&) { on_after(frame - 1); });
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
    const std::array counts{7, 4, 3, 1, 6};
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
  SECTION("Message") {
    Harness app;
    app.input = {"", repeats(next_category, 4), "\t\r", "\r"};
    app.drive(4);
    CHECK(app.overlay_count() == 0);
    CHECK(app.status() == "Message acknowledged.");
  }
  SECTION("Confirm clears the Composer only on confirmation") {
    Harness app;
    app.input = {"", repeats(next_category, 4) + next_card, "\t\r", "n", "\r",
                 "y"};
    app.drive(6);
    CHECK(app.results[3] == "Composer draft kept.");
    CHECK(app.composer_text().empty());
    CHECK(app.status() == "Composer draft cleared.");
  }
  SECTION("Prompt changes the actual input value") {
    Harness app;
    app.input = {"", repeats(next_category, 4) + repeats(next_card, 2), "\t\r",
                 "Renamed\r"};
    app.drive(4);
    CHECK(app.draft() == "Renamed");
    CHECK(app.status() == "TextInput renamed: Renamed");
  }
  SECTION("Choice submits checked preferences") {
    Harness app;
    app.input = {"", repeats(next_category, 4) + repeats(next_card, 3), "\t\r",
                 " \t\t\t\r"};
    app.drive(4);
    CHECK(app.status() == "Choice submitted: 1 selections");
    CHECK(app.overlay_count() == 0);
  }
  SECTION("Wizard reports only the final submission") {
    Harness app;
    app.input = {"", repeats(next_category, 4) + repeats(next_card, 4), "\t\r",
                 "\t\r", "\t\t\r"};
    app.drive(5);
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
    app.drive(4);
    CHECK(app.status() == "Path selected (not opened): " + source.string());
    CHECK(app.overlay_count() == 0);
    CHECK(std::filesystem::exists(source));
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
