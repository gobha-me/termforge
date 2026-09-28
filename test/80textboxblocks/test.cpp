// #354: embedded document blocks, including the real App pixel-pass order.
// All content is synthetic and widget-owned; no decoder, media, or tty.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "support/apc.hpp"
#include "support/image.hpp"
#include "support/terminal_grid.hpp"
#include "termforge/core/app.hpp"
#include "termforge/core/byte_sink.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/drivers/fallback_driver.hpp"
#include "termforge/drivers/kitty_driver.hpp"
#include "termforge/widgets/text_box.hpp"

using namespace termforge;

namespace {
auto fallback(std::string text) -> StyledText {
  return {
      TextSpan{std::move(text), TextStyle{Rgb{220, 30, 20}, {}, Attr::Bold}}};
}

auto paint(TextBox& box, Rect rect) -> Screen {
  Screen screen{30, 12};
  box.set_geometry(rect);
  box.draw(screen);
  return screen;
}

auto geometry(TextBox& box, TextBlockHandle handle) -> TextBlockGeometry {
  const auto result = box.block_geometry(handle);
  REQUIRE(result);
  return *result;
}
} // namespace

TEST_CASE("TextBox blocks: invalid and foreign handles never mutate a slot",
          "[textboxblocks][failure][handles]") {
  TextBox box;
  TextBox foreign;
  const auto block = box.append_block(2, fallback("safe"));
  const auto other = foreign.append_block(2, fallback("other"));
  REQUIRE(block);
  REQUIRE(other);
  CHECK_FALSE(box.set_block_rows({}, 1));
  CHECK_FALSE(box.set_block_rows(*other, 1));
  CHECK_FALSE(box.update_block(*other, 1, fallback("corrupt")));
  CHECK_FALSE(box.remove_block(*other));
  CHECK_FALSE(box.block_geometry(*other));
  CHECK_FALSE(box.block_geometry({}));
  const auto tail = box.begin_entry("tail");
  const TextBlockHandle not_a_block{tail.index, tail.generation, block->owner};
  CHECK_FALSE(box.set_block_rows(not_a_block, 1));
  CHECK_FALSE(box.block_geometry(not_a_block));
  REQUIRE(box.finalize_entry(tail));
  const auto refused = box.set_block_rows(*block, -1);
  REQUIRE_FALSE(refused);
  CHECK(refused.error().severity == Severity::Warning);
  CHECK(box.retained_bytes() == 8);
  const auto screen = paint(box, {2, 3, 8, 4});
  CHECK(screen.text_at(2, 3) == "s");
  REQUIRE(geometry(box, *block).allocation);
  CHECK(geometry(box, *block).allocation->h == 2);
  CHECK_FALSE(box.update_block(*block, -1, fallback("corrupt")));
  CHECK(geometry(box, *block).state == TextBlockLayoutState::Visible);

  REQUIRE(box.remove_block(*block));
  const auto recycled = box.append_block(1, fallback("replacement"));
  REQUIRE(recycled);
  CHECK_FALSE(box.set_block_rows(*block, 9));
  CHECK_FALSE(box.block_geometry(*block));
  box.clear();
  REQUIRE(box.append_block(1, fallback("after clear")));
  CHECK_FALSE(box.block_geometry(*recycled));
  CHECK_FALSE(box.update_block(*recycled, 2, fallback("after clear")));
}

