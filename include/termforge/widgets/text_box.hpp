#pragma once

// TermForge — TextBox: a scrolling multi-line text area (chat-scrollback
// style). Finalized lines and one mutable streaming tail share the same
// document; the view shows the most recent rows that fit its rect,
// auto-scrolling to the bottom on new content unless the user has scrolled up.
// Supports bounded retention, manual scroll (PageUp/PageDown / scroll wheel)
// and display-width-aware word wrapping across styled spans (#24/#25), with
// hard wrapping only for an unbroken run wider than the widget. This is the
// foundation of a chat message or live-log view.

#include <cstddef>
#include <cstdint>
#include <deque>
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
  constexpr auto operator==(const TextEntryHandle&) const noexcept -> bool = default;
};

// Optional logical-entry and source-byte limits. nullopt means unlimited; zero
// is a real limit. The mutable live tail is never evicted, so
// retention_over_budget() can remain true until it is finalized or the limits
// are relaxed.
struct TextBoxRetention {
  std::optional<std::size_t> max_entries{};
  std::optional<std::size_t> max_bytes{};

  constexpr auto operator==(const TextBoxRetention&) const noexcept -> bool = default;
};

class TextBox final : public Widget {
 public:
  TextBox() = default;

  // Append a logical line (a chat message, a log entry). Long lines wrap at
  // the last fitting space, falling back to a display-width-safe hard split
  // for an overlong word. Source whitespace is preserved. Marks the widget
  // dirty and auto-scrolls to the bottom if the user is already at the bottom.
  // Plain text becomes one default-colour span; sanitization runs here (#25).
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

  // Mutate the live tail. Plain chunks use the same default style as the
  // compatibility append(string) path; styled chunks retain their styles.
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
    m_style = style;
    mark_dirty();
  }
  [[nodiscard]] auto style() const noexcept -> BorderStyle { return m_style; }

  // Scrollbar colours (#21): the │ track and the █ thumb.
  auto set_scrollbar_colors(Rgb track_fg, Rgb thumb_fg) -> void {
    m_track_fg = track_fg;
    m_thumb_fg = thumb_fg;
    mark_dirty();
  }

  // The width text wraps and paints at: the rect width minus the column #21's
  // scrollbar claims when the content overflows. Whether the bar is up is
  // decided at draw time (only then is the wrapped row count known), so this
  // is the draw-loop's width -- an app laying out against it should treat it
  // as advisory, like the wrap itself. Floored at 0 (never negative): the
  // wrap helper requires a positive width and the narrow-rect draw guards
  // before calling it.
  [[nodiscard]] auto content_w() const noexcept -> int;

 private:
  struct WrapCache {
    std::vector<StyledText> rows;
    std::uint64_t content_revision{0};
    std::uint32_t policy_revision{0};
    int width{0};
    bool valid{false};
  };

  struct Entry {
    StyledText text;
    std::string pending_utf8;
    TextStyle pending_style{};
    std::size_t bytes{0};
    std::uint64_t content_revision{1};
    bool finalized{false};
    WrapCache wrap;
  };

  struct Slot {
    std::optional<Entry> entry;
    std::uint64_t generation{1};
  };

  struct ViewAnchor {
    TextEntryHandle entry;
    std::size_t row{0};
  };

  static constexpr std::uint32_t kWrapPolicyRevision = 1;

  [[nodiscard]] auto allocate_entry(StyledText initial, bool finalized)
      -> TextEntryHandle;
  [[nodiscard]] auto resolve(TextEntryHandle handle) noexcept -> Entry*;
  [[nodiscard]] auto resolve_live(TextEntryHandle handle) noexcept -> Entry*;
  [[nodiscard]] static auto payload_bytes(const Entry& entry) -> std::size_t;
  static auto append_clean_span(Entry& entry, std::string text,
                                TextStyle style, bool preserve_empty) -> bool;
  auto ingest_chunks(Entry& entry, StyledText chunks) -> bool;
  auto append_chunks(Entry& entry, StyledText chunks) -> bool;
  auto replace_chunks(Entry& entry, StyledText chunks) -> bool;
  auto finish_pending(Entry& entry) -> void;
  auto note_entry_change(Entry& entry, std::size_t old_bytes,
                         bool visible_changed) -> void;
  auto enforce_retention() -> bool;
  auto release_slot(std::deque<std::size_t>::iterator position) -> void;
  [[nodiscard]] auto ensure_wrapped(Entry& entry, int width)
      -> const std::vector<StyledText>&;
  [[nodiscard]] auto wrapped_total(int width) -> std::size_t;
  auto anchor_from_top(std::size_t top, int width) -> void;
  [[nodiscard]] auto anchored_top(int width, std::size_t max_top)
      -> std::optional<std::size_t>;
  auto refresh_anchor_from_scroll() -> void;

  std::vector<Slot> m_slots;
  std::deque<std::size_t> m_order;  // chronological slot indices
  std::vector<std::size_t> m_free;
  std::optional<TextEntryHandle> m_live;
  std::optional<ViewAnchor> m_anchor;

  int m_scroll{0};                  // 0 = pinned to bottom; >0 = lines scrolled up
  bool m_follow{true};              // auto-scroll to bottom on new content
  TextBoxRetention m_retention;
  std::size_t m_retained_bytes{0};
  std::uint64_t m_wrap_build_count{0};

  // #21: the scrollbar strip's family and colours. Default colours mirror
  // the list/table: dim track, selection-blue thumb.
  BorderStyle m_style{BorderStyle::Single};
  Rgb m_track_fg{theme::kDim};
  Rgb m_thumb_fg{theme::kFocusBg};
};

}  // namespace termforge
