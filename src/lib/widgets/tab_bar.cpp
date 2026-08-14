#include "termforge/widgets/tab_bar.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <utility>

#include "detail/width.hpp"
#include "termforge/widgets/detail/callback.hpp"
#include "termforge/widgets/detail/glyph_fit.hpp"
#include "termforge/widgets/detail/scrollbar.hpp"
#include "termforge/widgets/detail/strip.hpp"
#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/theme.hpp"

namespace termforge {

TabBar::TabBar(std::vector<std::string> titles) {
  set_tabs(std::move(titles));
}

auto TabBar::set_tabs(std::vector<std::string> titles) -> void {
  // Sanitized ONCE, inside set_all, so the string this widget measures is
  // byte-for-byte the string it paints (#10/#22). write_text would sanitize
  // it anyway; doing it only there is what leaves a title's click span 6
  // columns from its glyphs. set_all also owns the -1-iff-empty invariant for
  // all four widgets that carry this state.
  m_list.set_all(std::move(titles));
  m_first = 0;  // every sibling setter rewinds its viewport
  mark_dirty();
}

auto TabBar::add_tab(std::string title) -> void {
  m_list.add(std::move(title));
  mark_dirty();
}

auto TabBar::clear() -> void {
  m_list.clear();
  m_first = 0;
  mark_dirty();
}

auto TabBar::title(int index) const -> std::string {
  if (index < 0 || index >= count()) return {};
  return m_list.at(index);
}

auto TabBar::content_total() const -> int {
  const int n = count();
  if (n <= 0) return 0;
  int total = 0;
  for (int i = 0; i < n; ++i) {
    total += detail::span_width(m_list.at(i));
    if (i + 1 < n) ++total;  // gap column between titles, same as layout_spans
  }
  return total;
}

auto TabBar::content_offset(int first) const -> int {
  const int n = count();
  if (n <= 0) return 0;
  first = std::clamp(first, 0, n - 1);
  int offset = 0;
  for (int i = 0; i < first; ++i) {
    offset += detail::span_width(m_list.at(i)) + 1;
  }
  return offset;
}

auto TabBar::nearest_first_at(int content_col) const -> int {
  const int n = count();
  if (n <= 0) return 0;
  if (content_col < 0) content_col = 0;
  int best = 0;
  int best_dist = std::abs(content_col);  // distance to tab 0 at column 0
  int start = 0;
  for (int i = 1; i < n; ++i) {
    start += detail::span_width(m_list.at(i - 1)) + 1;
    const int dist = std::abs(content_col - start);
    if (dist < best_dist) {
      best_dist = dist;
      best = i;
    }
  }
  return std::clamp(best, 0, max_first());
}

auto TabBar::hbar_visible() const -> bool {
  // A bar must represent a tab-counted move the widget can actually make.
  // One clipped tab has pixel overflow but no second tab offset, and a
  // one-cell track is always a full-width thumb with no usable position. Both
  // are the same degenerate case as the height-one strip's one-column rule:
  // keys and the wheel remain available, while inert chrome stays absent.
  if (!uses_hbar() || count() < 2) return false;
  const Rect r = rect();
  if (r.w < 2) return false;
  return content_total() > r.w;
}

auto TabBar::layout_strip(int first) const -> StripLayout {
  StripLayout out;
  const Rect r = rect();
  const int n = count();
  if (n == 0 || r.w <= 0 || r.h <= 0) return out;

  first = std::clamp(first, 0, n - 1);

  // AN INDICATOR NEVER TAKES THE LAST CONTENT COLUMN. At one column wide there
  // is room for the strip or for the arrow saying the strip continues, and the
  // strip wins: "▸" tells the user which tab is active, which is the whole
  // content of this widget, while "‹" tells them only that the answer is
  // somewhere else. The wheel and the arrow keys still scroll at that width.
  // The › indicator yields for the same reason further down.
  //
  // Height two or more (#131): the second row owns overflow chrome, so the
  // content row keeps every column for titles. Indicators stay height-one only.
  const bool use_indicators = !uses_hbar();
  out.left_arrow = use_indicators && first > 0 && r.w >= 2;
  out.left_x = r.x;
  const int content_x0 = r.x + (out.left_arrow ? 1 : 0);
  const int rect_right = r.x + r.w;

  // Emit tabs from `first` into [content_x0, content_right).
  //
  // StripFit::Whole is TabBar's half of the shared rule (#130): the tab at
  // `first` is emitted UNCONDITIONALLY, clipped to whatever columns remain,
  // because dropping it would leave the offset pointing at a tab that is
  // neither painted nor clickable, which is how a strip goes dead; only later
  // tabs have to fit whole, because a truncated tab in the middle of a strip
  // that already has a › saying "there is more" is just a lie about which one
  // it is. MenuBar passes Truncate, and detail/strip.hpp says why the policy is
  // a required argument rather than a default.
  // -> std::string_view, spelled out. Without it `auto` deduction DECAYS
  // OptionsList::at's `const std::string&` to a std::string, so every span
  // measured would copy its title -- and max_first() calls this per candidate
  // offset, which is the O(n^2) path the comment below it measures in
  // microseconds. The view borrows m_list's storage, which outlives the call.
  const auto title_at = [this](int i) -> std::string_view {
    return m_list.at(i);
  };
  const auto fit = [&](int content_right) {
    return detail::layout_spans(first, n, content_x0, content_right,
                                detail::StripFit::Whole, title_at);
  };

  // Two passes, because whether the › indicator is up depends on how many tabs
  // fit and how many fit depends on whether › took a column. Pass 2's range is
  // a strict subrange of pass 1's, so this settles in two and cannot oscillate.
  out.spans = fit(rect_right);
  const bool all_shown =
      !out.spans.empty() && out.spans.back().index == n - 1;
  if (!all_shown && use_indicators) {
    // Below three columns the indicator would land on ‹'s column or leave no
    // content at all, so it is suppressed rather than drawn on top of it. Two
    // indicators in one column makes the strip permanently one-directional --
    // the unreachable-item class #85 closed. ‹ is the one kept, because it is
    // the one that says "you are scrolled" and it is the way back.
    if (const int narrowed = rect_right - 1; narrowed > content_x0) {
      out.right_arrow = true;
      out.right_x = rect_right - 1;
      out.spans = fit(narrowed);
    }
  }
  return out;
}

// The width test here is currently unobservable, and that is worth saying out
// loud rather than discovering by deleting it: `fit` only ever clips the tab at
// `first`, so a tab reached as a LATER tab is whole or absent, and the one that
// can be clipped is only ever asked about at the offset that clips it -- where
// both max_first() and reveal() have already run out of room to move. Relaxing
// it to "present at all" passes the whole suite today. It stays strict because
// the day `fit` learns to clip a trailing tab too, this is the line that keeps
// End from landing on an offset draw() clamps back, and no test would catch its
// absence in advance.
auto TabBar::shows(const StripLayout& strip, int index) -> bool {
  const auto it =
      std::ranges::find(strip.spans, index, &detail::StripSpan::index);
  return it != strip.spans.end() && it->w >= it->natural;
}
auto TabBar::max_first() const -> int {
  const int n = count();
  if (n == 0) return 0;
  for (int f = 0; f < n - 1; ++f)
    if (shows(layout_strip(f), n - 1)) return f;
  // No offset shows the last tab whole (it is wider than the strip, or there is
  // no strip yet). Putting it leftmost and truncated is the only answer left.
  // Callers that can reach this with no strip must guard: n - 1 is a real
  // offset, and scrolling to it on a widget that has never been laid out is a
  // move nobody made (see scroll_by, reveal).
  return n - 1;

  // COST, measured rather than assumed: this is a linear scan of layouts, so a
  // draw is O(n^2) width measurements -- ~26us at 20 tabs, ~0.45ms at 400 (-O2).
  // Fine for any tab bar a person reads; the fix if that ever changes is a
  // BINARY search, since "all of [f, n) fit" is monotone in f. It is written
  // linearly on purpose: monotonicity is a property of today's fit rule, not an
  // invariant anything enforces, and a max_first() that is silently wrong by one
  // is precisely the End-jump this function is shaped to prevent.
}

auto TabBar::first_visible() const -> int {
  if (m_list.empty()) return 0;
  return std::clamp(m_first, 0, max_first());
}

auto TabBar::reveal(int index) -> void {
  const Rect r = rect();
  // rect() holds LAST frame's geometry here and is {0,0,0,0} before the first
  // draw, where every tab would measure as invisible and this would walk the
  // offset up to `index` on every keypress. Preserve rather than zero, the same
  // rule clamp_scroll and clamp_offset follow.
  if (r.w <= 0 || r.h <= 0) return;
  const int n = count();
  if (index < 0 || index >= n) return;

  if (m_first > index) m_first = index;
  // Bounded by `index` itself: a tab wider than the whole strip is never shown
  // whole, and leftmost-and-truncated is the only sane place for it.
  while (m_first < index && !shows(layout_strip(m_first), index)) ++m_first;
  m_first = std::clamp(m_first, 0, max_first());
}

auto TabBar::activate(int index) -> void {
  const int before = m_list.selected();
  m_list.select(index);  // clamps; forces -1 on an empty bar
  if (m_list.selected() == before) return;  // no-change: consumed, but silent
  reveal(m_list.selected());
  mark_dirty();
  // LAST, and nothing touches `this` after it: the callback may call set_tabs()
  // or destroy the owner. invoke_copy detaches the slot itself (#5/#32).
  detail::invoke_copy(m_on_change, m_list.selected());
}

auto TabBar::set_active(int index) -> void {
  const int before = m_list.selected();
  m_list.select(index);
  if (m_list.selected() == before) return;
  reveal(m_list.selected());
  mark_dirty();
}

auto TabBar::scroll_by(int delta) -> bool {
  if (m_list.empty()) return false;
  // The same degenerate-rect guard reveal() carries, and for a sharper reason:
  // with no strip, layout_strip shows nothing, so max_first() falls through to
  // its "cannot show the last tab whole" answer of n-1 and a single wheel notch
  // would walk a never-laid-out widget off tab 0. Returning early also PRESERVES
  // m_first rather than writing 0 over it, which is clamp_offset's rule for a
  // collapse-then-re-expand (#48 item 4).
  if (const Rect r = rect(); r.w <= 0 || r.h <= 0) return false;
  // One max_first(), not the two that first_visible() plus a clamp would cost:
  // it is the expensive call here (a layout per candidate offset) and this runs
  // on every wheel notch.
  const int limit = max_first();
  const int before = std::clamp(m_first, 0, limit);
  const int target = std::clamp(before + delta, 0, limit);
  m_first = target;  // also normalizes an offset stranded by a geometry change
  return target != before;
}

auto TabBar::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0) {
    clear_dirty();
    return;
  }

  // A RESIZE re-reveals the active tab; a scroll does not. reveal() is bounded
  // by the active index and clamped, so this is idempotent -- two draws at the
  // same geometry do nothing, which is what keeps the wheel's "the active tab
  // may scroll out of view" behaviour intact. It writes m_first but must NOT
  // mark dirty: this frame is already painting, and marking here would leave
  // dirty() permanently true (#69's edge rule).
  if (m_drawn != r) {
    reveal(m_list.selected());
    m_drawn = r;
  }

  // Own the whole rect: the strip is row r.y, any extra rows are blanked.
  screen.fill_rect(r.x, r.y, r.w, r.h, m_fg, m_bg);

  const int first = first_visible();
  const StripLayout strip = layout_strip(first);
  const MarkGlyphs glyphs = mark_glyphs(m_style);

  // One column each -- the indicator columns and the title's left pad -- so
  // the budget is 1 and empty means do not paint. detail/glyph_fit.hpp owns
  // the rule and the reason it is sanitize-then-measure.
  const std::string left = detail::fitted_glyph(glyphs.arrow_left, 1);
  const std::string right = detail::fitted_glyph(glyphs.arrow_right, 1);
  const std::string mark = detail::fitted_glyph(glyphs.selector, 1);
  if (strip.left_arrow && !left.empty())
    screen.write_text(strip.left_x, r.y, left, m_fg, m_bg);
  if (strip.right_arrow && !right.empty())
    screen.write_text(strip.right_x, r.y, right, m_fg, m_bg);

  for (const auto& span : strip.spans) {
    const bool is_active = span.index == m_list.selected();
    // RadioGroup's split, and for its reason: the MARK says which one is
    // chosen whether or not this widget has focus, while the inverted colours
    // say "this is where the arrow keys go". Painting the focus colours
    // unfocused would have a TabBar claim a focus it does not have -- which in
    // examples/widgets.cpp is a real ambiguity, because TextInput binds
    // Left/Right too and the two would look identical.
    const bool lit = is_active && focused();
    const Rgb& fg = lit ? m_active_fg : m_fg;
    const Rgb& bg = lit ? m_active_bg : m_bg;

    screen.fill_rect(span.x, r.y, span.w, 1, fg, bg);
    // The left pad column carries the marker for the active tab -- the half of
    // the state that survives a driver dropping colour (#76). Inactive tabs
    // leave it blank; the fill above already put a space there.
    if (is_active && !mark.empty()) screen.write_text(span.x, r.y, mark, fg, bg);
    // Title starts one column in. The trailing pad is the first thing clipping
    // eats, which is why the budget is w - 1 and not w - 2.
    if (const int avail = span.w - 1; avail > 0)
      screen.write_text(
          span.x + 1, r.y,
          detail::truncate_to_width(
              m_list.at(span.index), avail),
          fg, bg);
  }

  // #131: second-row horizontal track when the rect can host it. Content units
  // are cumulative title columns; the tab-counted offset is mapped through
  // content_offset so variable-width titles stay honest. A whole-tab snap can
  // put that start past total - visible (the last short tab then leaves blank
  // cells to its right), so clamp at the thumb_window boundary rather than
  // feeding the shared helper an out-of-contract viewport triple.
  if (hbar_visible()) {
    const int total = content_total();
    const int offset = std::clamp(content_offset(first), 0, total - r.w);
    detail::draw_scrollbar(
        screen, {r.x, r.y + 1, r.w, 1}, total, offset, r.w,
        scrollbar_glyphs(m_style, ScrollOrientation::Horizontal), theme::kDim,
        m_active_bg, m_bg, ScrollOrientation::Horizontal);
  }

  clear_dirty();
}

