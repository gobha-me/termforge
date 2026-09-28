// Real App frame tests, including accepted-write and overlay paths. No live
// tty.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "support/apc.hpp"
#include "termforge/core/app.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"
#include "termforge/widgets/focus_ring.hpp"
#include "termforge/widgets/notebook.hpp"
#include "termforge/widgets/pixel_surface.hpp"
#include "termforge/widgets/text_box.hpp"
#include "termforge/widgets/text_input.hpp"

using namespace termforge;

namespace {
struct Page final : Widget {
  TextInput input;
  PixelSurface pixels{{2, 2}, Pixel{180, 60, 30, 255}};
  bool image{false};
  bool duplicate_children{false};
  int draws{0}, events{0}, ticks{0}, resets{0};
  auto draw(Screen& screen) -> void override {
    ++draws;
    input.set_geometry(rect());
    pixels.set_geometry(rect());
    if (image)
      pixels.draw(screen);
    else
      input.draw(screen);
  }
  auto on_event(const Event& event) -> bool override {
    ++events;
    if (const auto* paste = std::get_if<PasteEvent>(&event)) {
      input.set_text(input.text() + paste->text);
      return true;
    }
    return input.on_event(event);
  }
  auto on_tick(std::chrono::duration<double>) -> void override { ++ticks; }
  auto reset_transient() -> void override { ++resets; }
  auto set_focused(bool focus) -> void override {
    Widget::set_focused(focus);
    input.set_focused(focus);
  }
  auto pixel_children() -> std::vector<Widget*> override {
    if (duplicate_children) return {&pixels, &pixels, this, nullptr};
    return image ? std::vector<Widget*>{&pixels} : std::vector<Widget*>{};
  }
};

struct Overlay final : Widget {
  auto draw(Screen& screen) -> void override {
    set_geometry({0, 0, screen.cols(), screen.rows()});
    screen.fill_rect(0, 0, screen.cols(), screen.rows(), Rgb{}, Rgb{});
  }
};

struct OpaquePage final : Widget {
  // Deliberately opaque bytes: the library cannot parse PNG. The synthetic
  // terminal reply below exercises acknowledgement correlation, not a codec.
  std::array<std::byte, 3> bytes{std::byte{1}, std::byte{2}, std::byte{3}};
  EncodedImage encoded{ImageFormat::Png, bytes, {2, 2}};
  int submissions{0};
  bool content_dirty{true};
  auto draw(Screen& screen) -> void override {
    const Rect r = rect();
    screen.fill_rect(r.x, r.y, r.w, r.h, Rgb{}, Rgb{});
    screen.write_text(r.x, r.y, "opaque fallback", Rgb{}, Rgb{});
  }
  auto pixel_regions() -> std::vector<Rect> override { return {rect()}; }
  auto draw_encoded_pixels(Rect) -> const EncodedImage* override {
    return &encoded;
  }
  auto pixel_region_state(Rect) const noexcept -> PixelRegionState override {
    return {PixelRegionMode::Persistent, content_dirty, 1};
  }
  auto pixel_region_submitted(Rect, std::uint64_t) noexcept -> void override {
    ++submissions;
    content_dirty = false;
  }
};

struct Sink final : ByteSink {
  bool reject{false};
  std::vector<std::string> writes;
  auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override {
    writes.emplace_back(bytes.begin(), bytes.end());
    if (reject)
      return std::unexpected(ErrorEvent{Severity::Warning, "sink", "refused"});
    return {};
  }
};

class NotebookApp final : public App {
 public:
  Page first, second;
  Notebook book;
  Overlay overlay;
  Sink sink;
  int frame{0};
  std::function<void(int)> before;
  std::vector<std::string> input_frames;
  std::vector<ErrorEvent> errors;
  NotebookApp() {
    first.image = second.image = true;
    second.pixels.image().fill({0, 0, 2, 2}, Pixel{20, 60, 200, 255});
    (void)book.add_page("First", &first);
    (void)book.add_page("Second", &second);
    book.set_focused(true);
  }
  auto run(int frames, int tier) -> void {
    std::unique_ptr<TerminalDriver> selected;
    if (tier == 0)
      selected = std::make_unique<FallbackDriver>();
    else if (tier == 1)
      selected = std::make_unique<AnsiRgbDriver>();
    else
      selected = std::make_unique<KittyDriver>();
    test_run_frames(frames, 20, 6, &unused, std::move(selected));
  }
  auto show_overlay() -> void { push_overlay(overlay); }
  auto hide_overlay() -> void { pop_overlay(); }

