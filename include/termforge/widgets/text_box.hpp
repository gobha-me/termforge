#pragma once

// TermForge — TextBox: a scrolling multi-line text area (chat-scrollback
// style). Finalized lines, bounded embedded blocks and one mutable streaming
// tail share the same document; the view shows the latest rows that fit its
// rect, auto-scrolling to the bottom on new content unless the user has
// scrolled up. Supports bounded retention, manual scroll (PageUp/PageDown /
// scroll wheel) and display-width-aware word wrapping across styled spans
// (#24/#25), with hard wrapping only for an unbroken run wider than the widget.
// This is the foundation of a chat message or live-log view.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "termforge/core/styled_text.hpp"
#include "termforge/widgets/glyphs.hpp"
#include "termforge/widgets/theme.hpp"
#include "termforge/widgets/widget.hpp"

namespace termforge {

// Stable identity for one TextBox document entry (#217). Slots are recycled,
// generations are not: a stale handle can never mutate the entry that later
// inherited its index. Default construction is the empty handle.
struct TextEntryHandle {
  std::size_t index{0};
  std::uint64_t generation{0};

  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return generation != 0;
  }
  constexpr auto operator==(const TextEntryHandle&) const noexcept
      -> bool = default;
};

// Optional logical-entry and source-byte limits. nullopt means unlimited; zero
// is a real limit. The mutable live tail is never evicted, so
// retention_over_budget() can remain true until it is finalized or the limits
// are relaxed.
struct TextBoxRetention {
  std::optional<std::size_t> max_entries{};
  std::optional<std::size_t> max_bytes{};

  constexpr auto operator==(const TextBoxRetention&) const noexcept
      -> bool = default;
};

// Owner-qualified document identity (#354). Unlike a slot number, this never
// names a block in another TextBox, a copy, or a subsequently assigned box.
// Handles do not transfer on copy/move. Assignment invalidates the
// destination's handles; move also invalidates the source's. Keep a box at a
// stable address.
struct TextBlockHandle {
  std::size_t index{0};
  std::uint64_t generation{0};
  std::uint64_t owner{0};

  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return generation != 0 && owner != 0;
  }
  constexpr auto operator==(const TextBlockHandle&) const noexcept
      -> bool = default;
};

// Hard admission budgets, independent of oldest-first retention. Zero is a
// real budget; lowering a limit below current use refuses without eviction.
// Counts/aggregate rows cannot exceed INT_MAX. Heights are cells, not pixels.
struct TextBoxBlockLimits {
  std::size_t max_blocks{256};
  int max_rows_per_block{4096};
  std::size_t max_total_rows{65536};

  constexpr auto operator==(const TextBoxBlockLimits&) const noexcept
      -> bool = default;
};

enum class TextBlockLayoutState { NotLaidOut, Empty, Offscreen, Visible };

// Full cell allocation relative to the current screen, BEFORE clipping.
// Its y is deliberately wide: a long document can start far above the screen.
// Only TextBlockGeometry::visible is a Rect to give a child Widget/driver.
struct TextBlockAllocation {
  int x{0};
  std::int64_t y{0};
  int w{0}, h{0};

  constexpr auto operator==(const TextBlockAllocation&) const noexcept
      -> bool = default;
};

struct TextBlockGeometry {
  TextBlockLayoutState state{TextBlockLayoutState::NotLaidOut};
  std::optional<TextBlockAllocation> allocation{};
  Rect visible{};
  int source_row{0}; // first visible row inside the full block, never pixels
};

class TextBox final : public Widget {
 public:
  TextBox() = default;
  TextBox(const TextBox&) = default;
  auto operator=(const TextBox&) -> TextBox& = default;
  TextBox(TextBox&& other);
  auto operator=(TextBox&& other) -> TextBox&;

  // Append a logical line (a chat message, a log entry). Long lines wrap at
  // the last fitting space, falling back to a display-width-safe hard split
  // for an overlong word. Source whitespace is preserved. Marks the widget
  // dirty and auto-scrolls to the bottom if the user is already at the bottom.
  // Plain text inherits current Theme content roles, or historical default
  // colors when opted out. Sanitization runs here (#25); provenance is kept
  // so later snapshots never infer caller intent from color equality.
  auto append(std::string line) -> void;