TEST_CASE("TextBox blocks: hard budgets reject atomically before tail mutation",
          "[textboxblocks][failure][limits]") {
  TextBox box;
  REQUIRE(box.set_block_limits({2, 3, 4}));
  CHECK_FALSE(box.append_block(-1, {}));
  CHECK_FALSE(box.append_block(4, {}));
  CHECK(box.line_count() == 0);
  const auto a = box.append_block(3, fallback("a"));
  REQUIRE(a);
  const auto live = box.begin_entry("stream");
  CHECK_FALSE(box.append_block(2, fallback("too tall")));
  REQUIRE(box.append_to_entry(live, " still live"));
  const auto b = box.append_block(1, fallback("b"));
  REQUIRE(b);
  CHECK_FALSE(box.append_block(0, {})); // collapsed blocks still count
  CHECK_FALSE(box.set_block_rows(*b, 2));
  CHECK_FALSE(box.update_block(*b, 2, fallback("wrong")));
  CHECK_FALSE(box.set_block_limits({1, 3, 4}));
  CHECK_FALSE(box.set_block_limits({2, 2, 4}));
  CHECK_FALSE(box.set_block_limits({2, -1, 4}));
  CHECK_FALSE(
      box.set_block_limits({2, 3, std::numeric_limits<std::size_t>::max()}));
  CHECK(box.block_limits() == TextBoxBlockLimits{2, 3, 4});
  CHECK(box.block_count() == 2);
  CHECK(box.block_rows() == 4);
  CHECK(box.retained_bytes() == 19);
  TextBox empty;
  REQUIRE(empty.set_block_limits({0, 0, 0}));
  CHECK_FALSE(empty.append_block(0, {}));
  REQUIRE(empty.set_block_limits({1, 0, 0}));
  REQUIRE(empty.append_block(0, fallback("collapsed")));
  CHECK_FALSE(empty.set_block_limits({0, 0, 0}));

  TextBox large;
  constexpr int maximum = std::numeric_limits<int>::max();
  REQUIRE(
      large.set_block_limits({2, maximum, static_cast<std::size_t>(maximum)}));
  const auto huge = large.append_block(maximum, {});
  REQUIRE(huge);
  CHECK_FALSE(large.append_block(1, {}));
  CHECK(large.block_rows() == static_cast<std::size_t>(maximum));
  // Row accounting must not allocate height-sized placeholder vectors.
  (void)paint(large, {0, 0, 4, 2});
  CHECK(geometry(large, *huge).source_row == maximum - 2);
  large.scroll(std::numeric_limits<int>::min());
  (void)paint(large, {0, 0, 4, 2});
  CHECK(geometry(large, *huge).source_row == 0);
  large.append("x"); // total document rows exceed INT_MAX without an allocation
  large.scroll_to_bottom();
  (void)paint(large, {0, 0, 4, 2});
  CHECK(geometry(large, *huge).source_row == maximum - 1);
  CHECK(geometry(large, *huge).visible.h == 1);
  (void)paint(large,
              {0, 0, 4, maximum}); // enormous track, only twelve screen rows
  CHECK(geometry(large, *huge).source_row == 1);
  CHECK(geometry(large, *huge).visible.h == 12);
}

TEST_CASE("TextBox blocks: retention includes sanitized fallback bytes",
          "[textboxblocks][failure][retention]") {
  TextBox box;
  box.set_retention({.max_entries = 2, .max_bytes = 5});
  box.append("old");
  const auto block = box.append_block(2, fallback("hi\033[2J\007"));
  REQUIRE(block);
  CHECK(box.retained_bytes() == 5);
  const auto tail = box.begin_entry("new");
  CHECK(box.line_count() == 2);
  CHECK(box.retained_bytes() == 5);
  REQUIRE(box.set_block_rows(*block, 0));
  CHECK(box.retained_bytes() == 5); // collapse does not free fallback text
  REQUIRE(box.update_block(*block, 0, fallback("oversized")));
  CHECK_FALSE(box.block_geometry(*block)); // updating may evict its own block
  CHECK(box.block_count() == 0);
  CHECK(box.block_rows() == 0);
  CHECK(box.retained_bytes() == 3);
  REQUIRE(box.append_to_entry(tail, " tail"));
  CHECK(box.retention_over_budget()); // existing live exemption remains

  box.clear();
  box.set_retention({.max_entries = 0, .max_bytes = 0});
  const auto evicted = box.append_block(1, fallback("gone"));
  REQUIRE(evicted); // accepted insertion, immediately evicted by retention
  CHECK_FALSE(box.block_geometry(*evicted));
  CHECK(box.line_count() == 0);
  CHECK(box.retained_bytes() == 0);
  CHECK(box.block_rows() == 0);
}