 protected:
  auto on_render(Screen& screen) -> void override {
    driver().set_output(&sink);
    if (before) before(frame);
    ++frame;
    screen.clear();
    book.set_geometry({0, 0, screen.cols(), screen.rows()});
    book.draw(screen);
    render_pixel_regions(book);
  }
  auto on_event(const Event& event) -> void override {
    if (const auto* e = std::get_if<ErrorEvent>(&event))
      errors.push_back(*e);
    else
      (void)book.on_event(event);
  }
  auto on_tick(std::chrono::duration<double> dt) -> void override {
    book.on_tick(dt);
  }
  auto now_steady() const -> std::chrono::steady_clock::time_point override {
    return now;
  }
  auto wait_readable(int ms) -> bool override {
    now += std::chrono::milliseconds(ms);
    return false;
  }
  auto read_available(char* out, int max) -> int override {
    if (read_frame != frame) {
      read_frame = frame;
      incoming = frame < static_cast<int>(input_frames.size())
                     ? input_frames[static_cast<std::size_t>(frame)]
                     : "";
    }
    const int n = std::min(max, static_cast<int>(incoming.size()));
    std::copy_n(incoming.begin(), n, out);
    incoming.erase(0, static_cast<std::size_t>(n));
    return n;
  }

 private:
  std::string unused;
  std::chrono::steady_clock::time_point now{};
  int read_frame{-1};
  std::string incoming;
};
} // namespace

TEST_CASE("Notebook preserves page content and isolates events/focus/ticks") {
  Page first, second;
  Notebook book;
  REQUIRE(book.add_page("First", &first));
  REQUIRE(book.add_page("Second", &second));
  CHECK_FALSE(book.add_page("Duplicate", &first));
  CHECK_FALSE(book.add_page("Null", nullptr));
  CHECK_FALSE(book.add_page("Self", &book));
  Screen screen(24, 6);
  book.set_geometry({1, 1, 20, 4});
  book.set_focused(true);
  book.draw(screen);
  CHECK(book.content_rect() == Rect{1, 2, 20, 3});
  REQUIRE(book.on_event(KeyEvent{.key = Key::Tab}));
  CHECK(first.input.focused());
  REQUIRE(book.on_event(PasteEvent{"kept"}));
  book.on_tick(std::chrono::duration<double>{0.1});
  CHECK(first.ticks == 1);
  CHECK(second.ticks == 0);
  REQUIRE(book.on_event(KeyEvent{.key = Key::Tab, .ctrl = true}));
  CHECK_FALSE(first.input.focused());
  CHECK(second.input.focused());
  REQUIRE(book.on_event(PasteEvent{"other"}));
  book.set_active(0);
  CHECK(first.input.text() == "kept");
  CHECK(second.input.text() == "other");
  CHECK(first.input.focused());
  book.set_focused(false);
  CHECK_FALSE(first.input.focused());
  book.set_focused(true);
  CHECK(first.input.focused());
  const int events = first.events;
  CHECK_FALSE(book.on_event(
      KeyEvent{.key = Key::Char, .ch = U'x', .action = KeyAction::Release}));
  CHECK(first.events == events);
  CHECK_FALSE(book.on_event(MouseEvent{.x = 23, .y = 5, .pressed = true}));
  CHECK(first.events == events);
}