  // Append a styled logical line. Each span's text is sanitized at this
  // boundary (styles are data, never escape codes). Empty spans are retained
  // in the document but paint nothing.
  auto append(StyledText line) -> void;

  // Begin the one mutable tail and return its stable handle. Beginning a new
  // entry finalizes the previous tail first. Unlike append(), the initial
  // payload stays mutable until finalize_entry(); an incomplete trailing UTF-8
  // sequence is held for a later chunk instead of being discarded.
  [[nodiscard]] auto begin_entry() -> TextEntryHandle;
  [[nodiscard]] auto begin_entry(std::string initial) -> TextEntryHandle;
  [[nodiscard]] auto begin_entry(StyledText initial) -> TextEntryHandle;

  // Mutate the live tail. Plain chunks inherit the same content roles as the
  // compatibility append(string) path; styled chunks retain authored styles.
  // A UTF-8 sequence split across styled chunks inherits the style of its
  // lead byte, so completing it cannot recolour half of one code point.
  // Empty chunks are successful no-ops. False means the handle is empty,
  // stale, finalized, or no longer names the current live tail; no replacement
  // entry is ever touched on failure.
  [[nodiscard]] auto append_to_entry(TextEntryHandle entry, std::string chunk)
      -> bool;
  [[nodiscard]] auto append_to_entry(TextEntryHandle entry, StyledText chunk)
      -> bool;
  [[nodiscard]] auto replace_entry(TextEntryHandle entry, std::string text)
      -> bool;
  [[nodiscard]] auto replace_entry(TextEntryHandle entry, StyledText text)
      -> bool;

  // Make the live tail immutable and eligible for retention eviction. Any
  // incomplete UTF-8 suffix is dropped under the existing Strip sanitization
  // contract. A successful finalization may immediately evict this entry when
  // a zero/tight retention limit requires it.
  [[nodiscard]] auto finalize_entry(TextEntryHandle entry) -> bool;

  // Append a finalized, fixed-height document slot, finalizing any live tail.
  // Fallback is sanitized/wrapped like text, clipped to the reserved rows;
  // remaining rows are blank. Zero rows explicitly collapses the slot without
  // discarding its fallback. Blocks count as entries, and ALL stored fallback
  // bytes count toward retention (including collapsed/clipped text). Retention
  // can immediately evict a successful insertion, making its handle stale.
  // Invalid height/budget/handle operations return Warning before mutation.
  [[nodiscard]] auto append_block(int rows, StyledText fallback = {})
      -> std::expected<TextBlockHandle, ErrorEvent>;
  [[nodiscard]] auto update_block(TextBlockHandle block, int rows,
                                  StyledText fallback)
      -> std::expected<void, ErrorEvent>;
  [[nodiscard]] auto set_block_rows(TextBlockHandle block, int rows)
      -> std::expected<void, ErrorEvent>;
  [[nodiscard]] auto remove_block(TextBlockHandle block)
      -> std::expected<void, ErrorEvent>;
  [[nodiscard]] auto set_block_limits(TextBoxBlockLimits limits)
      -> std::expected<void, ErrorEvent>;
  [[nodiscard]] auto block_limits() const noexcept -> TextBoxBlockLimits {
    return m_block_limits;
  }
  [[nodiscard]] auto block_count() const noexcept -> std::size_t {
    return m_block_count;
  }
  [[nodiscard]] auto block_rows() const noexcept -> std::size_t {
    return m_block_rows;
  }

  // Value snapshot of the last draw's ACTUAL wrap/scrollbar layout, clipped
  // to both the content viewport and that draw's Screen. Query after draw and
  // before positioning/collecting child pixels. Mutations, scroll and changed
  // Widget::rect() yield NotLaidOut; invalid handles yield Warning, not
  // geometry. Empty means collapsed or zero-size viewport (no allocation in the
  // latter case); Offscreen has an allocation but no visible Rect. Visible's
  // Rect is also the hit-test area; source_row maps it to the unclipped
  // content. All operations, including queries/draw, belong to the box's owner
  // thread.
  [[nodiscard]] auto block_geometry(TextBlockHandle block) const
      -> std::expected<TextBlockGeometry, ErrorEvent>;