TEST_CASE("TextBox blocks: layout states and clipping carry exact source rows",
          "[textboxblocks][geometry][failure]") {
  TextBox box;
  box.append("before");
  const auto block = box.append_block(5, fallback("ABCDEFGHIJKLMNO"));
  REQUIRE(block);
  box.append("after");
  CHECK(geometry(box, *block).state == TextBlockLayoutState::NotLaidOut);
  CHECK_FALSE(geometry(box, *block).allocation);
  (void)paint(box, {2, 3, 6, 3}); // total 7, top 4, source rows 3..4
  auto g = geometry(box, *block);
  CHECK(g.state == TextBlockLayoutState::Visible);
  REQUIRE(g.allocation);
  CHECK(*g.allocation == TextBlockAllocation{2, 0, 5, 5});
  CHECK(g.visible == Rect{2, 3, 5, 2});
  CHECK(g.source_row == 3);

  box.scroll(-4);
  CHECK(geometry(box, *block).state == TextBlockLayoutState::NotLaidOut);
  (void)paint(box, {2, 3, 6, 3}); // top 0, source rows 0..1
  g = geometry(box, *block);
  CHECK(g.visible == Rect{2, 4, 5, 2});
  CHECK(g.source_row == 0);
  CHECK(g.visible.contains(2, 4));
  CHECK_FALSE(g.visible.contains(7, 4)); // excludes scrollbar
  CHECK_FALSE(g.visible.contains(2, 3)); // excludes text above
  box.set_geometry({2, 3, 7, 3});
  CHECK(geometry(box, *block).state == TextBlockLayoutState::NotLaidOut);
  (void)paint(box, {2, 3, 7, 3});
  CHECK(geometry(box, *block).visible.w == 6);

  REQUIRE(box.set_block_rows(*block, 0));
  (void)paint(box, {2, 3, 7, 3});
  CHECK(geometry(box, *block).state == TextBlockLayoutState::Empty);
  REQUIRE(geometry(box, *block).allocation);
  CHECK(geometry(box, *block).allocation->h == 0);
  (void)paint(box, {2, 3, 0, 3});
  CHECK(geometry(box, *block).state == TextBlockLayoutState::Empty);
  CHECK_FALSE(geometry(box, *block).allocation);
}

TEST_CASE(
    "TextBox blocks: wrapping resolves scrollbar transitions and tiny views",
    "[textboxblocks][wrap][resize][failure]") {
  TextBox box;
  box.append("界ab");
  const auto block = box.append_block(1, fallback("label"));
  REQUIRE(block);
  auto screen = paint(box, {2, 1, 5, 3});
  CHECK(geometry(box, *block).visible == Rect{2, 2, 5, 1});
  CHECK(box.content_w() == 5);
  const auto live = box.begin_entry("123456");
  screen = paint(box, {2, 1, 5, 3});
  CHECK(geometry(box, *block).visible == Rect{2, 1, 4, 1});
  CHECK(box.content_w() == 4);
  CHECK(screen.text_at(6, 1) != "l"); // scrollbar, never fallback content
  const auto builds = box.wrap_build_count();
  (void)paint(box, {2, 1, 5, 3});
  CHECK(box.wrap_build_count() == builds); // both width caches are stable
  REQUIRE(box.replace_entry(live, "x"));
  (void)paint(box, {2, 1, 5, 3});
  CHECK(geometry(box, *block).visible == Rect{2, 2, 5, 1});
  (void)paint(box, {2, 1, 3, 3}); // width-only resize reflows wide text
  CHECK(geometry(box, *block).visible == Rect{2, 2, 2, 1});
  CHECK(box.content_w() == 2);
  const auto resized_builds = box.wrap_build_count();
  (void)paint(box, {2, 1, 3, 3});
  CHECK(box.wrap_build_count() == resized_builds);

  box.clear();
  const auto wide = box.append_block(2, fallback("界x"));
  REQUIRE(wide);
  Screen narrow{5, 3};
  narrow.write_text(3, 0, "Z", {}, {});
  box.set_geometry({2, 0, 1, 3});
  box.draw(narrow);
  CHECK(geometry(box, *wide).visible == Rect{2, 0, 1, 2});
  CHECK(narrow.text_at(2, 0) == " "); // half-wide glyph is padded
  CHECK(narrow.text_at(2, 1) == "x");
  CHECK(narrow.text_at(3, 0) == "Z"); // neighbor was not overwritten
  (void)paint(box, {0, 0, 1, 0});
  CHECK(geometry(box, *wide).state == TextBlockLayoutState::Empty);
  CHECK_FALSE(geometry(box, *wide).allocation);
  (void)paint(box, {std::numeric_limits<int>::max(),
                    std::numeric_limits<int>::min(), 4, 3});
  CHECK(geometry(box, *wide).state == TextBlockLayoutState::Empty);
}

