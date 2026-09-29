// Widget-role and real App frame tests, not a live terminal/font probe.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "support/apc.hpp"
#include "support/screen.hpp"
#include "support/terminal_grid.hpp"
#include "termforge/core/app.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/drivers/ansi_rgb_driver.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"
#include "termforge/widgets/label.hpp"
#include "termforge/widgets/list_widget.hpp"
#include "termforge/widgets/menu_bar.hpp"
#include "termforge/widgets/notebook.hpp"
#include "termforge/widgets/pixel_surface.hpp"
#include "termforge/widgets/progress_bar.hpp"
#include "termforge/widgets/select.hpp"
#include "termforge/widgets/table_widget.hpp"

using namespace termforge;
namespace {
auto roles() -> Theme {
  return {.content_fg = {101, 2, 3},
          .content_bg = {4, 5, 6},
          .surface_fg = {7, 8, 9},
          .surface_bg = {10, 11, 12},
          .focus_fg = {13, 14, 15},
          .focus_bg = {16, 17, 18},
          .selection_fg = {19, 20, 21},
          .selection_bg = {22, 23, 24},
          .muted = {25, 26, 27},
          .accent = {28, 29, 30},
          .glyphs = BorderStyle::Ascii};
}
auto same(const Screen& a, const Screen& b) -> void {
  for (int y = 0; y < a.rows(); ++y)
    for (int x = 0; x < a.cols(); ++x) {
      CHECK(a.text_at(x, y) == b.text_at(x, y));
      CHECK(a.at(x, y).fg == b.at(x, y).fg);
      CHECK(a.at(x, y).bg == b.at(x, y).bg);
      CHECK(a.at(x, y).attrs == b.at(x, y).attrs);
    }
}
auto fill(TableWidget& table) -> void {
  table.set_columns({{"Header", Align::Left, 6}});
  for (int i = 0; i < 10; ++i)
    table.add_row({"Row" + std::to_string(i)});
}
auto fill(MenuBar& menu, int& changes) -> void {
  menu.set_menus({{"File", {{"First", [&] { ++changes; }}, {"Second", [&] {
                                                              ++changes;
                                                            }}}}});
}
} // namespace

TEST_CASE("Theme clearing exactly restores remaining control defaults",
          "[theme][controls][compatibility]") {
  ListWidget list;
  list.set_items({"One", "Two", "Three"});
  TableWidget table;
  fill(table);
  table.set_selected(1);
  TabBar tabs{{"One", "Two", "Three"}};
  MenuBar menu;
  int changes = 0;
  fill(menu, changes);
  Select select{{"One", "Two", "Three"}};
  ProgressBar progress;
  REQUIRE(progress.set_value(0.5F));
  progress.set_label("Half");
  Label page{"Borrowed"};
  Notebook book;
  REQUIRE(book.add_page("Page", &page));
  const std::array<Widget*, 7> widgets{&list,   &table,    &tabs, &menu,
                                       &select, &progress, &book};
  for (auto* widget : widgets) {
    widget->set_geometry({1, 1, 12, 3});
    for (bool focused : {false, true}) {
      widget->set_focused(focused);
      Screen before{14, 7}, after{14, 7};
      widget->draw(before);
      widget->set_theme(roles());
      widget->clear_theme();
      widget->draw(after);
      same(before, after);
    }
  }
  CHECK(changes == 0);
  CHECK_FALSE(page.theme_snapshot());
}

