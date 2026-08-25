#pragma once

// TermForge — Screen: the cell grid that widgets render into.
//
// A Screen is a cols×rows grid of Cells. Widgets draw into it; the Renderer
// diffs it against the previous frame and emits only the changes through the
// driver. Screen also owns resize handling (SIGWINCH) and the escape
// sanitization boundary for text.

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "termforge/core/styled_text.hpp"
#include "termforge/core/types.hpp"

namespace termforge {

class Renderer;
class App;

// A single terminal cell: one grapheme token, fg/bg color, optional image ref.
//
// The common case (one UTF-8 scalar, up to four bytes) lives inline. Longer
// graphemes carry a process-unique token into their owning Screen's spill
// table. Resolve text through Screen::text_at(): a standalone Cell deliberately
// cannot turn a Screen-owned token back into bytes (#92).
struct Cell {
 private:
  static constexpr std::size_t kInlineTextBytes = 4;
  static constexpr std::uint8_t kSpilledText = 0xFF;

  std::uint64_t m_text_token{0};

 public:
  std::int32_t image_id{-1}; // >=0 references an image placement

 private:
  std::array<char, kInlineTextBytes> m_inline_text{};

 public:
  Rgb fg{0xE0, 0xE0, 0xF0};
  Rgb bg{0x0A, 0x0A, 0x14};
  Attr attrs{Attr::None}; // per-cell display attributes (#62)

 private:
  std::uint8_t m_text_size{0};

 public:
  [[nodiscard]] auto blank() const noexcept -> bool {
    return m_text_size == 0 && image_id < 0;
  }
  [[nodiscard]] auto operator==(const Cell& other) const noexcept -> bool {
    // Every byte has one canonical value: unused inline bytes and the token
    // are zeroed, all members have unique object representations, and the
    // struct has no padding. Equality can therefore be the exact operation
    // the renderer's hot path needs rather than N small string comparisons.
    return std::memcmp(this, &other, sizeof(Cell)) == 0;
  }

 private:
  friend class Screen;
};

static_assert(std::is_trivially_copyable_v<Cell>);
static_assert(std::has_unique_object_representations_v<Cell>);
static_assert(sizeof(Cell) == 24);

class Screen {
 public:
  Screen(int cols, int rows);

  [[nodiscard]] auto cols() const noexcept -> int { return m_cols; }
  [[nodiscard]] auto rows() const noexcept -> int { return m_rows; }

  // Resize the grid (SIGWINCH). Content is clipped/preserved top-left.
  auto resize(int cols, int rows) -> void;

  // Cell access. Out-of-bounds coordinates are clamped/ignored (defensive —
  // a widget bug must not corrupt memory).
  [[nodiscard]] auto at(int x, int y) const -> const Cell&;
  auto at(int x, int y) -> Cell&;

  // Resolve a cell's grapheme. The view may be invalidated by any mutating
  // Screen operation and is always invalidated by clear, resize or destruction.
  // OOB coordinates return an empty view.
  [[nodiscard]] auto text_at(int x, int y) const noexcept -> std::string_view;

  // Fill the whole grid with blank cells. The styled overload preserves the
  // old colored-clear use case without accepting a standalone spill token.
  auto clear() -> void;
  auto clear(Rgb fg, Rgb bg, Attr attrs = Attr::None) -> void;

  // Blank a sub-rectangle to a colored blank cell (empty text, fg/bg, no
  // image), clamped to the grid. This is how a widget repaints its whole rect()
  // each frame (see widget.hpp): it clears any prior glyph, wide-glyph
  // continuation cell, or stale image_id in the region. Negative/oversized
  // rects are clipped.
  auto fill_rect(int x, int y, int w, int h, Rgb fg, Rgb bg,
                 Attr attrs = Attr::None) -> void;

  // Write sanitized text starting at (x,y). Control characters and ESC are
  // stripped here — the sanitization boundary — so drivers can emit cells
  // verbatim.
  //
  // Text is CLIPPED at both edges and relocated at neither (#152): a glyph
  // whose columns are all left of 0 or all past the last column paints
  // nothing, and the rest paints at its true position. A width-2 glyph that
  // would straddle either edge is dropped and its one on-screen column is
  // padded with a space in the run's colours, because half a wide glyph is not
  // expressible and an unpainted hole inside a run keeps the previous frame's
  // content (the renderer only emits cells that changed).
  //
  // Returns the number of on-screen cells painted, so never more than cols();
  // an off-screen glyph counts nothing, and the return does not say where the
  // visible part started.
  auto write_text(int x, int y, std::string_view text, Rgb fg, Rgb bg,
                  Attr attrs = Attr::None) -> int;

  // Write a sequence of styled spans on one row (#25). Span boundaries share
  // write_text's exact cursor walk, so left/right clipping and wide-glyph
  // padding cannot relocate a later span. Empty spans paint and advance
  // nothing. Returns the total on-screen cells painted across all spans.
  auto write_styled(int x, int y, std::span<const TextSpan> spans) -> int;

  // Sanitize untrusted text: drop C0/C1 control chars and ESC, keep printable
  // + valid UTF-8 continuation bytes. Delegates to text::sanitize(in, Strip)
  // (#149) — the write path and every measure-what-will-paint caller share
  // one policy; see termforge/core/text.hpp.
  static auto sanitize(std::string_view in) -> std::string;

 private:
  struct WriteResult {
    int written;
    int next_x;
  };

  // The one text-placement primitive behind both public write paths. next_x
  // is the cursor produced by the actual glyph walk; it cannot be recovered
  // from written when a prefix was clipped off the left edge.
  auto write_text_impl(int x, int y, std::string_view text, Rgb fg, Rgb bg,
                       Attr attrs) -> WriteResult;

  struct SpillEntry {
    std::string text;
    bool referenced{false};
  };

  [[nodiscard]] auto cell_text(const Cell& cell) const noexcept
      -> std::string_view;
  auto mutable_cell(int x, int y) noexcept -> Cell&;
  auto reset_text(Cell& cell) noexcept -> void;
  auto set_text(Cell& cell, std::string_view text) -> void;
  auto append_text(Cell& cell, std::string_view suffix) -> void;
  auto restore_cell(int x, int y, const Cell& cell, std::string_view text)
      -> void;
  auto clear_cell(int x, int y) -> void;
  auto maybe_reclaim_spills_after_mutation() -> void;
  auto reclaim_unused_spills() const -> void;

  // Renderer owns the shadow copy of this exact grid. Keeping the contiguous
  // hand-off private avoids exposing Screen's vector representation as API.
  friend class Renderer;
  friend class App;

  int m_cols{0};
  int m_rows{0};
  std::vector<Cell> m_cells;
  // Spill storage is logically ancillary to the grid: Renderer may erase
  // entries no live Cell names after it has resolved the current frame,
  // without changing any observable Screen content. The collection
  // bookkeeping is mutable for that same const-present boundary.
  mutable std::unordered_map<std::uint64_t, SpillEntry> m_spills;
  mutable std::size_t m_spill_allocations_since_reclaim{0};
  mutable bool m_spill_reclaim_pending{false};
  // Returning Cell& lets a caller keep and mutate it after at() returns. Once
  // that has happened, only a grid scan can prove which spill tokens remain;
  // the latch therefore stays set for the Screen's lifetime.
  mutable bool m_mutable_cell_access_exposed{false};
  Cell m_out_of_bounds; // returned (const) for OOB reads; writes are dropped
};

} // namespace termforge
