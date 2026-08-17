#include "termforge/widgets/text_box.hpp"

#include <algorithm>
#include <limits>
#include <string_view>

#include "detail/utf8.hpp"
#include "detail/wrap.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/widgets/detail/scrollbar.hpp"
#include "termforge/widgets/detail/viewport.hpp"
#include "termforge/widgets/theme.hpp"

namespace termforge {
namespace {

// Default style for the plain-string append path — matches the colours draw()
// historically hard-coded (theme fg, zeroed bg, no attrs).
[[nodiscard]] auto plain_style() noexcept -> TextStyle {
  return TextStyle{theme::kFg, Rgb{}, Attr::None};
}

auto sanitize_spans(StyledText& line) -> void {
  for (TextSpan& span : line) span.text = Screen::sanitize(span.text);
}

[[nodiscard]] auto plain_text(std::string text) -> StyledText {
  return StyledText{TextSpan{std::move(text), plain_style()}};
}

// Return the number of bytes at the end that form a still-plausible but
// incomplete RFC-3629 sequence. Malformed tails return zero and are handed to
// Screen::sanitize, which drops them under the canonical Strip policy.
[[nodiscard]] auto incomplete_utf8_suffix(std::string_view text) noexcept
    -> std::size_t {
  if (text.empty()) return 0;
  std::size_t lead_pos = text.size() - 1;
  while (lead_pos > 0 &&
         (static_cast<unsigned char>(text[lead_pos]) & 0xC0u) == 0x80u)
    --lead_pos;

  const auto lead = static_cast<unsigned char>(text[lead_pos]);
  const std::size_t expected = detail::utf8_seq_len(lead);
  const std::size_t available = text.size() - lead_pos;
  if (expected <= 1 || available >= expected) return 0;

  if (available >= 2) {
    const auto [lo, hi] = detail::utf8_second_byte_range(lead);
    const auto second = static_cast<unsigned char>(text[lead_pos + 1]);
    if (second < lo || second > hi) return 0;
  }
  for (std::size_t i = lead_pos + 2; i < text.size(); ++i) {
    if ((static_cast<unsigned char>(text[i]) & 0xC0u) != 0x80u) return 0;
  }
  return available;
}

[[nodiscard]] auto plausible_utf8_prefix(std::string_view text) noexcept
    -> bool {
  if (text.empty()) return false;
  const auto lead = static_cast<unsigned char>(text.front());
  const std::size_t expected = detail::utf8_seq_len(lead);
  if (expected <= 1 || text.size() >= expected) return false;
  if (text.size() >= 2) {
    const auto [lo, hi] = detail::utf8_second_byte_range(lead);
    const auto second = static_cast<unsigned char>(text[1]);
    if (second < lo || second > hi) return false;
  }
  for (std::size_t i = 2; i < text.size(); ++i) {
    if ((static_cast<unsigned char>(text[i]) & 0xC0u) != 0x80u) return false;
  }
  return true;
}

}  // namespace

auto TextBox::append(std::string line) -> void {
  // Single-span compatibility wrapper over the styled document path (#25).
  append(plain_text(std::move(line)));
}

auto TextBox::append(StyledText line) -> void {
  sanitize_spans(line);
  if (m_live) {
    if (Entry* live = resolve(*m_live)) {
      const std::size_t old_bytes = live->bytes;
      finish_pending(*live);
      live->finalized = true;
      note_entry_change(*live, old_bytes, false);
    }
    m_live.reset();
  }
  (void)allocate_entry(std::move(line), true);
  if (m_follow) m_scroll = 0;
  const bool evicted = enforce_retention();
  if (evicted && !m_follow && !m_anchor)
    m_scroll = std::numeric_limits<int>::max();
  mark_dirty();
}

auto TextBox::begin_entry() -> TextEntryHandle {
  return begin_entry(StyledText{});
}

auto TextBox::begin_entry(std::string initial) -> TextEntryHandle {
  return begin_entry(plain_text(std::move(initial)));
}

auto TextBox::begin_entry(StyledText initial) -> TextEntryHandle {
  if (m_live) {
    if (Entry* live = resolve(*m_live)) {
      const std::size_t old_bytes = live->bytes;
      finish_pending(*live);
      live->finalized = true;
      note_entry_change(*live, old_bytes, false);
    }
    m_live.reset();
  }

  const TextEntryHandle handle = allocate_entry({}, false);
  m_live = handle;
  Entry* entry = resolve(handle);
  (void)append_chunks(*entry, std::move(initial));
  if (m_follow) {
    m_scroll = 0;
    m_anchor.reset();
  }
  const bool evicted = enforce_retention();
  if (evicted && !m_follow && !m_anchor)
    m_scroll = std::numeric_limits<int>::max();
  mark_dirty();
  return handle;
}

auto TextBox::append_to_entry(TextEntryHandle handle, std::string chunk)
    -> bool {
  return append_to_entry(handle, plain_text(std::move(chunk)));
}

auto TextBox::append_to_entry(TextEntryHandle handle, StyledText chunk)
    -> bool {
  Entry* entry = resolve_live(handle);
  if (!entry) return false;
  if (chunk.empty()) return true;
  bool any_bytes = false;
  for (const TextSpan& span : chunk) any_bytes |= !span.text.empty();
  if (!any_bytes) return true;

  (void)append_chunks(*entry, std::move(chunk));
  const bool evicted = enforce_retention();
  if (evicted) mark_dirty();
  if (evicted && !m_follow && !m_anchor)
    m_scroll = std::numeric_limits<int>::max();
  return true;
}

auto TextBox::replace_entry(TextEntryHandle handle, std::string text) -> bool {
  return replace_entry(handle, plain_text(std::move(text)));
}

auto TextBox::replace_entry(TextEntryHandle handle, StyledText text) -> bool {
  Entry* entry = resolve_live(handle);
  if (!entry) return false;
  (void)replace_chunks(*entry, std::move(text));
  const bool evicted = enforce_retention();
  if (evicted) mark_dirty();
  if (evicted && !m_follow && !m_anchor)
    m_scroll = std::numeric_limits<int>::max();
  return true;
}

auto TextBox::finalize_entry(TextEntryHandle handle) -> bool {
  Entry* entry = resolve_live(handle);
  if (!entry) return false;
  const std::size_t old_bytes = entry->bytes;
  finish_pending(*entry);
  entry->finalized = true;
  m_live.reset();
  note_entry_change(*entry, old_bytes, false);
  const bool evicted = enforce_retention();
  if (evicted) mark_dirty();
  return true;
}

auto TextBox::set_retention(TextBoxRetention retention) -> void {
  if (m_retention == retention) return;
  m_retention = retention;
  if (enforce_retention()) mark_dirty();
}

auto TextBox::retention_over_budget() const noexcept -> bool {
  return (m_retention.max_entries &&
          m_order.size() > *m_retention.max_entries) ||
         (m_retention.max_bytes &&
          m_retained_bytes > *m_retention.max_bytes);
}

auto TextBox::clear() -> void {
  m_order.clear();
  m_free.clear();
  for (std::size_t i = 0; i < m_slots.size(); ++i) {
    Slot& slot = m_slots[i];
    slot.entry.reset();
    ++slot.generation;
    if (slot.generation == 0) ++slot.generation;
    m_free.push_back(i);
  }
  m_live.reset();
  m_anchor.reset();
  m_retained_bytes = 0;
  m_scroll = 0;
  m_follow = true;
  mark_dirty();
}

auto TextBox::at_bottom() const noexcept -> bool { return m_scroll == 0; }

auto TextBox::scroll(int delta) -> void {
  // TextBox's m_scroll is INVERTED relative to the library convention
  // (detail/viewport.hpp): here 0 == pinned to the bottom and a LARGER value
  // means scrolled further UP, whereas the uniform convention counts rows
  // scrolled past the top. The public scroll(delta) keeps its own historical
  // meaning (positive = toward newer/down), so this body converts signs
  // rather than adopting the helper's direction -- at_bottom() and m_follow
  // must behave exactly as before from the app's point of view (#35).
  //
  // #217's per-entry caches make the wrapped count available here too, so the
  // anchor can be captured before a producer mutates the tail.
  m_scroll = std::max(0, m_scroll + (delta < 0 ? -delta : 0));  // up increases m_scroll
  if (delta > 0) m_scroll = std::max(0, m_scroll - delta);       // down decreases
  m_follow = (m_scroll == 0);
  refresh_anchor_from_scroll();
  mark_dirty();
}

auto TextBox::scroll_to_bottom() -> void {
  m_scroll = 0;
  m_follow = true;
  m_anchor.reset();
  mark_dirty();
}

auto TextBox::on_event(const Event& ev) -> bool {
  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    if (k->key == Key::PageUp) { scroll(-(rect().h > 1 ? rect().h - 1 : 1)); return true; }
    if (k->key == Key::PageDown) { scroll(rect().h > 1 ? rect().h - 1 : 1); return true; }
  }
  if (const auto* m = std::get_if<MouseEvent>(&ev)) {
    // #35 Q1: wheel scrolls the VIEW (TextBox has no selection to move). The
    // step is the shared kWheelStep; scroll() owns the sign inversion.
    if (m->scroll_up) { scroll(-detail::kWheelStep); return true; }
    if (m->scroll_down) { scroll(detail::kWheelStep); return true; }
    // #21: a press on the scrollbar's column page-jumps the view. TextBox
    // can't know the wrapped total without drawing, so it can't locate the
    // thumb here -- the jump is directional instead: upper half of the strip
    // pages toward older, lower half toward newer, which is the convention
    // every clicking user already expects from a track. scroll() owns the
    // sign inversion and the follow latch.
    if (m->pressed && m->button == 0 && rect().contains(m->x, m->y) &&
        m->x == rect().x + rect().w - 1 && rect().w > 1) {
      const int page = std::max(1, rect().h > 1 ? rect().h - 1 : 1);
      const int mid = rect().y + rect().h / 2;
      scroll(m->y < mid ? -page : page);
      return true;
    }
  }
  return false;
}