TEST_CASE(
    "Selection controls distinguish roles and keep state across snapshots",
    "[theme][controls][roles]") {
  ListWidget list;
  list.set_items({"One", "Two", "Three", "Four", "Five", "Six"});
  list.set_geometry({0, 0, 10, 3});
  list.set_selected(1);
  list.set_focused(true);
  list.set_theme(roles());
  Screen screen{12, 6};
  list.draw(screen);
  CHECK(screen.at(0, 0).fg == roles().content_fg);
  CHECK(screen.at(0, 1).fg == roles().selection_fg);
  CHECK(screen.at(0, 1).bg == roles().selection_bg);
  CHECK(any(screen.at(0, 1).attrs & Attr::Bold));
  CHECK(screen.at(9, 0).fg == roles().accent);
  CHECK(screen.at(9, 2).fg == roles().muted);
  CHECK(screen.text_at(0, 1) == "*");
  list.set_focused(false);
  list.draw(screen);
  CHECK(screen.text_at(0, 1) == "*");
  CHECK_FALSE(any(screen.at(0, 1).attrs & Attr::Bold));
  const auto selected = list.selected();
  REQUIRE(list.on_event(
      MouseEvent{.x = 2, .y = 1, .button = -1, .scroll_down = true}));
  const auto offset = list.scroll_offset();
  auto next = roles();
  next.content_bg = {90, 91, 92};
  list.set_theme(next);
  list.draw(screen);
  CHECK(list.selected() == selected);
  CHECK(list.scroll_offset() == offset);

  TableWidget table;
  fill(table);
  table.set_geometry({0, 0, 10, 4});
  table.set_selected(0);
  table.set_focused(true);
  table.set_theme(roles());
  table.draw(screen);
  CHECK(screen.at(0, 1).fg == roles().selection_fg);
  CHECK(screen.at(0, 1).bg == roles().selection_bg);
  CHECK(screen.at(8, 1).bg ==
        roles().selection_bg); // whole themed row, including gaps
  CHECK(any(screen.at(2, 1).attrs & Attr::Bold));
  CHECK(screen.at(2, 2).bg == roles().surface_bg);
  CHECK(screen.at(9, 1).fg == roles().accent);
  CHECK(screen.at(9, 3).fg == roles().muted);
  CHECK(screen.at(2, 0).fg == Column{}.header_fg);
  CHECK(screen.at(2, 0).bg == Column{}.header_bg); // authored descriptor wins
  table.scroll(4);
  const auto top = table.scroll_offset();
  table.set_theme(next);
  table.draw(screen);
  CHECK(table.selected() == 0);
  CHECK(table.scroll_offset() == top);
}

