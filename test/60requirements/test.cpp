// TermForge — AppRequirements (#91): a declared floor, evaluated before
// enter_screen(), with a minimal runtime resize half.
//
// Pure evaluate_requirements cases need no terminal. The setup refusal case
// uses a silent socket + pushed capabilities (same fixture shape as
// test/45identity) so the probe never hangs. The runtime case drives
// test_run_frames after set_size, the same way test/44size observes a push.

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <expected>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "detail/requirements.hpp"
#include "termforge/core/app.hpp"
#include "termforge/core/requirements.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/core/terminal.hpp"
#include "termforge/core/types.hpp"
#include "termforge/drivers/kitty_driver.hpp"
#include "termforge/widgets/widget.hpp"

using namespace termforge;
using detail::AppRequirementFacts;
using detail::evaluate_requirements;
using detail::make_requirement_facts;

namespace {

class SocketPair {
 public:
  SocketPair() {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, m_fd) != 0) return;
    for (const int fd : m_fd) {
      const int fl = ::fcntl(fd, F_GETFL);
      if (fl >= 0) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    m_ok = true;
  }
  ~SocketPair() {
    for (int& fd : m_fd) {
      if (fd >= 0) {
        ::close(fd);
        fd = -1;
      }
    }
  }
  SocketPair(const SocketPair&) = delete;
  auto operator=(const SocketPair&) -> SocketPair& = delete;

  [[nodiscard]] auto ok() const -> bool { return m_ok; }
  [[nodiscard]] auto app() const -> int { return m_fd[0]; }
  auto drain_peer() -> std::string {
    std::string out;
    char buf[256];
    for (;;) {
      const auto n = ::read(m_fd[1], buf, sizeof(buf));
      if (n <= 0) break;
      out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
  }

 private:
  int m_fd[2]{-1, -1};
  bool m_ok{false};
};

class FloorApp : public App {
 public:
  std::vector<ErrorEvent> errors;
  std::vector<ResizeEvent> resizes;
  int renders{0};

  auto inject(TerminalIo io) -> bool {
    return terminal().set_io(io).has_value();
  }
  auto push_caps(Capabilities caps) -> bool {
    return terminal().set_capabilities(std::move(caps)).has_value();
  }

 protected:
  auto on_render(Screen&) -> void override { ++renders; }
  auto on_event(const Event& ev) -> void override {
    if (const auto* e = std::get_if<ErrorEvent>(&ev)) {
      errors.push_back(*e);
      return;
    }
    if (const auto* r = std::get_if<ResizeEvent>(&ev)) {
      resizes.push_back(*r);
      return;
    }
    App::on_event(ev);
  }
};

struct Plate : Widget {
  Rect region{0, 0, 2, 1};
  Image cache = Image{
      2, 1,
      std::vector<Pixel>{Pixel{255, 0, 0, 255}, Pixel{0, 255, 0, 255}}};

  auto draw(Screen& screen) -> void override {
    for (int y = region.y; y < region.y + region.h; ++y)
      for (int x = region.x; x < region.x + region.w; ++x)
        screen.at(x, y).text = "#";
  }
  auto pixel_regions() -> std::vector<Rect> override { return {region}; }
  [[nodiscard]] auto pixel_region_state(Rect) const noexcept
      -> PixelRegionState override {
    return {.mode = PixelRegionMode::Persistent};
  }
  auto draw_pixels(Rect, Extent) -> const Image* override {
    ++pixel_calls;
    return &cache;
  }
  auto pixel_region_submitted(Rect) noexcept -> void override {
    ++submissions;
  }

  int pixel_calls{0};
  int submissions{0};
};

}  // namespace

// ── pure evaluate ───────────────────────────────────────────────────────────

TEST_CASE("evaluate_requirements: empty requirements always pass",
          "[requirements]") {
  REQUIRE(evaluate_requirements(AppRequirements{}, AppRequirementFacts{})
              .has_value());
  AppRequirementFacts bare;
  bare.cols = 1;
  bare.rows = 1;
  REQUIRE(evaluate_requirements(AppRequirements{}, bare).has_value());
}