auto TextBox::content_w() const noexcept -> int {
  const int w = rect().w;
  if (w <= 0) return 0;
  // The bar's existence depends on the WRAPPED row count, which only draw()
  // computes -- so the width it will claim is decided in two passes there
  // (wrap at full width, and if the content then overflows, keep the bar and
  // the text keeps this narrower width on the NEXT wrap). To avoid a
  // one-frame oscillation where the bar toggles the wrap width every frame,
  // content_w() reports the bar-aware width whenever the bar COULD be up:
  // logical lines alone already exceeding the view is a stable lower bound
  // (wrapping never shrinks the row count). A single short logical line that
  // wraps to exactly the view height is the edge where the two passes
  // disagree for one frame; the bar then appears with the text already
  // wrapped for it, which is the harmless direction.
  const bool bar_possible =
      rect().h > 0 && m_order.size() > static_cast<std::size_t>(rect().h);
  return std::max(0, w - (bar_possible ? 1 : 0));
}

auto TextBox::draw(Screen& screen) -> void {
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0) { clear_dirty(); return; }

  const Rgb fg = theme::kFg;
  // Own the whole rect: blank it every frame so clear()/scroll/shrink can't
  // leave stale text behind (immediate-mode contract, see widget.hpp).
  screen.fill_rect(r.x, r.y, r.w, r.h, fg, {});

  // Wrap at the bar-aware width (see content_w()): when the bar is possible
  // the text already leaves its column free, so an appearing bar covers no
  // text and the wrap is stable frame to frame.
  //
  // A cw of 0 still wraps: wrap_into(width <= 0) means "don't wrap", so the
  // logical lines pass through and the paint loop clips them to the columns
  // that exist. (Skipping the wrap here produced total == 0: a blank box
  // with no bar -- erasing the content AND the bar's reason to exist.)
  const int cw = content_w();
  const std::size_t total_size = wrapped_total(cw);
  const int total = static_cast<int>(std::min<std::size_t>(
      total_size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
  // Clamp the scroll offset now that the wrapped line count is known --
  // scroll() can't bound it (content may have changed since). m_scroll counts
  // UP from the bottom (inverted, see scroll()), but the bounds are symmetric:
  // the valid range is [0, max(0, total - h)] in either convention, so the
  // shared clamp applies directly.
  const std::size_t max_top =
      total_size > static_cast<std::size_t>(r.h)
          ? total_size - static_cast<std::size_t>(r.h)
          : 0;
  if (!m_follow) {
    if (const auto top = anchored_top(cw, max_top)) {
      const std::size_t visible_end =
          std::min(total_size, *top + static_cast<std::size_t>(r.h));
      const std::size_t from_bottom = total_size - visible_end;
      m_scroll = static_cast<int>(std::min<std::size_t>(
          from_bottom,
          static_cast<std::size_t>(std::numeric_limits<int>::max())));
    }
  }
  m_scroll = detail::clamp_offset(m_scroll, total, r.h);
  m_follow = (m_scroll == 0);
  const std::size_t bottom = total_size - static_cast<std::size_t>(m_scroll);
  const std::size_t top =
      bottom > static_cast<std::size_t>(r.h)
          ? bottom - static_cast<std::size_t>(r.h)
          : 0;

  std::size_t flat = 0;
  int paint_row = 0;
  for (const std::size_t slot_index : m_order) {
    Entry& entry = *m_slots[slot_index].entry;
    const auto& rows = ensure_wrapped(entry, cw);
    for (const StyledText& row : rows) {
      if (flat >= top && flat < bottom && paint_row < r.h) {
        screen.write_styled(r.x, r.y + paint_row, row);
        ++paint_row;
      }
      ++flat;
      if (flat >= bottom) break;
    }
    if (flat >= bottom) break;
  }

  if (m_follow)
    m_anchor.reset();
  else
    anchor_from_top(top, cw);

  // scroll indicator when not at the bottom
  if (m_scroll > 0 && r.w > 8) {
    screen.write_text(r.x + r.w - 7, r.y, "[more]", theme::kDim, {});
  }

  // #21: the scrollbar claims the last column when the wrapped content
  // overflows the view. The [more] chip stays: it marks the follow LATCH
  // (auto-scroll armed or not), the bar marks the viewport POSITION -- a box
  // pinned to the bottom has a thumb at the bottom and no chip, and both
  // facts are worth showing. Sign converted at the boundary, per
  // detail/viewport.hpp: the helper's offset is rows past the TOP, while
  // m_scroll counts UP from the bottom.
  //
  // The cw > 0 guard is the NARROW exception, and it resolves the other way
  // from ListWidget's w == 2 (which gives the strip the last column): a
  // 1-wide TextBox keeps its text and drops the bar. The guard exists for
  // the normal case -- a box wide enough for text keeps the strip out of it
  // -- and at 1 wide a position-only box is the worse half of the trade for
  // a widget whose whole job is text (its caller can give it two columns).
  if (total > r.h && cw > 0) {
    const int offset = total - m_scroll - r.h;
    detail::draw_scrollbar(screen, {r.x + r.w - 1, r.y, 1, r.h}, total, offset,
                           r.h, scrollbar_glyphs(m_style), m_track_fg,
                           m_thumb_fg, Rgb{});
  }
  clear_dirty();
}

auto TextBox::allocate_entry(StyledText initial, bool finalized)
    -> TextEntryHandle {
  std::size_t index = 0;
  if (m_free.empty()) {
    index = m_slots.size();
    m_slots.emplace_back();
  } else {
    index = m_free.back();
    m_free.pop_back();
  }
  Slot& slot = m_slots[index];
  slot.entry = Entry{};
  slot.entry->text = std::move(initial);
  slot.entry->finalized = finalized;
  slot.entry->bytes = payload_bytes(*slot.entry);
  m_retained_bytes += slot.entry->bytes;
  m_order.push_back(index);
  return TextEntryHandle{index, slot.generation};
}

auto TextBox::resolve(TextEntryHandle handle) noexcept -> Entry* {
  if (!handle || handle.index >= m_slots.size()) return nullptr;
  Slot& slot = m_slots[handle.index];
  if (slot.generation != handle.generation || !slot.entry) return nullptr;
  return &*slot.entry;
}

auto TextBox::resolve_live(TextEntryHandle handle) noexcept -> Entry* {
  if (!m_live) return nullptr;
  Entry* entry = resolve(handle);
  return entry && !entry->finalized ? entry : nullptr;
}

auto TextBox::payload_bytes(const Entry& entry) -> std::size_t {
  std::size_t bytes = entry.pending_utf8.size();
  for (const TextSpan& span : entry.text) bytes += span.text.size();
  return bytes;
}

auto TextBox::append_clean_span(Entry& entry, std::string text,
                                TextStyle style, bool preserve_empty) -> bool {
  if (text.empty() && !preserve_empty) return false;
  if (!text.empty() && !entry.text.empty() &&
      entry.text.back().style == style && !entry.text.back().text.empty()) {
    entry.text.back().text += text;
  } else {
    entry.text.push_back(TextSpan{std::move(text), style});
  }
  return true;
}

auto TextBox::ingest_chunks(Entry& entry, StyledText chunks) -> bool {
  bool visible_changed = false;

  for (TextSpan& span : chunks) {
    std::string_view remaining = span.text;
    if (!entry.pending_utf8.empty()) {
      const std::size_t expected = detail::utf8_seq_len(
          static_cast<unsigned char>(entry.pending_utf8.front()));
      const std::size_t need = expected - entry.pending_utf8.size();
      const std::size_t take = std::min(need, remaining.size());
      entry.pending_utf8.append(remaining.substr(0, take));
      remaining.remove_prefix(take);
      if (entry.pending_utf8.size() == expected) {
        visible_changed |= append_clean_span(
            entry, Screen::sanitize(entry.pending_utf8), entry.pending_style,
            false);
        entry.pending_utf8.clear();
      } else if (!plausible_utf8_prefix(entry.pending_utf8)) {
        // A non-continuation proves the held lead was malformed now; do not
        // strand an ordinary ASCII byte until another chunk or finalization.
        visible_changed |= append_clean_span(
            entry, Screen::sanitize(entry.pending_utf8), entry.pending_style,
            false);
        entry.pending_utf8.clear();
      }
    }

    if (remaining.empty()) continue;
    const std::size_t pending = incomplete_utf8_suffix(remaining);
    const std::string_view complete = remaining.substr(0, remaining.size() - pending);
    visible_changed |= append_clean_span(
        entry, Screen::sanitize(complete), span.style, !span.text.empty());
    if (pending != 0) {
      entry.pending_utf8.assign(remaining.substr(remaining.size() - pending));
      entry.pending_style = span.style;
    }
  }

  return visible_changed;
}

auto TextBox::append_chunks(Entry& entry, StyledText chunks) -> bool {
  const std::size_t old_bytes = entry.bytes;
  const bool visible_changed = ingest_chunks(entry, std::move(chunks));
  note_entry_change(entry, old_bytes, visible_changed);
  return visible_changed;
}

auto TextBox::replace_chunks(Entry& entry, StyledText chunks) -> bool {
  const std::size_t old_bytes = entry.bytes;
  entry.text.clear();
  entry.pending_utf8.clear();
  const bool visible_changed = ingest_chunks(entry, std::move(chunks));
  note_entry_change(entry, old_bytes, true);
  return visible_changed;
}

auto TextBox::finish_pending(Entry& entry) -> void {
  entry.pending_utf8.clear();
}

auto TextBox::note_entry_change(Entry& entry, std::size_t old_bytes,
                                bool visible_changed) -> void {
  m_retained_bytes -= old_bytes;
  entry.bytes = payload_bytes(entry);
  m_retained_bytes += entry.bytes;
  if (visible_changed) {
    ++entry.content_revision;
    if (entry.content_revision == 0) ++entry.content_revision;
    entry.wrap.valid = false;
    if (m_follow) {
      m_scroll = 0;
      m_anchor.reset();
    }
    mark_dirty();
  }
}

auto TextBox::enforce_retention() -> bool {
  bool evicted = false;
  while (retention_over_budget()) {
    const auto victim = std::find_if(
        m_order.begin(), m_order.end(), [this](std::size_t index) {
          return m_slots[index].entry && m_slots[index].entry->finalized;
        });
    if (victim == m_order.end()) break;
    release_slot(victim);
    evicted = true;
  }
  return evicted;
}

auto TextBox::release_slot(std::deque<std::size_t>::iterator position) -> void {
  const std::size_t index = *position;
  Slot& slot = m_slots[index];
  const TextEntryHandle handle{index, slot.generation};
  m_retained_bytes -= slot.entry->bytes;
  slot.entry.reset();
  ++slot.generation;
  if (slot.generation == 0) ++slot.generation;
  m_free.push_back(index);
  m_order.erase(position);
  if (m_anchor && m_anchor->entry == handle) {
    m_anchor.reset();
    if (!m_follow) m_scroll = std::numeric_limits<int>::max();
  }
}

auto TextBox::ensure_wrapped(Entry& entry, int width)
    -> const std::vector<StyledText>& {
  if (!entry.wrap.valid || entry.wrap.width != width ||
      entry.wrap.content_revision != entry.content_revision ||
      entry.wrap.policy_revision != kWrapPolicyRevision) {
    entry.wrap.rows.clear();
    detail::wrap_styled_into(entry.wrap.rows, entry.text, width);
    entry.wrap.width = width;
    entry.wrap.content_revision = entry.content_revision;
    entry.wrap.policy_revision = kWrapPolicyRevision;
    entry.wrap.valid = true;
    ++m_wrap_build_count;
  }
  return entry.wrap.rows;
}

auto TextBox::wrapped_total(int width) -> std::size_t {
  std::size_t total = 0;
  for (const std::size_t index : m_order)
    total += ensure_wrapped(*m_slots[index].entry, width).size();
  return total;
}

auto TextBox::anchor_from_top(std::size_t top, int width) -> void {
  std::size_t first = 0;
  for (const std::size_t index : m_order) {
    Entry& entry = *m_slots[index].entry;
    const std::size_t count = ensure_wrapped(entry, width).size();
    if (top < first + count) {
      m_anchor = ViewAnchor{
          TextEntryHandle{index, m_slots[index].generation}, top - first};
      return;
    }
    first += count;
  }
  m_anchor.reset();
}

auto TextBox::anchored_top(int width, std::size_t max_top)
    -> std::optional<std::size_t> {
  if (!m_anchor) return std::nullopt;
  std::size_t first = 0;
  for (const std::size_t index : m_order) {
    Entry& entry = *m_slots[index].entry;
    const auto& rows = ensure_wrapped(entry, width);
    const TextEntryHandle handle{index, m_slots[index].generation};
    if (handle == m_anchor->entry) {
      const std::size_t row = rows.empty()
                                  ? 0
                                  : std::min(m_anchor->row, rows.size() - 1);
      return std::min(first + row, max_top);
    }
    first += rows.size();
  }
  m_anchor.reset();
  return std::nullopt;
}

auto TextBox::refresh_anchor_from_scroll() -> void {
  if (m_follow) {
    m_anchor.reset();
    return;
  }
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0) {
    m_anchor.reset();
    return;
  }
  const int width = content_w();
  const std::size_t total = wrapped_total(width);
  const std::size_t max_scroll =
      total > static_cast<std::size_t>(r.h)
          ? total - static_cast<std::size_t>(r.h)
          : 0;
  const std::size_t scroll = std::min<std::size_t>(
      static_cast<std::size_t>(m_scroll), max_scroll);
  m_scroll = static_cast<int>(scroll);
  m_follow = (m_scroll == 0);
  if (m_follow) {
    m_anchor.reset();
    return;
  }
  const std::size_t bottom = total - scroll;
  const std::size_t top =
      bottom > static_cast<std::size_t>(r.h)
          ? bottom - static_cast<std::size_t>(r.h)
          : 0;
  anchor_from_top(top, width);
}

}  // namespace termforge