TEST_CASE("Control overrides win regardless of theme setter order",
          "[theme][controls][overrides]") {
  for (bool theme_first : {false, true}) {
    ListWidget list;
    list.set_items({"One", "Two", "Three", "Four", "Five", "Six"});
    TableWidget table;
    fill(table);
    for (Widget* widget : std::array<Widget*, 2>{&list, &table}) {
      widget->set_geometry({0, 0, 10, 4});
      if (theme_first) widget->set_theme(roles());
    }
    const auto override_rows = [](auto& widget) {
      // Equal-to-old-default values still express explicit caller intent.
      widget.set_colors(theme::kFg, theme::kBg);
      widget.set_selected_colors(theme::kFocusFg, theme::kFocusBg);
      widget.set_scrollbar_colors(theme::kDim, theme::kFocusBg);
      widget.set_style(BorderStyle::Single);
      widget.set_marker("!");
      widget.set_selected(0);
    };
    override_rows(list);
    override_rows(table);
    for (Widget* widget : std::array<Widget*, 2>{&list, &table}) {
      widget->set_theme(roles());
      Screen screen{12, 6};
      widget->draw(screen);
      const int y = widget == &list ? 0 : 1;
      CHECK(screen.at(0, y).fg == theme::kFocusFg);
      CHECK(screen.at(0, y).bg == theme::kFocusBg);
      CHECK(screen.text_at(0, y) == "!");
      CHECK(screen.at(2, y + 1).fg == theme::kFg);
      CHECK(screen.at(9, y).fg == theme::kFocusBg);
      CHECK(screen.at(9, 3).fg == theme::kDim);
      widget->clear_theme();
      widget->draw(screen);
      CHECK(screen.text_at(0, y) == "!");
    }
    CHECK(list.style() == BorderStyle::Single);
    CHECK(table.style() == BorderStyle::Single);
    list.set_marker_enabled(false);
    table.set_marker_enabled(false);
    list.set_theme(roles());
    table.set_theme(roles());
    CHECK(list.gutter_cols() == 0);
    CHECK(table.gutter_cols() == 0);

    TabBar tabs{{"Page"}};
    Select select{{"Choice"}};
    MenuBar menu;
    int changes = 0;
    fill(menu, changes);
    Label page{"Borrowed"};
    Notebook book;
    REQUIRE(book.add_page("Page", &page));
    for (Widget* widget : std::array<Widget*, 4>{&tabs, &select, &menu, &book})
      if (theme_first) widget->set_theme(roles());
    tabs.set_style(BorderStyle::Single);
    select.set_style(BorderStyle::Single);
    menu.set_style(BorderStyle::Single);
    book.set_style(BorderStyle::Single);
    for (Widget* widget :
         std::array<Widget*, 4>{&tabs, &select, &menu, &book}) {
      widget->set_geometry({0, 0, 12, 2});
      widget->set_theme(roles());
      Screen screen{12, 5};
      widget->draw(screen);
      CHECK(tfsupport::row_text(screen, 0).find('*') == std::string::npos);
      widget->clear_theme();
    }
    CHECK(tabs.style() == BorderStyle::Single);
    CHECK(select.style() == BorderStyle::Single);
    CHECK(menu.style() == BorderStyle::Single);

    ProgressBar progress;
    progress.set_geometry({0, 0, 10, 1});
    REQUIRE(progress.set_value(0.5F));
    progress.set_label("X");
    if (theme_first) progress.set_theme(roles());
    progress.set_colors({0, 255, 128}, {48, 48, 64}, theme::kFg);
    progress.set_label_bg({32, 32, 64});
    progress.set_theme(roles());
    Screen screen{10, 1};
    progress.draw(screen);
    CHECK(screen.at(0, 0).fg == Rgb{0, 255, 128});
    CHECK(screen.at(9, 0).fg == Rgb{48, 48, 64});
    CHECK(screen.at(4, 0).fg == theme::kFg);
    CHECK(screen.at(4, 0).bg == Rgb{32, 32, 64});
  }
}

TEST_CASE(
    "Popup themes preserve highlight callbacks and last-painted hit mapping",
    "[theme][controls][popup]") {
  Select select{{"First", "Second", "Third"}};
  select.set_geometry({1, 1, 12, 1});
  select.set_focused(true);
  int changes = 0;
  select.on_change([&](int, const std::string&) { ++changes; });
  select.set_theme(roles());
  Screen screen{24, 8};
  select.draw(screen);
  CHECK(screen.at(1, 1).fg == roles().focus_fg);
  CHECK(screen.at(1, 1).bg == roles().focus_bg);
  CHECK(any(screen.at(1, 1).attrs & Attr::Bold));
  REQUIRE(select.on_event(KeyEvent{Key::Enter}));
  REQUIRE(select.on_event(KeyEvent{Key::Down}));
  select.draw(screen);
  REQUIRE(select.highlighted() == 1);
  CHECK(screen.at(1, 2).fg == roles().surface_fg);
  CHECK(screen.at(1, 3).fg == roles().selection_fg);
  CHECK(any(screen.at(1, 3).attrs & Attr::Bold));
  CHECK(screen.text_at(1, 3) == "*");
  select.set_geometry({14, 5, 8, 1});
  select.clear_theme();
  select.set_theme(roles());
  CHECK(select.dropdown_open());
  CHECK(select.highlighted() == 1);
  CHECK(select.selected() == 0);
  CHECK(changes == 0);
  REQUIRE(select.hit_test(3, 3)); // still the last painted row, not the new box
  REQUIRE(select.on_event(
      MouseEvent{.x = 3, .y = 3, .button = 0, .pressed = true}));
  CHECK(select.selected() == 1);
  CHECK(changes == 1);
  CHECK_FALSE(select.dropdown_open());

  MenuBar menu;
  int actions = 0;
  fill(menu, actions);
  menu.set_geometry({1, 1, 20, 1});
  menu.set_focused(true);
  menu.set_theme(roles());
  REQUIRE(menu.on_event(KeyEvent{Key::Enter}));
  REQUIRE(menu.on_event(KeyEvent{Key::Down}));
  menu.draw(screen);
  CHECK(screen.at(1, 1).fg == roles().focus_fg);
  CHECK(any(screen.at(1, 1).attrs & Attr::Bold));
  CHECK(screen.at(1, 2).fg == roles().surface_fg);
  CHECK(screen.at(1, 3).fg == roles().selection_fg);
  CHECK(screen.text_at(1, 3) == "*");
  menu.set_geometry({14, 5, 8, 1});
  menu.clear_theme();
  menu.set_theme(roles());
  CHECK(menu.dropdown_open());
  CHECK(menu.active_menu() == 0);
  CHECK(actions == 0);
  REQUIRE(menu.hit_test(3, 3));
  REQUIRE(
      menu.on_event(MouseEvent{.x = 3, .y = 3, .button = 0, .pressed = true}));
  CHECK(actions == 1);
  CHECK_FALSE(menu.dropdown_open());
}

