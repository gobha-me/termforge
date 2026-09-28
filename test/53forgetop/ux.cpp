// Actual ForgeTop App frames and decoded input. Screen observations are only
// the underlying app; modal visibility below is decoded from accepted bytes.
// The ASCII oracle is intentionally not a terminal or image emulator.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <expected>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "forge_top.hpp"
#include "support/apc.hpp"
#include "support/screen.hpp"

using namespace termforge;
using namespace termforge::forge_top;

namespace {
const std::string f1 = "\033OP", f2 = "\033OQ", f3 = "\033OR";
const std::string end_key = "\033[F", home_key = "\033[H";

auto cells(const Screen& screen) -> std::string {
  std::string result;
  for (int y = 0; y < screen.rows(); ++y)
    result += tfsupport::row_text(screen, y) + '\n';
  return result;
}

struct Journey {
  SyntheticClock clock;
  std::string wire;
  ForgeTopApp app;
  std::vector<std::string> frames, screens, statuses, errors;
  std::vector<std::array<bool, 4>> sections;
  std::function<void(int)> next;
  explicit Journey(std::unique_ptr<SystemReader> reader = make_fake_reader())
      : app(std::move(reader), true) {
    app.set_clock(&clock);
    app.set_frame_observer([this](const FrameObservation& observation) {
      REQUIRE(observation.output_accepted);
      frames.push_back(wire.substr(offset));
      offset = wire.size();
      screens.push_back(cells(app.screen_for_test()));
      statuses.push_back(app.status_for_test());
      errors.push_back(app.sample_error_for_test());
      sections.push_back(app.section_state_for_test());
      if (next) next(static_cast<int>(frames.size()) - 1);
    });
  }
  auto drive(int frames_count, int cols = 80, int rows = 24,
             DriverChoice tier = DriverChoice::Fallback) -> void {
    app.run_headless(frames_count, cols, rows, &wire, tier);
  }
  auto keys(const std::string& bytes) -> void { app.test_pump({bytes}); }

 private:
  std::size_t offset{0};
};

auto click(int x, int y) -> std::string {
  return std::format("\033[<0;{};{}M", x + 1, y + 1);
}
auto image_actions(std::string_view bytes, std::string_view action) -> int {
  int result = 0;
  for (const auto& record : tfsupport::apcs(bytes))
    if (tfsupport::key_value(record, "a") == action &&
        tfsupport::has_key(record, "i"))
      ++result;
  return result;
}

auto emitted(const Journey& journey, int last, int cols, int rows)
    -> std::string {
  std::vector<std::string> grid(
      static_cast<std::size_t>(rows),
      std::string(static_cast<std::size_t>(cols), ' '));
  int x = 0, y = 0;
  for (int frame = 0; frame <= last; ++frame) {
    const auto& bytes = journey.frames.at(static_cast<std::size_t>(frame));
    for (std::size_t i = 0; i < bytes.size();) {
      if (bytes[i] == '\033') {
        if (i + 1 >= bytes.size() || bytes[i + 1] != '[')
          throw std::runtime_error("unexpected ASCII escape");
        const auto start = i + 2;
        i = start;
        while (i < bytes.size() && (bytes[i] < '@' || bytes[i] > '~'))
          ++i;
        if (i == bytes.size()) throw std::runtime_error("truncated CSI");
        const char final = bytes[i++];
        const auto params = bytes.substr(start, i - start - 1);
        if (final == 'H') {
          const auto split = params.find(';');
          if (split == std::string::npos) throw std::runtime_error("cursor");
          y = std::stoi(params.substr(0, split)) - 1;
          x = std::stoi(params.substr(split + 1)) - 1;
        } else if (final == 'J' && params == "2") {
          for (auto& row : grid)
            std::fill(row.begin(), row.end(), ' ');
        } else if (final != 'm') {
          throw std::runtime_error("unexpected ASCII CSI");
        }
      } else {
        const auto byte = static_cast<unsigned char>(bytes[i++]);
        if (byte < 32 || byte >= 127) throw std::runtime_error("non-ASCII");
        if (x >= 0 && x < cols && y >= 0 && y < rows)
          grid[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] =
              static_cast<char>(byte);
        ++x;
      }
    }
  }
  std::string result;
  for (const auto& row : grid)
    result += row + '\n';
  return result;
}

class RecoveryReader final : public SystemReader {
 public:
  explicit RecoveryReader(bool first_fails) : first_fails(first_fails) {}
  auto sample() -> std::expected<SystemSnapshot, ErrorEvent> override {
    const int call = calls++;
    if ((first_fails && call == 0) || call == 2)
      return std::unexpected(
          ErrorEvent{Severity::Warning, "fixture", "unavailable\033[2J"});
    return fake->sample();
  }
  int calls{0};

