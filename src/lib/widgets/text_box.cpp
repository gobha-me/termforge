#include "termforge/widgets/text_box.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "detail/utf8.hpp"
#include "detail/wrap.hpp"
#include "termforge/core/screen.hpp"
#include "termforge/widgets/detail/scrollbar.hpp"
#include "termforge/widgets/detail/viewport.hpp"
#include "termforge/widgets/theme.hpp"

namespace termforge {
namespace {

std::atomic<std::uint64_t> g_next_block_owner{1};

auto next_block_owner() -> std::uint64_t {
  auto id = g_next_block_owner.load(std::memory_order_relaxed);
  while (id != std::numeric_limits<std::uint64_t>::max()) {
    if (g_next_block_owner.compare_exchange_weak(
            id, id + 1, std::memory_order_relaxed, std::memory_order_relaxed))
      return id;
  }
  throw std::overflow_error{"termforge: TextBox block owner space exhausted"};
}

auto block_error(std::string message) -> std::unexpected<ErrorEvent> {
  return std::unexpected{
      ErrorEvent{Severity::Warning, "textbox", std::move(message)}};
}

// Wrapped rows fit their width, except for the progress-preserving case of
// one wide glyph in a one-column row. Pad that row instead of leaking into a
// neighboring widget. Screen still supplies sanitization and screen clipping.
auto paint_text_row(Screen& screen, int x, int y, const StyledText& row,
                    int width) -> void {
  if (width == 1) {
    for (const auto& span : row) {
      if (detail::display_width(span.text) > 1) {
        screen.write_text(x, y, " ", span.style.fg, span.style.bg,
                          span.style.attrs);
        return;
      }
    }
  }
  screen.write_styled(x, y, row);
}

// Preserve the full track's thumb geometry, but iterate only on-screen rows.
// A caller may reserve INT_MAX block rows without allocating or walking them.
auto paint_textbox_scrollbar(Screen& screen, Rect track, int total, int offset,
                             ScrollGlyphs glyphs, Rgb track_fg, Rgb thumb_fg)
    -> void {
  const auto [start, length] =
      detail::thumb_window(track.h, total, offset, track.h);
  const auto first = std::max(std::int64_t{0}, std::int64_t{track.y});
  const auto end =
      std::min(std::int64_t{screen.rows()}, std::int64_t{track.y} + track.h);
  for (auto y = first; y < end; ++y) {
    const auto row = y - track.y;
    const bool thumb = row >= start && row < std::int64_t{start} + length;
    screen.write_text(track.x, static_cast<int>(y),
                      thumb ? glyphs.thumb : glyphs.track,
                      thumb ? thumb_fg : track_fg, {});
  }
}

// Default style for the plain-string append path — matches the colours draw()
// historically hard-coded (theme fg, zeroed bg, no attrs).
[[nodiscard]] auto plain_style() noexcept -> TextStyle {
  return TextStyle{theme::kFg, Rgb{}, Attr::None};
}

auto sanitize_spans(StyledText& line) -> void {
  for (TextSpan& span : line)
    span.text = Screen::sanitize(span.text);
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

} // namespace

TextBox::BlockOwner::BlockOwner() : id(next_block_owner()) {
}
TextBox::BlockOwner::BlockOwner(const BlockOwner&) : BlockOwner() {
}
TextBox::BlockOwner::BlockOwner(BlockOwner&& other) : BlockOwner() {
  other.id = next_block_owner();
}
auto TextBox::BlockOwner::operator=(const BlockOwner& other) -> BlockOwner& {
  if (this != &other) id = next_block_owner();
  return *this;
}
auto TextBox::BlockOwner::operator=(BlockOwner&& other) -> BlockOwner& {
  if (this != &other) {
    id = next_block_owner();
    other.id = next_block_owner();
  }
  return *this;
}

TextBox::TextBox(TextBox&& other)
    : Widget(other), m_block_owner(std::move(other.m_block_owner)),
      m_slots(std::move(other.m_slots)), m_order(std::move(other.m_order)),
      m_free(std::move(other.m_free)), m_live(other.m_live),
      m_anchor(other.m_anchor), m_scroll(other.m_scroll),
      m_follow(other.m_follow), m_retention(other.m_retention),
      m_retained_bytes(other.m_retained_bytes),
      m_wrap_build_count(other.m_wrap_build_count),
      m_block_limits(other.m_block_limits), m_block_count(other.m_block_count),
      m_block_rows(other.m_block_rows), m_style(other.m_style),
      m_track_fg(other.m_track_fg), m_thumb_fg(other.m_thumb_fg) {
  // Reinitialize the moved-from document counters as well as its containers.
  other.clear();
}

auto TextBox::operator=(TextBox&& other) -> TextBox& {
  if (this == &other) return *this;
  Widget::operator=(other);
  m_block_owner = std::move(other.m_block_owner);
  m_slots = std::move(other.m_slots);
  m_order = std::move(other.m_order);
  m_free = std::move(other.m_free);
  m_live = other.m_live;
  m_anchor = other.m_anchor;
  m_scroll = other.m_scroll;
  m_follow = other.m_follow;
  m_retention = other.m_retention;
  m_retained_bytes = other.m_retained_bytes;
  m_wrap_build_count = other.m_wrap_build_count;
  m_block_limits = other.m_block_limits;
  m_block_count = other.m_block_count;
  m_block_rows = other.m_block_rows;
  m_layout = {};
  m_style = other.m_style;
  m_track_fg = other.m_track_fg;
  m_thumb_fg = other.m_thumb_fg;
  other.clear();
  return *this;
}

auto TextBox::append(std::string line) -> void {
  // Single-span compatibility wrapper over the styled document path (#25).
  append(plain_text(std::move(line)));
}

auto TextBox::append(StyledText line) -> void {
  m_layout.valid = false;
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
    m_scroll = std::numeric_limits<std::size_t>::max();
  mark_dirty();
}

auto TextBox::begin_entry() -> TextEntryHandle {
  return begin_entry(StyledText{});
}

auto TextBox::begin_entry(std::string initial) -> TextEntryHandle {
  return begin_entry(plain_text(std::move(initial)));
}

auto TextBox::begin_entry(StyledText initial) -> TextEntryHandle {
  m_layout.valid = false;
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
    m_scroll = std::numeric_limits<std::size_t>::max();
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
  for (const TextSpan& span : chunk)
    any_bytes |= !span.text.empty();
  if (!any_bytes) return true;

  (void)append_chunks(*entry, std::move(chunk));
  const bool evicted = enforce_retention();
  if (evicted) mark_dirty();
  if (evicted && !m_follow && !m_anchor)
    m_scroll = std::numeric_limits<std::size_t>::max();
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
    m_scroll = std::numeric_limits<std::size_t>::max();
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

auto TextBox::resolve_block(TextBlockHandle handle) const noexcept
    -> const Entry* {
  if (!handle || handle.owner != m_block_owner.id ||
      handle.index >= m_slots.size())
    return nullptr;
  const auto& slot = m_slots[handle.index];
  if (slot.generation != handle.generation || !slot.entry ||
      !slot.entry->block_rows)
    return nullptr;
  return &*slot.entry;
}

auto TextBox::validate_block_rows(int rows, int previous) const
    -> std::expected<void, ErrorEvent> {
  if (rows < 0 || rows > m_block_limits.max_rows_per_block)
    return block_error("block height is outside the configured row budget");
  const auto remaining = m_block_rows - static_cast<std::size_t>(previous);
  if (static_cast<std::size_t>(rows) >
      m_block_limits.max_total_rows - remaining)
    return block_error("block would exceed the aggregate row budget");
  return {};
}

auto TextBox::set_block_limits(TextBoxBlockLimits limits)
    -> std::expected<void, ErrorEvent> {
  const auto maximum =
      static_cast<std::size_t>(std::numeric_limits<int>::max());
  if (limits.max_rows_per_block < 0 || limits.max_blocks > maximum ||
      limits.max_total_rows > maximum || limits.max_blocks < m_block_count ||
      limits.max_total_rows < m_block_rows)
    return block_error("invalid block limits or limits below current use");
  for (const auto index : m_order) {
    const auto& entry = *m_slots[index].entry;
    if (entry.block_rows && *entry.block_rows > limits.max_rows_per_block)
      return block_error("per-block row limit is below current use");
  }
  m_block_limits = limits;
  return {};
}

auto TextBox::append_block(int rows, StyledText fallback)
    -> std::expected<TextBlockHandle, ErrorEvent> {
  if (m_block_count >= m_block_limits.max_blocks)
    return block_error("block count budget exhausted");
  if (const auto valid = validate_block_rows(rows, 0); !valid)
    return std::unexpected{valid.error()};
  sanitize_spans(fallback);
  std::size_t bytes = 0;
  for (const auto& span : fallback) {
    if (span.text.size() > std::numeric_limits<std::size_t>::max() - bytes)
      return block_error("block fallback byte count overflows");
    bytes += span.text.size();
  }
  if (bytes > std::numeric_limits<std::size_t>::max() - m_retained_bytes)
    return block_error("retained block byte count overflows");

  if (m_live) (void)finalize_entry(*m_live);
  const auto entry = allocate_entry(std::move(fallback), true);
  m_slots[entry.index].entry->block_rows = rows;
  ++m_block_count;
  m_block_rows += static_cast<std::size_t>(rows);
  m_layout.valid = false;
  if (m_follow) m_scroll = 0;
  (void)enforce_retention();
  mark_dirty();
  return TextBlockHandle{entry.index, entry.generation, m_block_owner.id};
}

auto TextBox::update_block(TextBlockHandle handle, int rows,
                           StyledText fallback)
    -> std::expected<void, ErrorEvent> {
  const auto* current = resolve_block(handle);
  if (!current) return block_error("empty, stale, or foreign block handle");
  if (const auto valid = validate_block_rows(rows, *current->block_rows);
      !valid)
    return std::unexpected{valid.error()};
  sanitize_spans(fallback);
  std::size_t bytes = 0;
  for (const auto& span : fallback) {
    if (span.text.size() > std::numeric_limits<std::size_t>::max() - bytes)
      return block_error("block fallback byte count overflows");
    bytes += span.text.size();
  }
  if (bytes > std::numeric_limits<std::size_t>::max() -
                  (m_retained_bytes - current->bytes))
    return block_error("retained block byte count overflows");

  Entry& entry = *m_slots[handle.index].entry;
  m_block_rows -= static_cast<std::size_t>(*entry.block_rows);
  m_block_rows += static_cast<std::size_t>(rows);
  entry.block_rows = rows;
  const auto old_bytes = entry.bytes;
  entry.text = std::move(fallback);
  note_entry_change(entry, old_bytes, true);
  (void)enforce_retention();
  return {};
}

auto TextBox::set_block_rows(TextBlockHandle handle, int rows)
    -> std::expected<void, ErrorEvent> {
  const auto* current = resolve_block(handle);
  if (!current) return block_error("empty, stale, or foreign block handle");
  if (const auto valid = validate_block_rows(rows, *current->block_rows);
      !valid)
    return std::unexpected{valid.error()};
  if (*current->block_rows == rows) return {};
  m_block_rows -= static_cast<std::size_t>(*current->block_rows);
  m_block_rows += static_cast<std::size_t>(rows);
  m_slots[handle.index].entry->block_rows = rows;
  m_layout.valid = false;
  if (m_follow) m_scroll = 0;
  mark_dirty();
  return {};
}

auto TextBox::remove_block(TextBlockHandle handle)
    -> std::expected<void, ErrorEvent> {
  if (!resolve_block(handle))
    return block_error("empty, stale, or foreign block handle");
  release_slot(std::find(m_order.begin(), m_order.end(), handle.index));
  mark_dirty();
  return {};
}

auto TextBox::block_geometry(TextBlockHandle handle) const
    -> std::expected<TextBlockGeometry, ErrorEvent> {
  const auto* block = resolve_block(handle);
  if (!block) return block_error("empty, stale, or foreign block handle");
  TextBlockGeometry result;
  if (!m_layout.valid || m_layout.owner != m_block_owner.id ||
      m_layout.widget != rect())
    return result;
  if (m_layout.viewport.empty()) {
    result.state = TextBlockLayoutState::Empty;
    return result;
  }
  std::size_t first = 0;
  for (const auto index : m_order) {
    if (index == handle.index) break;
    const auto& entry = *m_slots[index].entry;
    if (entry.block_rows) {
      first += static_cast<std::size_t>(*entry.block_rows);
    } else {
      const auto& cache = entry.wrap.width == m_layout.width
                              ? entry.wrap
                              : entry.alternate_wrap;
      first += cache.rows.size();
    }
  }
  const std::int64_t y = std::int64_t{rect().y} +
                         static_cast<std::int64_t>(first) -
                         static_cast<std::int64_t>(m_layout.top);
  result.allocation =
      TextBlockAllocation{rect().x, y, m_layout.width, *block->block_rows};
  result.state = *block->block_rows == 0 ? TextBlockLayoutState::Empty
                                         : TextBlockLayoutState::Offscreen;
  const Rect v = m_layout.viewport;
  const auto top = std::max(y, std::int64_t{v.y});
  const auto bottom = std::min(y + *block->block_rows, std::int64_t{v.y} + v.h);
  if (top < bottom) {
    result.state = TextBlockLayoutState::Visible;
    result.visible = {v.x, static_cast<int>(top), v.w,
                      static_cast<int>(bottom - top)};
    result.source_row = static_cast<int>(top - y);
  }
  return result;
}

auto TextBox::set_retention(TextBoxRetention retention) -> void {
  if (m_retention == retention) return;
  m_retention = retention;
  if (enforce_retention()) mark_dirty();
}

auto TextBox::retention_over_budget() const noexcept -> bool {
  return (m_retention.max_entries &&
          m_order.size() > *m_retention.max_entries) ||
         (m_retention.max_bytes && m_retained_bytes > *m_retention.max_bytes);
}

auto TextBox::clear() -> void {
  m_layout.valid = false;
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
  m_block_count = 0;
  m_block_rows = 0;
  m_scroll = 0;
  m_follow = true;
  mark_dirty();
}

auto TextBox::at_bottom() const noexcept -> bool {
  return m_scroll == 0;
}

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
  if (delta < 0) {
    const auto up = static_cast<std::size_t>(-std::int64_t{delta});
    m_scroll +=
        std::min(up, std::numeric_limits<std::size_t>::max() - m_scroll);
  } else {
    m_scroll -= std::min(m_scroll, static_cast<std::size_t>(delta));
  }
  m_layout.valid = false;
  m_follow = (m_scroll == 0);
  refresh_anchor_from_scroll();
  mark_dirty();
}

auto TextBox::scroll_to_bottom() -> void {
  m_layout.valid = false;
  m_scroll = 0;
  m_follow = true;
  m_anchor.reset();
  mark_dirty();
}

auto TextBox::on_event(const Event& ev) -> bool {
  if (const auto* k = std::get_if<KeyEvent>(&ev)) {
    if (k->key == Key::PageUp) {
      scroll(-(rect().h > 1 ? rect().h - 1 : 1));
      return true;
    }
    if (k->key == Key::PageDown) {
      scroll(rect().h > 1 ? rect().h - 1 : 1);
      return true;
    }
  }
  if (const auto* m = std::get_if<MouseEvent>(&ev)) {
    // #35 Q1: wheel scrolls the VIEW (TextBox has no selection to move). The
    // step is the shared kWheelStep; scroll() owns the sign inversion.
    if (m->scroll_up) {
      scroll(-detail::kWheelStep);
      return true;
    }
    if (m->scroll_down) {
      scroll(detail::kWheelStep);
      return true;
    }
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
  if (m_layout.valid && m_layout.owner == m_block_owner.id &&
      m_layout.widget == rect())
    return m_layout.width;
  const std::size_t minimum_rows =
      m_order.size() - m_block_count + m_block_rows;
  const bool bar = w > 1 && rect().h > 0 &&
                   minimum_rows > static_cast<std::size_t>(rect().h);
  return w - (bar ? 1 : 0);
}

auto TextBox::layout_width() -> int {
  const Rect r = rect();
  if (r.w <= 1 || r.h <= 0) return std::max(0, r.w);
  const std::size_t minimum_rows =
      m_order.size() - m_block_count + m_block_rows;
  if (minimum_rows > static_cast<std::size_t>(r.h) ||
      wrapped_total(r.w) > static_cast<std::size_t>(r.h))
    return r.w - 1;
  return r.w;
}

auto TextBox::draw(Screen& screen) -> void {
  m_layout.valid = false;
  const Rect r = rect();
  if (r.w <= 0 || r.h <= 0) {
    m_layout = Layout{r, {}, 0, std::max(0, r.w), m_block_owner.id, true};
    clear_dirty();
    return;
  }

  const Rgb fg = theme::kFg;
  // Own the whole rect: blank it every frame so clear()/scroll/shrink can't
  // leave stale text behind (immediate-mode contract, see widget.hpp).
  screen.fill_rect(r.x, r.y, r.w, r.h, fg, {});

  // Resolve the actual width before painting or publishing child geometry.
  // If full-width wrapping overflows, reflow with the scrollbar reserved;
  // keeping both width caches avoids rebuilding that pair every frame.
  const int cw = layout_width();
  const std::size_t total_size = wrapped_total(cw);
  const int total = static_cast<int>(std::min<std::size_t>(
      total_size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
  // Clamp the scroll offset now that the wrapped line count is known --
  // scroll() can't bound it (content may have changed since). m_scroll counts
  // UP from the bottom (inverted, see scroll()), but the bounds are symmetric:
  // the valid range is [0, max(0, total - h)] in either convention.
  const std::size_t max_top = total_size > static_cast<std::size_t>(r.h)
                                  ? total_size - static_cast<std::size_t>(r.h)
                                  : 0;
  if (!m_follow) {
    if (const auto top = anchored_top(cw, max_top)) {
      const std::size_t visible_end =
          std::min(total_size, *top + static_cast<std::size_t>(r.h));
      const std::size_t from_bottom = total_size - visible_end;
      m_scroll = from_bottom;
    }
  }
  m_scroll = std::min(m_scroll, max_top);
  m_follow = (m_scroll == 0);
  const std::size_t bottom = total_size - m_scroll;
  const std::size_t top = bottom > static_cast<std::size_t>(r.h)
                              ? bottom - static_cast<std::size_t>(r.h)
                              : 0;

  const Rect viewport = Rect{r.x, r.y, cw, r.h}.intersect(
      Rect{0, 0, screen.cols(), screen.rows()});
  m_layout = Layout{r, viewport, top, cw, m_block_owner.id, false};
  const auto clipped_top =
      viewport.empty()
          ? 0
          : top + static_cast<std::size_t>(std::int64_t{viewport.y} - r.y);
  const auto clipped_bottom =
      clipped_top + static_cast<std::size_t>(viewport.h);
  std::size_t flat = 0;
  for (const std::size_t slot_index : m_order) {
    Entry& entry = *m_slots[slot_index].entry;
    const auto count = entry_rows(entry, cw);
    const auto first = std::max(flat, clipped_top);
    const auto end = std::min(flat + count, clipped_bottom);
    if (!viewport.empty() && first < end) {
      const auto& rows = ensure_wrapped(entry, cw);
      for (auto row = first; row < end && row - flat < rows.size(); ++row) {
        const auto y = std::int64_t{r.y} + static_cast<std::int64_t>(row - top);
        paint_text_row(screen, r.x, static_cast<int>(y), rows[row - flat], cw);
      }
    }
    flat += count;
    if (flat >= bottom) break;
  }

  if (m_follow)
    m_anchor.reset();
  else
    anchor_from_top(top, cw);

  // scroll indicator when not at the bottom
  const auto more_x = std::int64_t{r.x} + r.w - 7;
  if (m_scroll > 0 && r.w > 8 && !viewport.empty() &&
      more_x <= std::numeric_limits<int>::max()) {
    screen.write_text(static_cast<int>(more_x), r.y, "[more]", theme::kDim, {});
  }

  // #21: the scrollbar claims the last column when the wrapped content
  // overflows the view. The [more] chip stays: it marks the follow LATCH
  // (auto-scroll armed or not), the bar marks the viewport POSITION -- a box
  // pinned to the bottom has a thumb at the bottom and no chip, and both
  // facts are worth showing. Sign converted at the boundary, per
  // detail/viewport.hpp: the helper's offset is rows past the TOP, while
  // m_scroll counts UP from the bottom.
  //
  // One-column views keep their content and drop the bar. Compute its column
  // wide and clip before narrowing, just like the block's full allocation.
  const auto bar_x = std::int64_t{r.x} + r.w - 1;
  if (total_size > static_cast<std::size_t>(r.h) && r.w > 1 && bar_x >= 0 &&
      bar_x < screen.cols() && !viewport.empty()) {
    const auto offset = static_cast<int>(
        std::min(top, static_cast<std::size_t>(std::max(0, total - r.h))));
    paint_textbox_scrollbar(screen, {static_cast<int>(bar_x), r.y, 1, r.h},
                            total, offset, scrollbar_glyphs(m_style),
                            m_track_fg, m_thumb_fg);
  }
  m_layout.valid = true;
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
  for (const TextSpan& span : entry.text)
    bytes += span.text.size();
  return bytes;
}

auto TextBox::append_clean_span(Entry& entry, std::string text, TextStyle style,
                                bool preserve_empty) -> bool {
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
        visible_changed |=
            append_clean_span(entry, Screen::sanitize(entry.pending_utf8),
                              entry.pending_style, false);
        entry.pending_utf8.clear();
      } else if (!plausible_utf8_prefix(entry.pending_utf8)) {
        // A non-continuation proves the held lead was malformed now; do not
        // strand an ordinary ASCII byte until another chunk or finalization.
        visible_changed |=
            append_clean_span(entry, Screen::sanitize(entry.pending_utf8),
                              entry.pending_style, false);
        entry.pending_utf8.clear();
      }
    }

    if (remaining.empty()) continue;
    const std::size_t pending = incomplete_utf8_suffix(remaining);
    const std::string_view complete =
        remaining.substr(0, remaining.size() - pending);
    visible_changed |= append_clean_span(entry, Screen::sanitize(complete),
                                         span.style, !span.text.empty());
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
    m_layout.valid = false;
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
    const auto victim =
        std::find_if(m_order.begin(), m_order.end(), [this](std::size_t index) {
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
  if (slot.entry->block_rows) {
    --m_block_count;
    m_block_rows -= static_cast<std::size_t>(*slot.entry->block_rows);
  }
  m_layout.valid = false;
  slot.entry.reset();
  ++slot.generation;
  if (slot.generation == 0) ++slot.generation;
  m_free.push_back(index);
  m_order.erase(position);
  if (m_anchor && m_anchor->entry == handle) {
    m_anchor.reset();
    if (!m_follow) m_scroll = std::numeric_limits<std::size_t>::max();
  }
}

auto TextBox::ensure_wrapped(Entry& entry, int width)
    -> const std::vector<StyledText>& {
  // Two-entry LRU: widths after a resize replace BOTH old widths, rather than
  // thrashing a single alternate while the first-ever width stays protected.
  if (entry.wrap.width != width) std::swap(entry.wrap, entry.alternate_wrap);
  auto& cache = entry.wrap;
  if (!cache.valid || cache.width != width ||
      cache.content_revision != entry.content_revision ||
      cache.policy_revision != kWrapPolicyRevision) {
    cache.rows.clear();
    detail::wrap_styled_into(cache.rows, entry.text, width);
    cache.width = width;
    cache.content_revision = entry.content_revision;
    cache.policy_revision = kWrapPolicyRevision;
    cache.valid = true;
    ++m_wrap_build_count;
  }
  return cache.rows;
}

auto TextBox::entry_rows(Entry& entry, int width) -> std::size_t {
  return entry.block_rows ? static_cast<std::size_t>(*entry.block_rows)
                          : ensure_wrapped(entry, width).size();
}

auto TextBox::wrapped_total(int width) -> std::size_t {
  std::size_t total = 0;
  for (const std::size_t index : m_order)
    total += entry_rows(*m_slots[index].entry, width);
  return total;
}

auto TextBox::anchor_from_top(std::size_t top, int width) -> void {
  std::size_t first = 0;
  for (const std::size_t index : m_order) {
    Entry& entry = *m_slots[index].entry;
    const std::size_t count = entry_rows(entry, width);
    if (top < first + count) {
      m_anchor = ViewAnchor{TextEntryHandle{index, m_slots[index].generation},
                            top - first};
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
    const auto count = entry_rows(entry, width);
    const TextEntryHandle handle{index, m_slots[index].generation};
    if (handle == m_anchor->entry) {
      const std::size_t row =
          count == 0 ? 0 : std::min(m_anchor->row, count - 1);
      return std::min(first + row, max_top);
    }
    first += count;
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
  const int width = layout_width();
  const std::size_t total = wrapped_total(width);
  const std::size_t max_scroll = total > static_cast<std::size_t>(r.h)
                                     ? total - static_cast<std::size_t>(r.h)
                                     : 0;
  const std::size_t scroll =
      std::min<std::size_t>(static_cast<std::size_t>(m_scroll), max_scroll);
  m_scroll = scroll;
  m_follow = (m_scroll == 0);
  if (m_follow) {
    m_anchor.reset();
    return;
  }
  const std::size_t bottom = total - scroll;
  const std::size_t top = bottom > static_cast<std::size_t>(r.h)
                              ? bottom - static_cast<std::size_t>(r.h)
                              : 0;
  anchor_from_top(top, width);
}

} // namespace termforge