  // Apply both limits immediately, evicting oldest finalized entries first.
  // Byte accounting covers the bytes stored in TextSpan strings plus a held
  // incomplete UTF-8 suffix; container allocation overhead is not claimed.
  auto set_retention(TextBoxRetention retention) -> void;
  [[nodiscard]] auto retention() const noexcept -> TextBoxRetention {
    return m_retention;
  }
  [[nodiscard]] auto retained_bytes() const noexcept -> std::size_t {
    return m_retained_bytes;
  }
  [[nodiscard]] auto retention_over_budget() const noexcept -> bool;

  // Deterministic cache observation for applications/tests. Counts per-entry
  // wrap builds over this TextBox's lifetime; cache hits never increment it.
  [[nodiscard]] auto wrap_build_count() const noexcept -> std::uint64_t {
    return m_wrap_build_count;
  }

  // Replace all content.
  auto clear() -> void;

  // Scroll the view. positive = toward newer (down), negative = older (up).
  auto scroll(int delta) -> void;
  auto scroll_to_bottom() -> void;

  // Event handling: PageUp/PageDown scroll a page; scroll wheel scrolls.
  auto on_event(const Event& ev) -> bool override;

  auto draw(Screen& screen) -> void override;

  [[nodiscard]] auto line_count() const noexcept -> std::size_t {
    return m_order.size();
  }
  [[nodiscard]] auto at_bottom() const noexcept -> bool;

  // Which glyph family #21's scrollbar comes from. TextBox has no other
  // glyph need, so this knob exists purely for the bar: an app holding one
  // BorderStyle passes it here too, and BorderStyle::Ascii is what keeps the
  // strip 7-bit on a bare TTY. Same convention as ListWidget/TableWidget.
  auto set_style(BorderStyle style) -> void {
    m_style_override = true;
    m_style = style;
    mark_dirty();
  }
  [[nodiscard]] auto style() const noexcept -> BorderStyle { return m_style; }

  // Scrollbar colours (#21): the │ track and the █ thumb.
  auto set_scrollbar_colors(Rgb track_fg, Rgb thumb_fg) -> void {
    m_scrollbar_override = true;
    m_track_fg = track_fg;
    m_thumb_fg = thumb_fg;
    mark_dirty();
  }

  // Actual width after the current draw; otherwise advisory until wrapping
  // decides whether the scrollbar claims a column. One-column views keep
  // content and omit the bar. Use block_geometry, not this hint, for children.
  [[nodiscard]] auto content_w() const noexcept -> int;

 private:
  auto on_theme_changed() -> void override;
  // Provenance belongs to each stored span, not a color comparison or a
  // parallel vector that could become misaligned after an allocation failure.
  struct ContentSpan {
    std::string text;
    TextStyle style;
    bool content_roles{false};
  };
  static auto content_spans(StyledText text, bool content_roles)
      -> std::vector<ContentSpan>;
  auto append_line(StyledText line, bool content_roles) -> void;
  auto begin_tail(StyledText initial, bool content_roles) -> TextEntryHandle;
  auto append_tail(TextEntryHandle handle, StyledText chunk, bool content_roles)
      -> bool;
  auto replace_tail(TextEntryHandle handle, StyledText text, bool content_roles)
      -> bool;
  struct WrapCache {
    std::vector<StyledText> rows;
    std::uint64_t content_revision{0};
    std::uint32_t policy_revision{0};
    int width{0};
    bool valid{false};
  };

  struct Entry {
    std::vector<ContentSpan> text;
    std::string pending_utf8;
    TextStyle pending_style{};
    bool pending_content_roles{false};
    std::size_t bytes{0};
    std::uint64_t content_revision{1};
    bool finalized{false};
    std::optional<int> block_rows{}; // engaged even for a collapsed block
    WrapCache wrap;
    WrapCache alternate_wrap; // full/bar widths can both be cached
  };