TEST_CASE("TextBox blocks: screen clipping adds the correct source-row offset",
          "[textboxblocks][geometry][failure]") {
  TextBox box;
  const auto block = box.append_block(5, fallback("abcdefghijklmno"));
  REQUIRE(block);
  const auto screen = paint(box, {-1, -1, 4, 4});
  const auto g = geometry(box, *block);
  CHECK(g.allocation == TextBlockAllocation{-1, -2, 3, 5});
  CHECK(g.visible == Rect{0, 0, 2, 3});
  CHECK(g.source_row == 2);
  CHECK(screen.text_at(0, 0) == "h"); // row "ghi", clipped by one column
}

TEST_CASE(
    "TextBox blocks: growth, collapse, and removal preserve documented anchors",
    "[textboxblocks][scroll][stream][retention]") {
  TextBox box;
  box.append("A");
  const auto b = box.append_block(5, fallback("B"));
  REQUIRE(b);
  box.append("C");
  const auto d = box.append_block(3, fallback("D"));
  REQUIRE(d);
  const auto live = box.begin_entry("E");
  (void)paint(box, {0, 0, 8, 3});
  CHECK(box.at_bottom());
  box.scroll(-5); // anchored inside B at source row 2
  (void)paint(box, {0, 0, 8, 3});
  CHECK(geometry(box, *b).source_row == 2);
  REQUIRE(box.append_to_entry(live, " grows over many rows of text here"));
  REQUIRE(box.set_block_rows(*d, 6));
  (void)paint(box, {0, 0, 8, 3});
  CHECK(geometry(box, *b).source_row == 2);
  CHECK_FALSE(box.at_bottom());
  REQUIRE(box.set_block_rows(*b, 1)); // anchor clamps to the surviving row
  (void)paint(box, {0, 0, 8, 3});
  CHECK(geometry(box, *b).visible == Rect{0, 0, 7, 1});
  CHECK(geometry(box, *b).source_row == 0);
  REQUIRE(box.set_block_rows(*b, 0)); // next content now owns the top row
  auto screen = paint(box, {0, 0, 8, 3});
  CHECK(screen.text_at(0, 0) == "C");
  CHECK(geometry(box, *d).visible.y == 1);
  REQUIRE(box.set_block_rows(*b, 5));
  screen = paint(box, {0, 0, 8, 3});
  CHECK(screen.text_at(0, 0) == "C"); // growing earlier content cannot jump C
  CHECK(geometry(box, *b).state == TextBlockLayoutState::Offscreen);
  box.scroll(1); // anchored on D
  (void)paint(box, {0, 0, 8, 3});
  REQUIRE(geometry(box, *d).visible.y == 0);
  REQUIRE(box.remove_block(*d));
  screen = paint(box, {0, 0, 8, 3});
  CHECK(screen.text_at(0, 0) == "A"); // removed anchor -> oldest surviving
  box.scroll_to_bottom();
  REQUIRE(box.set_block_rows(*b, 6));
  (void)paint(box, {0, 0, 8, 3});
  CHECK(box.at_bottom());
  CHECK(geometry(box, *b).state == TextBlockLayoutState::Offscreen);
  REQUIRE(box.finalize_entry(live));
  box.scroll(std::numeric_limits<int>::min());
  (void)paint(box, {0, 0, 8, 3});
  box.scroll(2); // anchor within B, then evict it
  (void)paint(box, {0, 0, 8, 3});
  box.set_retention({.max_entries = 2});
  screen = paint(box, {0, 0, 8, 3});
  CHECK_FALSE(box.block_geometry(*b));
  CHECK(screen.text_at(0, 0) == "C");
  CHECK(box.block_count() == 0);
}