TEST_CASE("evaluate_requirements: graphics satisfied by kitty or sixel",
          "[requirements]") {
  AppRequirements req{.graphics = true};
  AppRequirementFacts none;
  auto miss = evaluate_requirements(req, none);
  REQUIRE_FALSE(miss.has_value());
  REQUIRE(miss.error().severity == Severity::Error);
  REQUIRE(miss.error().source == "requirements");
  REQUIRE(miss.error().message.find("graphics") != std::string::npos);

  AppRequirementFacts kitty;
  kitty.driver_caps.kitty_graphics = true;
  REQUIRE(evaluate_requirements(req, kitty).has_value());

  // Reported support is not reachability: until a SixelDriver is selected,
  // App has no graphics route and must refuse rather than silently pass.
  AppRequirementFacts reported_sixel;
  reported_sixel.terminal_caps.sixel = true;
  REQUIRE_FALSE(evaluate_requirements(req, reported_sixel).has_value());

  AppRequirementFacts selected_sixel = reported_sixel;
  selected_sixel.driver_caps.sixel = true;
  REQUIRE(evaluate_requirements(req, selected_sixel).has_value());
}

TEST_CASE("evaluate_requirements: truecolor satisfied by KittyDriver tier",
          "[requirements]") {
  // Truecolor is a property of the route App selected, not an environment
  // hint. KittyDriver and AnsiRgbDriver both self-report it.
  AppRequirements req{.truecolor = true};
  AppRequirementFacts kitty;
  kitty.driver_caps.kitty_graphics = true;
  kitty.driver_caps.truecolor = true;
  REQUIRE(evaluate_requirements(req, kitty).has_value());

  AppRequirementFacts ansi;
  ansi.driver_caps.truecolor = true;
  REQUIRE(evaluate_requirements(req, ansi).has_value());

  AppRequirementFacts reported_only;
  reported_only.terminal_caps.truecolor = true;
  REQUIRE_FALSE(evaluate_requirements(req, reported_only).has_value());

  REQUIRE_FALSE(evaluate_requirements(req, AppRequirementFacts{}).has_value());
}

TEST_CASE("evaluate_requirements: key actions need support and Enhanced mode",
          "[requirements]") {
  AppRequirements repeat{.key_repeat = true};
  AppRequirements release{.key_release = true};
  AppRequirements press{.key_press = true};

  REQUIRE(evaluate_requirements(press, AppRequirementFacts{}).has_value());
  REQUIRE_FALSE(evaluate_requirements(repeat, AppRequirementFacts{}).has_value());
  REQUIRE_FALSE(
      evaluate_requirements(release, AppRequirementFacts{}).has_value());

  AppRequirementFacts legacy;
  legacy.terminal_caps.kitty_keyboard = true;
  REQUIRE_FALSE(evaluate_requirements(repeat, legacy).has_value());
  REQUIRE(legacy.keyboard_mode == KeyboardMode::Legacy);

  AppRequirementFacts disambiguate = legacy;
  disambiguate.keyboard_mode = KeyboardMode::Disambiguate;
  REQUIRE_FALSE(evaluate_requirements(release, disambiguate).has_value());

  AppRequirementFacts unsupported;
  unsupported.keyboard_mode = KeyboardMode::Enhanced;
  REQUIRE_FALSE(evaluate_requirements(repeat, unsupported).has_value());

  AppRequirementFacts enhanced = unsupported;
  enhanced.terminal_caps.kitty_keyboard = true;
  REQUIRE(evaluate_requirements(repeat, enhanced).has_value());
  REQUIRE(evaluate_requirements(release, enhanced).has_value());
}

TEST_CASE("evaluate_requirements: unknown geometry fails only when required",
          "[requirements]") {
  AppRequirementFacts unknown;
  unknown.cols = 80;
  unknown.rows = 24;
  // No pixel pair → unknown. Empty geometry requirements still pass.
  REQUIRE(evaluate_requirements(AppRequirements{}, unknown).has_value());
  REQUIRE(evaluate_requirements(AppRequirements{.min_cols = 40}, unknown)
              .has_value());

  AppRequirements need_known{.known_cell_pixels = true};
  auto miss = evaluate_requirements(need_known, unknown);
  REQUIRE_FALSE(miss.has_value());
  REQUIRE(miss.error().message.find("known cell-pixel") != std::string::npos);

  AppRequirements need_min{.min_cell_pixels = Extent{6, 12}};
  REQUIRE_FALSE(evaluate_requirements(need_min, unknown).has_value());

  AppRequirementFacts known = make_requirement_facts(
      {}, {}, KeyboardMode::Legacy, 80, 24, 800, 480);
  REQUIRE(known.cell_pixels_known);
  REQUIRE(known.cell_pixels == Extent{10, 20});
  REQUIRE(evaluate_requirements(need_known, known).has_value());
  REQUIRE(evaluate_requirements(need_min, known).has_value());

  AppRequirements tall{.min_cell_pixels = Extent{6, 30}};
  REQUIRE_FALSE(evaluate_requirements(tall, known).has_value());
}