auto TabBar::handle_mouse(const MouseEvent& m) -> bool {
  if (m_list.empty()) return false;

  // #35 Q1/Q2: the wheel scrolls the VIEW. The active tab stays put and may
  // scroll out of sight, which is safe here in a way it was not for a dropdown
  // (#85) because a TabBar never COMMITS an off-screen highlight -- there is no
  // key that picks "whatever is highlighted", the active tab is already picked.
  //
  // No rect().contains gate, matching ListWidget: App::route_mouse has already
  // chosen the widget under the pointer via hit_test.
  if (m.scroll_up || m.scroll_down) {
    if (scroll_by(m.scroll_up ? -1 : 1)) mark_dirty();
    return true;
  }

  if (!m.pressed || m.button != 0) return false;
  if (!rect().contains(m.x, m.y)) return false;

  // #131: a click on the second-row track snaps to the nearest tab boundary.
  // Map BOTH inclusive cell-index ranges: [0, track_w - 1] onto
  // [0, total - 1]. Using track_w as the divisor makes the last track cell
  // fall short of the content endpoint; with one very wide early title that
  // can make the final tab unreachable from the track at any click position.
  if (m.y == rect().y + 1 && hbar_visible()) {
    const int total = content_total();
    const int track_w = rect().w;
    const int track_run = track_w - 1;
    const int content_run = total - 1;
    int content_col = static_cast<int>(
        (static_cast<long long>(m.x - rect().x) * content_run +
         track_run / 2) /
        track_run);
    content_col = std::clamp(content_col, 0, content_run);
    const int target = nearest_first_at(content_col);
    if (target != first_visible()) {
      m_first = target;
      mark_dirty();
    }
    return true;
  }

  if (m.y != rect().y) return true;  // a row below the strip: consumed, inert

  const StripLayout strip = layout_strip(first_visible());
  if (strip.left_arrow && m.x == strip.left_x) {
    if (scroll_by(-1)) mark_dirty();
    return true;
  }
  if (strip.right_arrow && m.x == strip.right_x) {
    if (scroll_by(1)) mark_dirty();
    return true;
  }
  // Through the shared reverse map, not a hand-rolled loop: the painted extent
  // and the clickable extent come from one expression over one set of spans, so
  // #10's drift has nowhere to live (#130). -1 is a gap column or the strip's
  // background, both of which fall through to the inert return below.
  if (const int hit = detail::span_at(strip.spans, m.x); hit >= 0) {
    activate(hit);
    return true;
  }
  return true;  // gap or background inside the rect: consumed, inert
}

auto TabBar::on_event(const Event& ev) -> bool {
  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    // An empty bar consuming keys is a keyboard trap: the FocusRing only
    // cycles on a key the focused widget declined.
    if (m_list.empty()) return false;

    if (k->key == Key::Left) {
      activate(m_list.selected() - 1);
      return true;
    }
    if (k->key == Key::Right) {
      activate(m_list.selected() + 1);
      return true;
    }
    if (k->key == Key::Home) {
      activate(0);
      return true;
    }
    if (k->key == Key::End) {
      activate(count() - 1);
      return true;
    }
    // Everything else declined, deliberately: Tab so the FocusRing cycles;
    // Enter and Space because the arrow already switched the view, so eating a
    // form's submit would buy nothing; PageUp/PageDown because a tab bar has no
    // page (#22 does not ask for them, and a silent alias for Home/End is worse
    // than nothing).
    return false;
  }

  if (const auto* m = std::get_if<MouseEvent>(&ev)) return handle_mouse(*m);
  return false;
}

}  // namespace termforge