  struct Slot {
    std::optional<Entry> entry;
    std::uint64_t generation{1};
  };

  struct ViewAnchor {
    TextEntryHandle entry;
    std::size_t row{0};
  };

  // TextBox copying/moving must not copy a handle namespace. The
  // moved-from namespace changes too, before its slots can be reused.
  struct BlockOwner {
    BlockOwner();
    BlockOwner(const BlockOwner&);
    BlockOwner(BlockOwner&& other);
    auto operator=(const BlockOwner& other) -> BlockOwner&;
    auto operator=(BlockOwner&& other) -> BlockOwner&;
    std::uint64_t id;
  };

  struct Layout {
    Rect widget{};
    Rect viewport{};
    std::size_t top{0};
    int width{0};
    std::uint64_t owner{0};
    bool valid{false};
  };

  static constexpr std::uint32_t kWrapPolicyRevision = 1;

  [[nodiscard]] auto allocate_entry(StyledText initial, bool finalized,
                                    bool content_roles = false)
      -> TextEntryHandle;
  [[nodiscard]] auto resolve(TextEntryHandle handle) noexcept -> Entry*;
  [[nodiscard]] auto resolve_live(TextEntryHandle handle) noexcept -> Entry*;
  [[nodiscard]] auto resolve_block(TextBlockHandle handle) const noexcept
      -> const Entry*;
  [[nodiscard]] auto validate_block_rows(int rows, int previous) const
      -> std::expected<void, ErrorEvent>;
  [[nodiscard]] static auto payload_bytes(const Entry& entry) -> std::size_t;
  static auto append_clean_span(Entry& entry, std::string text, TextStyle style,
                                bool preserve_empty, bool content_roles)
      -> bool;
  auto ingest_chunks(Entry& entry, StyledText chunks, bool content_roles)
      -> bool;
  auto append_chunks(Entry& entry, StyledText chunks, bool content_roles)
      -> bool;
  auto replace_chunks(Entry& entry, StyledText chunks, bool content_roles)
      -> bool;
  auto finish_pending(Entry& entry) -> void;
  auto note_entry_change(Entry& entry, std::size_t old_bytes,
                         bool visible_changed) -> void;
  auto enforce_retention() -> bool;
  auto release_slot(std::deque<std::size_t>::iterator position) -> void;
  [[nodiscard]] auto ensure_wrapped(Entry& entry, int width)
      -> const std::vector<StyledText>&;
  [[nodiscard]] auto wrapped_total(int width) -> std::size_t;
  [[nodiscard]] auto entry_rows(Entry& entry, int width) -> std::size_t;
  [[nodiscard]] auto layout_width() -> int;
  auto anchor_from_top(std::size_t top, int width) -> void;
  [[nodiscard]] auto anchored_top(int width, std::size_t max_top)
      -> std::optional<std::size_t>;
  auto refresh_anchor_from_scroll() -> void;

  BlockOwner m_block_owner;
  std::vector<Slot> m_slots;
  std::deque<std::size_t> m_order; // chronological slot indices
  std::vector<std::size_t> m_free;
  std::optional<TextEntryHandle> m_live;
  std::optional<ViewAnchor> m_anchor;

  std::size_t m_scroll{0}; // 0 = pinned to bottom; >0 = rows scrolled up
  bool m_follow{true};     // auto-scroll to bottom on new content
  TextBoxRetention m_retention;
  std::size_t m_retained_bytes{0};
  std::uint64_t m_wrap_build_count{0};
  TextBoxBlockLimits m_block_limits;
  std::size_t m_block_count{0};
  std::size_t m_block_rows{0};
  Layout m_layout;

  // #21: the scrollbar strip's family and colours. Default colours mirror
  // the list/table: dim track, selection-blue thumb.
  BorderStyle m_style{BorderStyle::Single};
  bool m_style_override{false}, m_scrollbar_override{false};
  Rgb m_track_fg{theme::kDim};
  Rgb m_thumb_fg{theme::kFocusBg};
};

} // namespace termforge