 private:
  bool first_fails;
  std::unique_ptr<SystemReader> fake = make_fake_reader();
};
} // namespace

TEST_CASE("ForgeTop approved layouts match actual-frame capture fixtures",
          "[forge-top][ux][captures]") {
  for (const auto size :
       {Extent{24, 8}, Extent{40, 16}, Extent{80, 24}, Extent{120, 32}}) {
    for (int tier = 0; tier < 3; ++tier) {
      CAPTURE(size.w, size.h, tier);
      Journey journey;
      journey.drive(1, size.w, size.h,
                    tier == 0   ? DriverChoice::Fallback
                    : tier == 1 ? DriverChoice::AnsiRgb
                                : DriverChoice::Kitty);
      const auto filename =
          std::format("{}/after-tier{}-{}x{}.txt", FORGE_TOP_CAPTURES, tier,
                      size.w, size.h);
      std::ifstream fixture(filename, std::ios::binary);
      REQUIRE(fixture.good());
      const std::string expected{std::istreambuf_iterator<char>{fixture}, {}};
      CHECK(journey.screens.front() == expected);
      const auto& panel = journey.app.process_panel_for_test();
      const int expected_rows = size.w == 24   ? 2
                                : size.w == 40 ? 10
                                : size.w == 80 ? 7
                                               : 24;
      CHECK(panel.table().rect().h - 1 == expected_rows);
      CHECK(panel.filter().rect().w > 0);
      CHECK(journey.app.layout_for_test() == (size.w < 60     ? "compact"
                                              : size.w >= 110 ? "wide"
                                                              : "normal"));
      CHECK(journey.screens.front().find("DEMO") != std::string::npos);
      if (tier == 0)
        CHECK(std::ranges::all_of(journey.screens.front(), [](unsigned char c) {
          return c == '\n' || (c >= 32 && c < 127);
        }));
    }
  }
}

TEST_CASE("ForgeTop decoded presets retain filter selection and expose focus",
          "[forge-top][ux][input]") {
  for (const auto size : {Extent{24, 8}, Extent{80, 24}, Extent{120, 32}}) {
    Journey journey;
    std::optional<ProcessIdentity> selected;
    journey.next = [&](int frame) {
      auto& panel = journey.app.process_panel_for_test();
      if (frame == 0) {
        panel.table().set_selected(3);
        selected = panel.visible_rows().at(3).identity();
        journey.keys(f2);
      }
      if (frame == 1) journey.keys(f2);
      if (frame == 2) {
        CHECK(panel.filter().rect().empty());
        CHECK(panel.table().rect().empty());
        journey.keys(f2 + f3);
      }
      if (frame == 3) {
        REQUIRE(panel.table().selected() >= 0);
        CHECK(panel.visible_rows()
                  .at(static_cast<std::size_t>(panel.table().selected()))
                  .identity() == selected);
        journey.keys("/worker");
      }
      if (frame == 4) {
        CHECK(panel.filter().text() == "worker");
        CHECK(journey.screens.back().find("FILTER") != std::string::npos);
        journey.keys("\rN");
      }
      if (frame == 5) {
        CHECK(panel.sort_key() == ProcessSort::Pid);
        CHECK_FALSE(journey.app.detail_open_for_test());
        journey.keys("R");
      }
    };
    journey.drive(7, size.w, size.h);
    CHECK(journey.sections[1] ==
          std::array<bool, 4>{false, false, false, true});
    CHECK(journey.sections[2] == std::array<bool, 4>{true, true, true, false});
    CHECK(journey.app.summary_balance_for_test() == 1);
    CHECK(journey.statuses.back().starts_with("Sort: PID"));
  }
}

TEST_CASE("ForgeTop filter literals paste refusal and escape are unambiguous",
          "[forge-top][ux][failure][input]") {
  Journey journey;
  journey.next = [&](int frame) {
    if (frame == 0) journey.keys("/qPMNTRltm1cds");
    if (frame == 1) {
      CHECK(journey.app.process_panel_for_test().filter().text() ==
            "qPMNTRltm1cds");
      CHECK(journey.app.running());
      journey.keys("\033[200~injected\033[201~");
    }
    if (frame == 2) {
      CHECK(journey.statuses.back().find("paste unsupported") !=
            std::string::npos);
      CHECK(journey.app.process_panel_for_test().filter().text() ==
            "qPMNTRltm1cds");
      journey.keys("\033[27u");
    }
  };
  journey.drive(4);
  CHECK(journey.screens.back().find("TABLE") != std::string::npos);
  CHECK(journey.app.section_state_for_test() ==
        std::array<bool, 4>{true, true, true, true});
  CHECK(journey.app.process_panel_for_test().sort_key() == ProcessSort::Cpu);
}