TEST_CASE(
    "Progress themes change paint not value pulse phase or content glyphs",
    "[theme][controls][progress]") {
  ProgressBar progress;
  progress.set_geometry({1, 1, 10, 1});
  REQUIRE(progress.set_value(0.5F));
  progress.set_label("X");
  progress.set_theme(roles());
  Screen screen{12, 3};
  progress.draw(screen);
  CHECK(screen.at(1, 1).fg == roles().accent);
  CHECK(screen.at(10, 1).fg == roles().muted);
  CHECK(screen.at(1, 1).bg == roles().content_bg);
  CHECK(screen.at(5, 1).fg == roles().content_fg);
  CHECK(screen.at(5, 1).bg == roles().surface_bg);
  CHECK(screen.text_at(1, 1) == "█");
  CHECK(screen.text_at(10, 1) == "─");
  progress.set_label("");
  progress.set_indeterminate();
  progress.on_tick(std::chrono::duration<double>{0.5});
  progress.draw(screen);
  const auto before = tfsupport::row_text(screen, 1);
  progress.clear_theme();
  progress.draw(screen);
  CHECK(tfsupport::row_text(screen, 1) == before);
  progress.set_theme(roles());
  progress.draw(screen);
  CHECK(tfsupport::row_text(screen, 1) == before);
  CHECK(progress.indeterminate());
  CHECK(progress.value() == 0.5F);
}

TEST_CASE("Notebook themes owned tabs but never borrowed pages",
          "[theme][controls][notebook]") {
  Label first{"First page"}, second{"Second page"};
  auto independent = roles();
  independent.content_fg = {91, 92, 93};
  second.set_theme(independent);
  Notebook book;
  int changes = 0;
  book.on_change([&](int) { ++changes; });
  book.set_geometry({0, 0, 20, 5});
  book.set_focused(true);
  book.set_theme(roles()); // also applies when pages are registered later
  REQUIRE(book.add_page("One", &first));
  REQUIRE(book.add_page("Two", &second));
  Screen screen{20, 5};
  book.draw(screen);
  CHECK(screen.at(0, 0).fg == roles().focus_fg);
  CHECK(screen.at(0, 0).bg == roles().focus_bg);
  CHECK(any(screen.at(0, 0).attrs & Attr::Bold));
  CHECK(screen.text_at(0, 0) == "*");
  CHECK_FALSE(first.theme_snapshot());
  CHECK(second.theme_snapshot() == independent);
  CHECK(screen.at(0, 2).fg == theme::kFg);
  book.set_active(1);
  book.draw(screen);
  CHECK(screen.at(0, 2).fg == independent.content_fg);
  book.clear_theme();
  book.draw(screen);
  CHECK(book.active() == 1);
  CHECK(book.active_page() == &second);
  CHECK_FALSE(first.theme_snapshot());
  CHECK(second.theme_snapshot() == independent);
  CHECK(changes == 0);
}