TEST_CASE(
    "TextBox blocks: owner identities survive neither assignment nor reuse",
    "[textboxblocks][handles][failure]") {
  TextBox original;
  const auto old = original.append_block(2, fallback("old"));
  REQUIRE(old);
  (void)paint(original, {0, 0, 8, 4});
  TextBox copy = original;
  CHECK_FALSE(copy.block_geometry(*old));
  CHECK(copy.block_count() == 1);
  original.clear();
  original = copy;
  CHECK_FALSE(original.block_geometry(*old)); // copy cannot resurrect a handle
  const auto current = original.append_block(1, {});
  REQUIRE(current);
  TextBox moved = std::move(original);
  CHECK_FALSE(moved.block_geometry(*current));
  CHECK_FALSE(original.block_geometry(*current));
  CHECK(original.block_count() == 0);
  CHECK(original.block_rows() == 0);
  CHECK(moved.block_count() == 2);
  const auto reused = original.append_block(1, {});
  REQUIRE(reused);
  CHECK_FALSE(original.set_block_rows(*current, 9));
  moved = std::move(original);
  CHECK_FALSE(moved.block_geometry(*reused));
  CHECK(original.block_count() == 0);

  alignas(TextBox) std::byte storage[sizeof(TextBox)];
  auto* first = std::construct_at(reinterpret_cast<TextBox*>(storage));
  const auto retired = first->append_block(1, {});
  REQUIRE(retired);
  std::destroy_at(first);
  auto* second = std::construct_at(reinterpret_cast<TextBox*>(storage));
  const auto fresh = second->append_block(1, {});
  REQUIRE(fresh);
  CHECK_FALSE(second->set_block_rows(*retired, 2));
  REQUIRE(second->set_block_rows(*fresh, 2));
  std::destroy_at(second);
}

TEST_CASE("TextBox blocks: tiny layouts agree with an independent row model",
          "[textboxblocks][geometry][wrap][failure]") {
  for (int width = 0; width <= 8; ++width) {
    for (int height = 0; height <= 6; ++height) {
      for (int rows = 0; rows <= 5; ++rows) {
        for (int up : {0, 1, 2, 5, 10, 30}) {
          CAPTURE(width, height, rows, up);
          TextBox box;
          box.append("abcdefgh");
          const auto block = box.append_block(rows, {});
          REQUIRE(block);
          const auto live = box.begin_entry("ijklm");
          (void)live;
          (void)paint(box, {2, 3, width, height});
          box.scroll(-up);
          (void)paint(box, {2, 3, width, height});
          const auto g = geometry(box, *block);
          if (width == 0 || height == 0) {
            CHECK(g.state == TextBlockLayoutState::Empty);
            CHECK_FALSE(g.allocation);
            continue;
          }
          // These are unbroken ASCII runs: ceil(bytes / width), no shared
          // production wrapping/layout helper supplies the expected answer.
          const auto count = [](int bytes, int columns) {
            return (bytes + columns - 1) / columns;
          };
          const int full_total = count(8, width) + rows + count(5, width);
          const int cw = width - (width > 1 && full_total > height ? 1 : 0);
          const int first = count(8, cw);
          const int total = first + rows + count(5, cw);
          const int top = std::max(0, total - height - up);
          const Rect full{2, 3 + first - top, cw, rows};
          const auto visible = full.intersect({2, 3, cw, height});
          REQUIRE(g.allocation);
          CHECK(*g.allocation == TextBlockAllocation{full.x, full.y, cw, rows});
          CHECK(g.visible == visible);
          const auto state = rows == 0         ? TextBlockLayoutState::Empty
                             : visible.empty() ? TextBlockLayoutState::Offscreen
                                               : TextBlockLayoutState::Visible;
          CHECK(g.state == state);
          CHECK(g.source_row == (visible.empty() ? 0 : visible.y - full.y));
        }
      }
    }
  }
}

namespace {
struct PreviewSink final : ByteSink {
  bool refuse_first{false};
  std::vector<std::string> frames;
  std::string accepted;