TEST_CASE("ForgeTop decoded mouse uses current headers menus and filters",
          "[forge-top][ux][mouse]") {
  Journey journey;
  journey.next = [&](int frame) {
    const auto& panel = journey.app.process_panel_for_test();
    if (frame == 0)
      journey.keys(click(panel.table().rect().x + panel.table().gutter_cols(),
                         panel.table().rect().y));
    if (frame == 1) {
      CHECK(panel.sort_key() == ProcessSort::Pid);
      journey.keys(click(2, 0));
    }
    if (frame == 2) journey.keys(click(2, 2)); // Sort menu memory row
    if (frame == 3) {
      CHECK(panel.sort_key() == ProcessSort::Memory);
      CHECK(journey.statuses.back().starts_with("Sort: MEM"));
      journey.keys(click(panel.filter().rect().x + 1, panel.filter().rect().y) +
                   "1000");
    }
  };
  journey.drive(5);
  CHECK(journey.app.process_panel_for_test().filter().text() == "1000");
  CHECK(journey.screens.back().find("FILTER") != std::string::npos);
}

TEST_CASE(
    "ForgeTop emitted short help scrolls and delay controls remain visible",
    "[forge-top][ux][modal]") {
  for (const auto size : {Extent{24, 8}, Extent{80, 24}}) {
    Journey journey;
    journey.next = [&](int frame) {
      if (frame == 0) journey.keys(f1);
      if (frame == 1) journey.keys(end_key);
      if (frame == 2) journey.keys(home_key);
      if (frame == 3) journey.keys("qd");
      if (frame == 4)
        journey.keys("\x7f"
                     "0.25\r");
    };
    journey.drive(6, size.w, size.h);
    CHECK(emitted(journey, 1, size.w, size.h).find("Esc/q/h/?/F1") !=
          std::string::npos);
    CHECK(emitted(journey, 2, size.w, size.h).find("End of help.") !=
          std::string::npos);
    CHECK(emitted(journey, 3, size.w, size.h).find("Esc/q/h/?/F1") !=
          std::string::npos);
    const auto prompt = emitted(journey, 4, size.w, size.h);
    CHECK(prompt.find("OK") != std::string::npos);
    CHECK(prompt.find("Cancel") != std::string::npos);
    CHECK(journey.app.sample_delay_for_test().count() == 0.25);
    CHECK(journey.statuses.back().starts_with("Sampling delay set"));
  }
}

TEST_CASE("ForgeTop initial failure does not invent facts and recovery "
          "preserves actions",
          "[forge-top][ux][failure]") {
  Journey journey{std::make_unique<RecoveryReader>(true)};
  std::string last_rows;
  journey.next = [&](int frame) {
    if (frame == 0) {
      CHECK(journey.screens.back().find("No successful sample") !=
            std::string::npos);
      CHECK(journey.screens.back().find("Swap disabled") == std::string::npos);
      CHECK(journey.errors.back().find('\033') == std::string::npos);
      journey.keys(" N");
    }
    if (frame == 1) {
      CHECK(journey.errors.back().empty());
      CHECK(journey.statuses.back().starts_with("Sort: PID"));
      last_rows =
          journey.app.process_panel_for_test().visible_rows().front().name;
      journey.keys(" ");
    }
    if (frame == 2) {
      CHECK_FALSE(journey.errors.back().empty());
      CHECK(journey.screens.back().find("STALE:") != std::string::npos);
      CHECK(journey.app.process_panel_for_test().visible_rows().front().name ==
            last_rows);
      journey.keys(" ");
    }
  };
  journey.drive(4);
  CHECK(journey.errors.back().empty());
  CHECK(journey.statuses.back().starts_with("Sort: PID"));
  CHECK(journey.screens.back().find("Recovered sample") != std::string::npos);
}

TEST_CASE("ForgeTop resize cancels hidden forms before posted input",
          "[forge-top][ux][failure][resize]") {
  Journey journey;
  int selected = -1;
  journey.next = [&](int frame) {
    if (frame == 0) {
      selected = journey.app.process_panel_for_test().table().selected();
      journey.keys("d");
    }
    if (frame == 1) {
      REQUIRE(journey.app.set_size({12, 6}));
      journey.app.post(KeyEvent{.key = Key::Enter});
      journey.app.post(
          MouseEvent{.x = 5, .y = 15, .button = 0, .pressed = true});
    }
    if (frame == 2) {
      CHECK(journey.app.process_panel_for_test().filter().rect().empty());
      CHECK(journey.app.process_panel_for_test().table().rect().empty());
      CHECK(journey.app.process_panel_for_test().table().selected() ==
            selected);
      CHECK(journey.app.sample_delay_for_test().count() == 1.0);
      CHECK_FALSE(journey.app.detail_open_for_test());
      REQUIRE(journey.app.set_size({80, 24}));
    }
  };
  journey.drive(4);
  CHECK(journey.screens[2].find("Resize >=24") != std::string::npos);
  CHECK(journey.statuses.back().find("cancelled") != std::string::npos);
  CHECK_FALSE(journey.app.process_panel_for_test().table().rect().empty());
}