TEST_CASE("Themed tab strips keep scroll and drawn hit spans",
          "[theme][controls][tabs]") {
  TabBar tabs{{"First", "Second", "Third", "Fourth"}};
  tabs.set_geometry({1, 1, 12, 2});
  tabs.set_theme(roles());
  tabs.set_focused(true);
  int changes = 0;
  tabs.on_change([&](int) { ++changes; });
  Screen screen{20, 5};
  tabs.draw(screen);
  CHECK(screen.at(1, 1).fg == roles().focus_fg);
  CHECK(any(screen.at(1, 1).attrs & Attr::Bold));
  CHECK(screen.at(1, 2).fg == roles().accent);
  CHECK(screen.at(12, 2).fg == roles().muted);
  REQUIRE(tabs.on_event(
      MouseEvent{.x = 2, .y = 1, .button = -1, .scroll_down = true}));
  const auto offset = tabs.first_visible();
  REQUIRE(offset > 0);
  const auto active = tabs.active();
  tabs.clear_theme();
  tabs.set_theme(roles());
  tabs.draw(screen);
  CHECK(tabs.first_visible() == offset);
  CHECK(tabs.active() == active);
  CHECK(changes == 0);
  tabs.set_active(1);
  tabs.draw(screen);
  const auto [x, width] =
      tfsupport::highlighted_run(screen, 1, roles().focus_bg);
  REQUIRE(width > 0);
  REQUIRE(
      tabs.on_event(MouseEvent{.x = x, .y = 1, .button = 0, .pressed = true}));
  CHECK(tabs.active() == 1);
  CHECK(changes == 0); // reselect is silent
  tabs.set_focused(false);
  tabs.draw(screen);
  CHECK(screen.text_at(x, 1) == "*");
  CHECK(screen.at(x, 1).fg == roles().content_fg);
  CHECK_FALSE(any(screen.at(x, 1).attrs & Attr::Bold));
  REQUIRE(tabs.on_event(KeyEvent{Key::Right}));
  CHECK(tabs.active() == 2);
  CHECK(changes == 1);
}

TEST_CASE("Themed controls clip tiny rectangles without touching neighbours",
          "[theme][controls][clipping]") {
  ListWidget list;
  list.set_items({"Wide 界", "Second"});
  TableWidget table;
  fill(table);
  TabBar tabs{{"Wide 界", "Second"}};
  Select select{{"Wide 界", "Second"}};
  ProgressBar progress;
  REQUIRE(progress.set_value(0.5F));
  Label page{"Wide 界"};
  Notebook book;
  REQUIRE(book.add_page("Wide 界", &page));
  for (Widget* widget : std::array<Widget*, 6>{&list, &table, &tabs, &select,
                                               &progress, &book}) {
    widget->set_theme(roles());
    widget->set_focused(true);
    for (int width : {0, 1, 2, 3}) {
      Screen screen{8, 6};
      screen.fill_rect(0, 0, 8, 6, {200, 201, 202}, {});
      const Rect rect{2, 2, width, 2};
      widget->set_geometry(rect);
      widget->draw(screen);
      for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 8; ++x)
          if (!rect.contains(x, y))
            CHECK(screen.at(x, y).fg == Rgb{200, 201, 202});
    }
  }
}

namespace {
struct ControlSink final : ByteSink {
  std::vector<std::string> frames;
  auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override {
    frames.emplace_back(bytes.begin(), bytes.end());
    return {};
  }
};
class ControlsApp final : public App {
 public:
  ListWidget list;
  Select select{{"First", "Second", "Third"}};
  PixelSurface pixels{{2, 2}, Pixel{180, 60, 30, 255}};
  Notebook book;
  ControlSink sink;
  int frame{0};
  bool images{false};
  auto run(int tier, bool image_page) -> void {
    list.set_items({"First", "Second", "Third"});
    images = image_page;
    REQUIRE(book.add_page("Pixels", &pixels));
    std::unique_ptr<TerminalDriver> selected;
    if (tier == 0) selected = std::make_unique<FallbackDriver>();
    if (tier == 1) selected = std::make_unique<AnsiRgbDriver>();
    if (tier == 2) selected = std::make_unique<KittyDriver>();
    test_run_frames(4, 20, 7, nullptr, std::move(selected));
  }