TEST_CASE("evaluate_requirements: min grid floor", "[requirements]") {
  AppRequirements req{.min_cols = 120, .min_rows = 40};
  AppRequirementFacts small{.cols = 100, .rows = 50};
  REQUIRE_FALSE(evaluate_requirements(req, small).has_value());

  AppRequirementFacts ok{.cols = 120, .rows = 40};
  REQUIRE(evaluate_requirements(req, ok).has_value());
}

// ── setup before enter_screen ───────────────────────────────────────────────

TEST_CASE("App::setup: unmet requirements refuse before alt-screen",
          "[requirements][app]") {
  SocketPair sp;
  REQUIRE(sp.ok());

  FloorApp app;
  REQUIRE(app.inject(TerminalIo{sp.app(), sp.app()}));
  REQUIRE(app.push_caps(Capabilities{}));  // silent socket, no graphics
  app.require(AppRequirements{.graphics = true});
  REQUIRE(app.set_size({80, 24}).has_value());

  auto r = app.test_setup();
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().severity == Severity::Error);
  REQUIRE(r.error().source == "requirements");
  REQUIRE_FALSE(app.requirements_met());
  // enter_screen never ran: no winch hook, no alt-screen enter sequence.
  REQUIRE_FALSE(app.test_winch_hooked());
  const std::string wrote = sp.drain_peer();
  REQUIRE(wrote.find("\033[?1049h") == std::string::npos);

  app.test_teardown();  // unwind raw mode explicitly
}

TEST_CASE("App::setup: reported sixel cannot satisfy an unimplemented route",
          "[requirements][app]") {
  SocketPair sp;
  REQUIRE(sp.ok());

  FloorApp app;
  REQUIRE(app.inject(TerminalIo{sp.app(), sp.app()}));
  Capabilities caps;
  caps.sixel = true;
  REQUIRE(app.push_caps(caps));
  app.require(AppRequirements{.graphics = true});
  REQUIRE(app.set_size({80, 24}).has_value());

  auto r = app.test_setup();
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().message.find("selected driver") != std::string::npos);
  REQUIRE(r.error().message.find("sixel=true") != std::string::npos);
  REQUIRE_FALSE(app.test_winch_hooked());
  REQUIRE(sp.drain_peer().find("\033[?1049h") == std::string::npos);
  app.test_teardown();
}

TEST_CASE("App::setup: fatal keyboard floor does not queue fallback Info",
          "[requirements][app][keyboard]") {
  SocketPair sp;
  REQUIRE(sp.ok());

  FloorApp app;
  REQUIRE(app.inject(TerminalIo{sp.app(), sp.app()}));
  REQUIRE(app.push_caps(Capabilities{}));
  app.set_keyboard_mode(KeyboardMode::Enhanced);
  app.require(AppRequirements{.key_release = true});
  REQUIRE(app.set_size({80, 24}).has_value());

  auto r = app.test_setup();
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().severity == Severity::Error);
  REQUIRE(r.error().message.find("keyboard-flags") != std::string::npos);
  app.test_pump({});
  REQUIRE(app.errors.empty());
  app.test_teardown();
}

TEST_CASE("App::setup: satisfied requirements still enter the screen",
          "[requirements][app]") {
  SocketPair sp;
  REQUIRE(sp.ok());

  FloorApp app;
  REQUIRE(app.inject(TerminalIo{sp.app(), sp.app()}));
  Capabilities caps;
  caps.kitty_graphics = true;
  caps.truecolor = true;
  REQUIRE(app.push_caps(caps));
  app.require(AppRequirements{.graphics = true, .truecolor = true});
  REQUIRE(app.set_size({120, 40, 1200, 800}).has_value());

  REQUIRE(app.test_setup().has_value());
  REQUIRE(app.requirements_met());
  REQUIRE(app.test_winch_hooked());
  app.test_teardown();
}