TEST_CASE(
    "ForgeTop clean graphs suspend for modals and retire for menu and view",
    "[forge-top][ux][persistent]") {
  Journey journey;
  journey.next = [&](int frame) {
    if (frame == 1) journey.keys(f1);
    if (frame == 2) journey.keys("q");
    if (frame == 3) journey.keys(click(2, 0));
    if (frame == 4) journey.keys("q");
    if (frame == 5) journey.keys(f2);
    if (frame == 6) journey.keys(f2 + f2);
  };
  journey.drive(9, 120, 32, DriverChoice::Kitty);
  CHECK(image_actions(journey.frames[0], "t") == 20);
  CHECK(image_actions(journey.frames[1], "t") == 0);
  CHECK(image_actions(journey.frames[1], "f") == 0);
  CHECK(image_actions(journey.frames[2], "t") == 0);
  CHECK(journey.frames[2].find("d=I") == std::string::npos);
  CHECK(image_actions(journey.frames[3], "t") == 0);
  CHECK(journey.frames[4].find("d=I") != std::string::npos);
  CHECK(image_actions(journey.frames[4], "t") == 0);
  CHECK(image_actions(journey.frames[5], "t") == 20);
  CHECK(journey.frames[6].find("d=I") != std::string::npos);
  CHECK(image_actions(journey.frames[7], "t") == 20);
  CHECK(image_actions(journey.frames[8], "t") == 0);
}

TEST_CASE("ForgeTop control C escapes an open menu", "[forge-top][ux][input]") {
  Journey journey;
  journey.next = [&](int frame) {
    if (frame == 0) journey.keys(click(2, 0));
    if (frame == 1) journey.keys("\003");
  };
  journey.drive(5);
  CHECK(journey.frames.size() == 2);
}

TEST_CASE(
    "ForgeTop decoded detail opens by keyboard and mouse without rebinding",
    "[forge-top][ux][modal][mouse]") {
  Journey journey;
  std::optional<ProcessIdentity> identity;
  journey.next = [&](int frame) {
    auto& panel = journey.app.process_panel_for_test();
    if (frame == 0) {
      identity = panel.visible_rows().front().identity();
      journey.keys("\033[B\r");
    }
    if (frame == 1) {
      CHECK(journey.app.detail_open_for_test());
      CHECK(journey.app.detail_identity_for_test() == identity);
      journey.keys("q");
    }
    if (frame == 2) {
      CHECK_FALSE(journey.app.detail_open_for_test());
      journey.keys(
          click(panel.table().rect().x + 3, panel.table().rect().y + 1));
    }
    if (frame == 3) {
      CHECK(journey.app.detail_open_for_test());
      CHECK(journey.app.detail_identity_for_test() == identity);
      journey.keys("\033[27u");
    }
  };
  journey.drive(5);
  REQUIRE(identity);
  CHECK(emitted(journey, 1, 80, 24).find(std::format("({})", identity->pid)) !=
        std::string::npos);
  CHECK_FALSE(journey.app.detail_open_for_test());
  CHECK(journey.app.running());
}

TEST_CASE("ForgeTop subfloor help remains reachable and pixel resize retires "
          "old regions",
          "[forge-top][ux][resize][persistent]") {
  Journey tiny;
  tiny.next = [&](int frame) {
    if (frame == 0) tiny.keys(f1);
    if (frame == 1) tiny.keys(end_key);
    if (frame == 2) tiny.keys("q");
  };
  tiny.drive(4, 12, 6);
  CHECK(emitted(tiny, 2, 12, 6).find("returns.") != std::string::npos);
  CHECK(tiny.screens.back().find("Resize >=24") != std::string::npos);

  Journey pixels;
  pixels.next = [&](int frame) {
    if (frame == 0) REQUIRE(pixels.app.set_size({24, 8}));
    if (frame == 1) REQUIRE(pixels.app.set_size({120, 32}));
  };
  pixels.drive(4, 120, 32, DriverChoice::Kitty);
  CHECK(image_actions(pixels.frames[0], "t") == 20);
  CHECK(pixels.frames[1].find("d=I") != std::string::npos);
  CHECK(image_actions(pixels.frames[1], "t") == 0);
  CHECK(image_actions(pixels.frames[2], "t") == 20);
  CHECK(image_actions(pixels.frames[3], "t") == 0);
}