TEST_CASE("Notebook removal, callback mutation, tiny and empty layouts") {
  Page a, b, c;
  Notebook book;
  CHECK_FALSE(book.focusable());
  REQUIRE(book.add_page("A", &a));
  REQUIRE(book.add_page("B", &b));
  REQUIRE(book.add_page("C", &c));
  int changes = 0;
  book.on_change([&](int) {
    ++changes;
    book.clear();
    book.on_change({});
  });
  book.set_active(1);
  CHECK(changes == 0);
  REQUIRE(book.remove_page(&a));
  CHECK(book.active_page() == &b);
  REQUIRE(book.remove_page(&b));
  CHECK(book.active_page() == &c);
  CHECK_FALSE(b.focused());
  REQUIRE(book.on_event(KeyEvent{.key = Key::Tab, .ctrl = true}));
  CHECK(changes == 0); // only one surviving page: no selection change
  REQUIRE(book.add_page("Again", &a));
  REQUIRE(book.on_event(KeyEvent{.key = Key::Tab, .ctrl = true}));
  CHECK(changes == 1);
  CHECK(book.active() == -1);
  CHECK_FALSE(book.remove_page(&a));
  Screen screen(5, 2);
  book.set_geometry({0, 0, 5, 2});
  book.draw(screen);
  CHECK_FALSE(book.on_event(PasteEvent{"hidden"}));
  REQUIRE(book.add_page("Long overflowed title", &a));
  REQUIRE(book.add_page("Last", &b));
  book.set_style(BorderStyle::Ascii);
  book.set_active(99);
  book.draw(screen);
  CHECK(book.active() == 1);
  CHECK(b.draws == 1);
  book.set_geometry({0, 0, 5, 1});
  book.draw(screen);
  CHECK(b.draws == 1);
  CHECK(book.pixel_children().empty());
  CHECK_FALSE(book.on_event(PasteEvent{"hidden"}));
  book.set_geometry({0, 0, 0, 0});
  book.draw(screen);
  CHECK(book.pixel_children().empty());
  book.clear();
  CHECK_FALSE(a.focused());
  CHECK_FALSE(b.focused());
  REQUIRE(book.add_page("A", &a));
  REQUIRE(book.add_page("B", &b));
  REQUIRE(book.add_page("C", &c));
  book.set_active(2);
  REQUIRE(book.remove_page(&c));
  CHECK(book.active_page() == &b);
  REQUIRE(book.remove_page(&b));
  CHECK(book.active_page() == &a);
  REQUIRE(book.remove_page(&a));
  CHECK(book.active() == -1);
  CHECK(changes == 1);
}

TEST_CASE("Notebook is not a nested forward-Tab focus trap") {
  Page page, sibling;
  Notebook book;
  REQUIRE(book.add_page("Page", &page));
  book.set_geometry({0, 0, 20, 4});
  FocusRing outer;
  outer.add(&book);
  outer.add(&sibling);
  REQUIRE(outer.handle_key(KeyEvent{.key = Key::Tab}));
  CHECK(page.focused());
  REQUIRE(outer.handle_key(KeyEvent{.key = Key::Tab}));
  CHECK(outer.current() == &sibling);
  CHECK_FALSE(page.focused());
  REQUIRE(outer.handle_key(KeyEvent{.key = Key::Tab, .shift = true}));
  CHECK(outer.current() == &book);
  CHECK(page.focused());
  REQUIRE(outer.handle_key(KeyEvent{.key = Key::Tab, .shift = true}));
  CHECK_FALSE(page.focused());
  REQUIRE(outer.handle_key(KeyEvent{.key = Key::Tab, .shift = true}));
  CHECK(outer.current() == &sibling);
}