  auto write(std::span<const char> bytes)
      -> std::expected<void, ErrorEvent> override {
    frames.emplace_back(bytes.data(), bytes.size());
    if (refuse_first && frames.size() == 1)
      return std::unexpected{
          ErrorEvent{Severity::Warning, "sink", "preview frame refused"}};
    accepted.append(bytes.data(), bytes.size());
    return {};
  }
};

// Stable Widget*, region zero, and one root payload. Clipping is placement
// state, not a newly decoded/rebuilt image or a content revision.
struct PreviewWidget final : Widget {
  Image image = tfsupport::checker(4, 4, Pixel{255, 20, 10, 255},
                                   Pixel{20, 30, 255, 255});
  int source_row{0};
  int borrows{0};
  int submissions{0};
  bool content_dirty{true};

  auto draw(Screen&) -> void override {} // TextBox authors the fallback
  auto pixel_regions() -> std::vector<Rect> override {
    return rect().empty() ? std::vector<Rect>{} : std::vector<Rect>{rect()};
  }
  auto draw_pixels(Rect, Extent) -> const Image* override {
    ++borrows;
    return &image;
  }
  auto pixel_region_state(Rect) const noexcept -> PixelRegionState override {
    return {PixelRegionMode::Persistent, content_dirty, 1};
  }
  auto pixel_region_submitted(Rect) noexcept -> void override {
    content_dirty = false;
    ++submissions;
  }
  auto pixel_placement(Rect region) const noexcept
      -> ImagePlacementOptions override {
    return {.source = PixelRect{0, source_row, 4, region.h}};
  }
};

struct PreviewCover final : Widget {
  auto draw(Screen& screen) -> void override {
    set_geometry({0, 0, 9, 7});
    screen.fill_rect(0, 0, 9, 7, {}, {});
    screen.write_text(2, 1, "modal", {}, {});
  }
};

class InlinePreviewApp final : public App {
 public:
  InlinePreviewApp() {
    box.append("lead");
    const auto inserted = box.append_block(4, fallback("QZJVQZJVQZJVQZJV"));
    REQUIRE(inserted);
    block = *inserted;
    box.append("tail");
  }

  TextBox box;
  TextBlockHandle block;
  PreviewWidget preview;
  PreviewSink sink;
  std::vector<TextBlockGeometry> layouts;
  std::vector<bool> dirty_at_render;
  std::vector<ErrorEvent> errors;
  bool timeline{false};
  bool cleared_handle_stale{false};

  auto on_render(Screen& screen) -> void override {
    driver().set_output(&sink);
    dirty_at_render.push_back(preview.content_dirty);
    if (timeline) {
      if (m_frame == 2) box.scroll(-3);
      if (m_frame == 3) push_overlay(m_cover);
      if (m_frame == 4) pop_overlay();
      if (m_frame == 5) {
        box.scroll_to_bottom();
        (void)box.begin_entry(
            "one two three four five six seven eight nine ten");
      }
      if (m_frame == 6) box.clear();
    }
    box.set_geometry({2, 1, 5, 3});
    box.draw(screen);
    const auto g = box.block_geometry(block);
    preview.set_geometry({});
    if (g) {
      layouts.push_back(*g);
      if (g->state == TextBlockLayoutState::Visible) {
        preview.set_geometry(g->visible);
        preview.source_row = g->source_row;
        render_pixel_regions(preview);
      }
    } else {
      cleared_handle_stale = true;
      layouts.push_back({});
    }
    ++m_frame;
  }

  auto run(int frames, bool kitty) -> void {
    std::unique_ptr<TerminalDriver> selected;
    if (kitty)
      selected = std::make_unique<KittyDriver>();
    else
      selected = std::make_unique<FallbackDriver>();
    test_run_frames(frames, 12, 8, nullptr, std::move(selected));
  }

 protected:
  auto on_event(const Event& event) -> void override {
    if (const auto* error = std::get_if<ErrorEvent>(&event))
      errors.push_back(*error);
    else
      App::on_event(event);
  }
  auto now_steady() const -> std::chrono::steady_clock::time_point override {
    return m_now;
  }
  auto wait_readable(int timeout_ms) -> bool override {
    m_now += std::chrono::milliseconds(timeout_ms);
    return false;
  }
  auto read_available(char*, int) -> int override { return 0; }

 private:
  PreviewCover m_cover;
  int m_frame{0};
  std::chrono::steady_clock::time_point m_now{};
};
} // namespace