 protected:
  auto read_available(char*, int) -> int override { return 0; }
  auto wait_readable(int) -> bool override { return false; }
  auto on_render(Screen& screen) -> void override {
    driver().set_output(&sink);
    if (frame == 0 || frame == 2) {
      auto next = roles();
      if (frame == 2) {
        next.selection_bg = {90, 91, 92};
        next.focus_bg = {93, 94, 95};
      }
      list.set_theme(next);
      select.set_theme(next);
      book.set_theme(next);
    }
    if (frame == 0) {
      list.set_selected(1);
      list.set_focused(true);
      select.set_focused(true);
      REQUIRE(select.on_event(KeyEvent{Key::Enter}));
      REQUIRE(select.on_event(KeyEvent{Key::Down}));
      book.set_focused(true);
    }
    if (images) {
      book.set_geometry({0, 0, 20, 7});
      book.draw(screen);
      render_pixel_regions(book);
      CHECK_FALSE(pixels.theme_snapshot());
    } else {
      list.set_geometry({0, 0, 20, 3});
      select.set_geometry({0, 3, 20, 1});
      list.draw(screen);
      select.draw(screen);
      CHECK(select.dropdown_open());
      CHECK(select.highlighted() == 1);
      CHECK(list.selected() == 1);
    }
    ++frame;
  }
};
} // namespace

TEST_CASE("Control roles reach three-tier App writes with quiet stable frames",
          "[theme][controls][app][wire]") {
  for (int tier : {0, 1, 2}) {
    ControlsApp app;
    app.run(tier, false);
    REQUIRE(app.sink.frames.size() == 4);
    CHECK(app.sink.frames[1].empty());
    CHECK(app.sink.frames[3].empty());
    tfsupport::TerminalGrid terminal{20, 7};
    terminal.feed(app.sink.frames[0]);
    CHECK(terminal.row_text(1).starts_with("* Second"));
    CHECK(terminal.row_text(5).starts_with("*Second"));
    CHECK(terminal.at(0, 1).bold);
    CHECK(terminal.at(0, 3).bold);
    CHECK(terminal.at(0, 5).bold);
    if (tier == 0)
      CHECK(terminal.at(0, 1).bg == -1);
    else
      CHECK(terminal.at(0, 1).bg == ((22 << 16) | (23 << 8) | 24));
    terminal.feed(app.sink.frames[2]);
    CHECK(terminal.row_text(1).starts_with("* Second"));
    CHECK(terminal.at(0, 1).bold);
    if (tier != 0) CHECK(terminal.at(0, 1).bg == ((90 << 16) | (91 << 8) | 92));
  }
}

TEST_CASE(
    "Notebook theme transitions keep borrowed persistent producers resident",
    "[theme][controls][app][pixels]") {
  ControlsApp app;
  app.run(2, true);
  CHECK(app.pixels.submission_count() == 1);
  CHECK_FALSE(app.pixels.content_dirty());
  REQUIRE(app.sink.frames.size() >= 4); // shutdown may append the final delete
  CHECK(tfsupport::total_data_transmits(app.sink.frames[0]) == 1);
  CHECK_FALSE(app.sink.frames[2].empty()); // tab chrome really changes
  const auto ids = tfsupport::ids_named(app.sink.frames[0]);
  REQUIRE(ids.size() == 1);
  for (std::size_t i : {1U, 2U, 3U}) {
    CHECK(tfsupport::total_data_transmits(app.sink.frames[i]) == 0);
    CHECK(tfsupport::data_deletes_of(app.sink.frames[i], *ids.begin()) == 0);
  }
}