TEST_CASE("Notebook passive or hidden content uses strip traversal") {
  struct PassivePage final : Widget {
    auto draw(Screen&) -> void override {}
    auto focusable() const -> bool override { return false; }
  } passive;
  Page page, sibling;
  Notebook book;
  REQUIRE(book.add_page("Interactive", &page));
  REQUIRE(book.add_page("Passive", &passive));
  book.set_geometry({0, 0, 20, 4});
  FocusRing outer;
  outer.add(&book);
  outer.add(&sibling);
  REQUIRE(outer.handle_key(KeyEvent{.key = Key::Tab}));
  REQUIRE(page.focused());
  SECTION("Selecting a passive page moves effective focus to the strip") {
    book.set_active(1);
    CHECK_FALSE(page.focused());
    CHECK_FALSE(passive.focused());
    REQUIRE(outer.handle_key(KeyEvent{.key = Key::Tab, .shift = true}));
    CHECK(outer.current() == &sibling);
  }
  SECTION("A zero-height page has no focus or input stop") {
    book.set_geometry({0, 0, 20, 1});
    Screen screen(20, 1);
    book.draw(screen);
    CHECK_FALSE(page.focused());
    const int events = page.events;
    REQUIRE(outer.handle_key(KeyEvent{.key = Key::Tab, .shift = true}));
    CHECK(outer.current() == &sibling);
    CHECK(page.events == events);
    book.set_geometry({0, 0, 20, 4});
    REQUIRE(outer.focus(&book));
    CHECK(page.focused()); // restore the remembered content location
  }
}

TEST_CASE(
    "Notebook real frames preserve leaf identities across same-Rect switches") {
  for (int tier = 0; tier < 3; ++tier) {
    CAPTURE(tier);
    NotebookApp app;
    app.before = [&](int frame) {
      if (frame == 1) app.book.set_active(1);
      if (frame == 2) app.book.set_active(0);
    };
    app.run(3, tier);
    CHECK(app.first.draws == 2);
    CHECK(app.second.draws == 1);
    CHECK(app.errors.empty());
    if (tier == 0) {
      CHECK(app.first.pixels.submission_count() == 0);
      CHECK(app.second.pixels.submission_count() == 0);
    } else {
      CHECK(app.first.pixels.submission_count() >= 1);
      CHECK(app.second.pixels.submission_count() == 1);
    }
    if (tier == 2) {
      REQUIRE(app.sink.writes.size() >= 3);
      CHECK(tfsupport::total_data_transmits(app.sink.writes[0]) == 1);
      CHECK(tfsupport::total_data_transmits(app.sink.writes[1]) == 1);
      bool retired = false;
      for (const auto& command : tfsupport::apcs(app.sink.writes[1]))
        if (tfsupport::key_value(command, "a") == "d") retired = true;
      CHECK(retired);
    }
  }
}

TEST_CASE("Notebook hidden and modal pages are never acknowledged; refused "
          "page retries") {
  NotebookApp app;
  app.before = [&](int frame) {
    if (frame == 1) {
      CHECK(app.first.pixels.submission_count() == 1);
      CHECK(app.second.pixels.submission_count() == 0);
      app.book.set_active(1);
      app.sink.reject = true;
    }
    if (frame == 2) {
      CHECK(app.second.pixels.submission_count() == 0);
      CHECK(app.second.pixels.content_dirty());
      app.sink.reject = false;
    }
    if (frame == 3) {
      CHECK(app.second.pixels.submission_count() == 1);
      app.first.pixels.invalidate();
      app.book.set_active(0);
      app.show_overlay();
    }
    if (frame == 4) {
      CHECK(app.first.pixels.submission_count() == 1);
      CHECK(app.first.pixels.content_dirty());
      app.hide_overlay();
    }
  };
  app.run(5, 2);
  CHECK(app.first.pixels.submission_count() == 2);
  CHECK(app.second.pixels.submission_count() == 1);
  REQUIRE(app.sink.writes.size() >= 5);
  CHECK(tfsupport::apcs(app.sink.writes[3]).size() >=
        1); // retirement, no upload
  CHECK(tfsupport::total_data_transmits(app.sink.writes[3]) == 0);
  CHECK_FALSE(app.errors.empty());
  const auto first_ids = tfsupport::ids_named(app.sink.writes[0]);
  REQUIRE(first_ids.size() == 1);
  // The refused page-switch write also refused retirement of the old root.
  // Cleanup must retry at an accepted boundary, not orphan resident data.
  CHECK(tfsupport::data_deletes_of(app.sink.writes[2], *first_ids.begin()) ==
        1);
}