TEST_CASE(
    "TextBox blocks: real App retries, clips, suspends, and retires a preview",
    "[textboxblocks][app][kitty][persistent][failure]") {
  using namespace tfsupport;
  InlinePreviewApp app;
  app.timeline = true;
  app.sink.refuse_first = true;
  app.run(7, true);
  REQUIRE(app.sink.frames.size() == 8); // seven frames plus shutdown
  REQUIRE(app.layouts.size() == 7);
  CHECK(app.dirty_at_render[0]);
  CHECK(app.dirty_at_render[1]); // refusal never acknowledges widget content
  CHECK_FALSE(app.dirty_at_render[2]);
  CHECK(app.preview.submissions == 1);
  CHECK(app.preview.borrows == 2); // failed upload and its accepted retry only
  REQUIRE(app.errors.size() == 1);
  CHECK(app.errors[0].message == "preview frame refused");
  CHECK(app.layouts[1].visible == Rect{2, 1, 4, 2});
  CHECK(app.layouts[1].source_row == 2);
  CHECK(app.layouts[2].visible == Rect{2, 2, 4, 2});
  CHECK(app.layouts[2].source_row == 0);
  CHECK(app.layouts[5].state == TextBlockLayoutState::Offscreen);
  CHECK(app.cleared_handle_stale);
  CHECK(app.preview.rect().empty());
  CHECK_FALSE(app.preview.hit_test(2, 1));

  const auto ids = ids_named(app.sink.frames[1]);
  REQUIRE(ids.size() == 1);
  const auto id = *ids.begin();
  const auto& frames = app.sink.frames;
  CHECK(total_transmits(frames[0]) == 1);
  CHECK(total_transmits(frames[1]) == 1);
  CHECK(placements_of(frames[1], id) == 1);
  CHECK(placements_of(frames[2], id) == 1);
  CHECK(total_transmits(frames[2]) == 0); // scroll is placement-only
  const auto first = placements(frames[1]);
  const auto scrolled = placements(frames[2]);
  REQUIRE(first.size() == 1);
  REQUIRE(scrolled.size() == 1);
  CHECK(key_value(first[0], "y") == "2");
  CHECK(key_value(first[0], "h") == "2");
  CHECK(key_value(scrolled[0], "y") == "0");
  CHECK(placements_of(frames[3], id) ==
        0); // modal never has images punched through
  CHECK(placement_deletes_of(frames[3], id) == 1);
  CHECK(data_deletes_of(frames[3], id) == 0); // overlay suspends, not evicts
  CHECK(placements_of(frames[4], id) == 1);
  CHECK(total_transmits(frames[4]) == 0);
  CHECK(data_deletes_of(frames[5], id) ==
        1); // offscreen returns the pin budget
  CHECK(placements(frames[5]).empty());
  CHECK(placements(frames[6]).empty());
  CHECK(total_transmits(frames[6]) == 0);

  TerminalGrid grid{12, 8};
  grid.feed(frames[1]);
  CHECK(grid.at(2, 1).text == " "); // enhanced route blanked authored fallback
  grid.feed(frames[2]);
  grid.feed(frames[3]);
  CHECK(grid.row_text(1).find("modal") != std::string::npos);
  grid.feed(frames[4]);
  grid.feed(frames[5]);
  grid.feed(frames[6]);
  CHECK(grid.row_text(1) ==
        std::string(12, ' ')); // clear leaves no stale cells
  CHECK(grid.row_text(2) == std::string(12, ' '));
  CHECK(grid.row_text(3) == std::string(12, ' '));
}

TEST_CASE("TextBox blocks: Baseline keeps fallback without borrowing pixels",
          "[textboxblocks][app][fallback]") {
  InlinePreviewApp app;
  app.run(2, false);
  CHECK(app.preview.borrows == 0);
  CHECK(app.preview.submissions == 0);
  CHECK(app.preview.content_dirty);
  CHECK(app.errors.empty());
  tfsupport::TerminalGrid grid{12, 8};
  grid.feed(app.sink.accepted);
  CHECK(grid.row_text(1).find("QZJV") != std::string::npos);
  CHECK(grid.row_text(2).find("QZJV") != std::string::npos);
}