TEST_CASE("App: live keyboard mode changes re-evaluate the floor",
          "[requirements][app][keyboard][runtime]") {
  SocketPair sp;
  REQUIRE(sp.ok());

  FloorApp app;
  REQUIRE(app.inject(TerminalIo{sp.app(), sp.app()}));
  Capabilities caps;
  caps.kitty_keyboard = true;
  REQUIRE(app.push_caps(caps));
  app.set_keyboard_mode(KeyboardMode::Enhanced);
  app.require(AppRequirements{.key_release = true});
  REQUIRE(app.set_size({80, 24}).has_value());
  REQUIRE(app.test_setup().has_value());
  REQUIRE(app.requirements_met());

  app.set_keyboard_mode(KeyboardMode::Legacy);
  REQUIRE_FALSE(app.requirements_met());
  app.test_pump({});
  REQUIRE(app.errors.size() == 1);
  REQUIRE(app.errors[0].severity == Severity::Warning);
  REQUIRE(app.errors[0].message.find("Enhanced") != std::string::npos);

  app.set_keyboard_mode(KeyboardMode::Enhanced);
  REQUIRE(app.requirements_met());
  app.test_pump({});
  REQUIRE(app.errors.size() == 2);
  REQUIRE(app.errors[1].severity == Severity::Info);
  app.test_teardown();
}

// ── runtime resize half ─────────────────────────────────────────────────────

TEST_CASE("App: one production cadence gates and restores enhanced submission",
          "[requirements][app][runtime][pixels]") {
  class PixelApp final : public App {
   public:
    Plate plate;
    std::vector<ErrorEvent> errors;
    std::vector<ResizeEvent> resizes;
    std::vector<FrameBytes> meters;
    int direct_pixel_calls{0};
    bool resize_push_ok{true};

    auto on_render(Screen& screen) -> void override {
      screen.clear();
      plate.draw(screen);
      render_pixel_regions(plate);
    }
    auto on_pixels(TerminalDriver&) -> void override { ++direct_pixel_calls; }
    auto on_event(const Event& ev) -> void override {
      if (const auto* e = std::get_if<ErrorEvent>(&ev)) {
        errors.push_back(*e);
        return;
      }
      if (const auto* r = std::get_if<ResizeEvent>(&ev)) {
        resizes.push_back(*r);
        // Four frames over ONE driver: above, below, still below, restored.
        // The repeated 40x40 push proves a stable false state emits no second
        // Warning; the same driver/persistent map proves restoration uploads.
        if (resizes.size() == 1)
          resize_push_ok &= set_size({40, 40}).has_value();
        if (resizes.size() == 2)
          resize_push_ok &= set_size({40, 40}).has_value();
        if (resizes.size() == 3)
          resize_push_ok &= set_size({110, 40}).has_value();
        return;
      }
      App::on_event(ev);
    }

   protected:
    [[nodiscard]] auto now_steady() const
        -> std::chrono::steady_clock::time_point override {
      return now;
    }
    auto wait_readable(int timeout_ms) -> bool override {
      // Called after the frame's sole flush and before test_run_frames' final
      // shutdown write, so each sample is exactly one production frame.
      meters.push_back(driver().last_frame_bytes());
      now += std::chrono::milliseconds(timeout_ms);
      return false;
    }
    auto read_available(char*, int) -> int override { return 0; }

   private:
    std::chrono::steady_clock::time_point now{};
  };

  PixelApp app;
  app.require(AppRequirements{.min_cols = 100});
  REQUIRE(app.set_size({120, 40}).has_value());

  std::string sink;
  app.test_run_frames(4, 7, 7, &sink, std::make_unique<KittyDriver>());
  REQUIRE(app.requirements_met());
  REQUIRE(app.resize_push_ok);
  REQUIRE(app.resizes.size() == 4);
  REQUIRE(app.errors.size() == 2);
  REQUIRE(app.errors[0].severity == Severity::Warning);
  REQUIRE(app.errors[1].severity == Severity::Info);
  REQUIRE(app.errors[1].message.find("met again") != std::string::npos);

  REQUIRE(app.meters.size() == 4);
  REQUIRE(app.meters[0].image_transmit > 0);
  REQUIRE(app.meters[1].image_transmit == 0);
  REQUIRE(app.meters[2].image_transmit == 0);
  REQUIRE(app.meters[3].image_transmit > 0);
  REQUIRE(app.meters[1].cells > 0);  // authored Baseline was still painted
  REQUIRE(app.plate.pixel_calls == 2);
  REQUIRE(app.plate.submissions == 2);
  REQUIRE(app.direct_pixel_calls == 2);
}