TEST_CASE("Notebook receives decoded input through the real App modal gate") {
  NotebookApp app;
  app.first.image = app.second.image = false;
  app.input_frames = {"", "\tA", "\033[<0;11;1M", "\tB", "C", ""};
  app.before = [&](int frame) {
    if (frame == 3) app.show_overlay();
    if (frame == 4) app.hide_overlay();
  };
  app.run(6, 0);
  CHECK(app.first.input.text() == "A");
  CHECK(app.second.input.text() == "B");
  CHECK(app.book.active() == 1);
}

TEST_CASE("Removing the visible page retires it without acknowledging hidden "
          "content") {
  NotebookApp app;
  app.before = [&](int frame) {
    if (frame == 1) {
      app.first.pixels.invalidate();
      REQUIRE(app.book.remove_page(&app.first));
      CHECK(app.book.active_page() == &app.second);
    }
    if (frame == 2) app.book.clear();
  };
  app.run(3, 2);
  CHECK(app.first.pixels.submission_count() == 1);
  CHECK(app.first.pixels.content_dirty());
  CHECK(app.second.pixels.submission_count() == 1);
  CHECK(tfsupport::total_data_transmits(app.sink.writes[2]) == 0);
  CHECK(app.book.active_page() == nullptr);
  CHECK(app.errors.empty());
}

TEST_CASE("Pixel child traversal skips nulls, duplicates and cycles") {
  NotebookApp app;
  app.first.duplicate_children = true;
  app.run(2, 2);
  CHECK(app.first.pixels.submission_count() == 1);
  CHECK(app.second.pixels.submission_count() == 0);
  CHECK(tfsupport::total_data_transmits(app.sink.writes[0]) == 1);
  CHECK(tfsupport::total_data_transmits(app.sink.writes[1]) == 0);
}

TEST_CASE("A late opaque reply cannot acknowledge a hidden Notebook page") {
  OpaquePage opaque;
  NotebookApp app;
  app.book.clear();
  REQUIRE(app.book.add_page("Opaque", &opaque));
  REQUIRE(app.book.add_page("Other", &app.second));
  app.input_frames.resize(3);
  app.before = [&](int frame) {
    if (frame == 1) {
      CHECK(opaque.submissions == 0);
      const auto ids = tfsupport::ids_named(app.sink.writes[0]);
      REQUIRE(ids.size() == 1);
      app.input_frames[2] =
          "\033_Gi=" + std::to_string(*ids.begin()) + ";OK\033\\";
      app.book.set_active(1);
    }
  };
  app.run(3, 2);
  CHECK(opaque.submissions == 0);
  CHECK(opaque.content_dirty);
  CHECK(app.second.pixels.submission_count() == 1);
  CHECK(app.errors.empty());
}

TEST_CASE(
    "Notebook keeps a transcript scroll anchor and nested page selection") {
  TextBox transcript;
  TextInput other;
  for (int i = 0; i < 20; ++i)
    transcript.append("line " + std::to_string(i));
  Notebook nested, outer;
  REQUIRE(nested.add_page("Transcript", &transcript));
  REQUIRE(nested.add_page("Other", &other));
  REQUIRE(outer.add_page("Nested", &nested));
  Page sibling;
  REQUIRE(outer.add_page("Sibling", &sibling));
  Screen screen(24, 7);
  outer.set_geometry({0, 0, 24, 7});
  outer.draw(screen);
  transcript.scroll(-5);
  outer.draw(screen);
  std::string before;
  for (int y = 2; y < 7; ++y)
    for (int x = 0; x < 24; ++x)
      before += screen.text_at(x, y);
  REQUIRE(before.find("line 19") == std::string::npos);
  outer.set_active(1);
  outer.draw(screen);
  outer.set_active(0);
  outer.draw(screen);
  std::string after;
  for (int y = 2; y < 7; ++y)
    for (int x = 0; x < 24; ++x)
      after += screen.text_at(x, y);
  CHECK(after == before);
  nested.set_active(1);
  outer.set_active(1);
  outer.set_active(0);
  CHECK(nested.active_page() == &other);
}
