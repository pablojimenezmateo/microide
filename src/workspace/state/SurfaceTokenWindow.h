#pragma once

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <utility>
#include <vector>

#include "editor/SyntaxHighlighter.h"

namespace microide::workspace {

// Syntax tokens for a read-only text pane that is tokenized CUMULATIVELY from the
// top: compare's two diff panes and merge's incoming/current panes.
//
// Those panes cannot tokenize row N without the syntax state left behind by row
// N-1, so reaching a row means walking every row above it. What they used to do
// was keep the TOKENS of every row that walk passed — one heap
// `vector<SyntaxTokenKind>` per line of the file, allocated the first time the
// walk reached it and retained for the tab's life. Scrolling to the bottom of the
// 24,000-line merge fixture cost 24,196 allocations, 99 % of that phase, and held
// megabytes of token buffers in order to paint the fifty rows actually on screen.
// The same shape was 86 % of `diff.next_hunk_burst` (TD-2026-08-15-242).
//
// The walk is unavoidable. Retaining its output is not. This keeps:
//
//  - one `SyntaxState` per row (a 40-byte POD) so ANY row can be re-tokenized in
//    O(1) from its predecessor's state — which is what makes scrolling backwards
//    cheap without keeping the tokens; and
//  - token buffers for a bounded WINDOW of rows around the viewport, recycled
//    through a pool as the window slides, so a scroll allocates nothing once the
//    window is warm.
//
// Net memory is lower than what it replaces: 40 bytes of state per row, against a
// vector header plus a heap buffer plus its allocator overhead per row.
//
// Rows are addressed in the pane's own line space. `Tokens(row)` returns an empty
// vector for a row outside the window or past the frontier, which is exactly what
// the renderers already do with a row that has not been tokenized yet.
class SurfaceTokenWindow {
 public:
  // Rows kept on each side of the requested window. Absorbs a scroll of up to
  // this many rows without re-tokenizing anything, and bounds what is retained.
  static constexpr std::size_t kWindowMarginRows = 128;
  // Buffers held for the next slide. A window is at most (visible + 2 * margin)
  // rows, and a slide retires at most that many at once.
  static constexpr std::size_t kMaxPooledBuffers = 512;

  // Re-points at a pane of `line_count` rows starting from `initial_state`.
  // Retires every live buffer into the pool rather than freeing it: the caller
  // that rebuilds a model is about to ask for the same window again.
  void Reset(std::size_t line_count, const editor::SyntaxState& initial_state) {
    RetireWindow();
    initial_state_ = initial_state;
    frontier_ = 0;
    end_states_.assign(line_count, editor::SyntaxState{});
    // assign() over the same length keeps the per-row buffers, which are empty
    // anyway after RetireWindow; a length change is the only case that reallocates.
    tokens_.resize(line_count);
    window_start_ = 0;
    window_end_ = 0;
  }

  std::size_t line_count() const { return end_states_.size(); }
  std::size_t frontier() const { return frontier_; }

  const std::vector<editor::SyntaxTokenKind>& Tokens(std::size_t row) const {
    static const std::vector<editor::SyntaxTokenKind> kEmpty;
    return row < tokens_.size() ? tokens_[row] : kEmpty;
  }

  // The state a row resumes from. Only meaningful below the frontier.
  const editor::SyntaxState& StateBefore(std::size_t row) const {
    return row == 0 ? initial_state_ : end_states_[row - 1];
  }

  // Advances the frontier toward `end` — by at most `walk_budget` rows, so a jump
  // to the bottom of a large file does not stall one frame — and tokenizes the
  // rows of [start, end) that the frontier has reached.
  //
  // `line_at(row)` yields the row's text. The walk below the window uses
  // `AdvanceState`, which computes the same end state WITHOUT materializing
  // tokens and therefore without allocating: passing over a row is what most of
  // this work is, and it no longer costs a buffer.
  template <typename LineAt>
  void EnsureWindow(std::size_t start,
                    std::size_t end,
                    const std::filesystem::path& path,
                    std::size_t walk_budget,
                    LineAt&& line_at) {
    const std::size_t rows = end_states_.size();
    if (rows == 0) {
      return;
    }
    start = std::min(start, rows);
    end = std::min(end, rows);
    if (start >= end) {
      return;
    }

    // 1. Walk the state frontier up to `end`, budgeted. Nothing is tokenized here.
    if (frontier_ < end) {
      const std::size_t target = std::min(end, frontier_ + walk_budget);
      while (frontier_ < target) {
        end_states_[frontier_] =
            editor::SyntaxHighlighter::AdvanceState(line_at(frontier_), path,
                                                    StateBefore(frontier_));
        ++frontier_;
      }
    }

    // 2. Slide the RETENTION band. It is wider than the requested range on both
    //    sides, so a scroll of up to `kWindowMarginRows` finds the rows it moves
    //    onto already tokenized. Rows leaving hand their buffer to the pool; rows
    //    entering the fill range take one back, so a steady scroll allocates
    //    nothing.
    //
    //    Retention is deliberately NOT the fill range. Tokenizing the margin
    //    eagerly would make every open pay for rows nobody has looked at:
    //    `merge.edit_result_scroll` went from 129 allocations to 576 when this
    //    filled the band instead of merely keeping it.
    const std::size_t keep_start = start > kWindowMarginRows ? start - kWindowMarginRows : 0;
    const std::size_t keep_end = std::min(rows, end + kWindowMarginRows);
    if (keep_start != window_start_ || keep_end != window_end_) {
      for (std::size_t row = window_start_; row < window_end_; ++row) {
        if (row < keep_start || row >= keep_end) {
          RetireRow(row);
        }
      }
      window_start_ = keep_start;
      window_end_ = keep_end;
    }

    // 3. Tokenize the REQUESTED rows the frontier has reached and that are not
    //    already populated. A row's buffer is only empty when it was retired or
    //    never filled, which is exactly the miss condition.
    const std::size_t fill_end = std::min(end, frontier_);
    for (std::size_t row = start; row < fill_end; ++row) {
      if (!tokens_[row].empty()) {
        continue;
      }
      TakePooledBufferInto(tokens_[row]);
      editor::SyntaxHighlighter::HighlightLineInto(line_at(row), path, StateBefore(row),
                                                   &tokens_[row]);
    }
  }

  // Frees everything, including the pool. For a tab being torn down or a pane
  // whose highlighting was switched off.
  void Clear() {
    end_states_.clear();
    end_states_.shrink_to_fit();
    tokens_.clear();
    tokens_.shrink_to_fit();
    pool_.clear();
    pool_.shrink_to_fit();
    frontier_ = 0;
    window_start_ = 0;
    window_end_ = 0;
  }

 private:
  void RetireRow(std::size_t row) {
    if (tokens_[row].capacity() == 0) {
      return;
    }
    if (pool_.size() >= kMaxPooledBuffers) {
      // Over the cap the buffer is dropped, which is the plain free this replaced.
      tokens_[row] = std::vector<editor::SyntaxTokenKind>{};
      return;
    }
    tokens_[row].clear();  // clear() keeps the capacity, which is the whole point
    pool_.push_back(std::move(tokens_[row]));
  }

  void RetireWindow() {
    for (std::size_t row = window_start_; row < window_end_ && row < tokens_.size(); ++row) {
      RetireRow(row);
    }
    window_start_ = 0;
    window_end_ = 0;
  }

  void TakePooledBufferInto(std::vector<editor::SyntaxTokenKind>& out) {
    if (pool_.empty()) {
      return;
    }
    out = std::move(pool_.back());
    pool_.pop_back();
    out.clear();
  }

  editor::SyntaxState initial_state_{};
  // End state of every row below `frontier_`. The reason a row outside the window
  // can be re-tokenized without re-walking the file.
  std::vector<editor::SyntaxState> end_states_;
  std::size_t frontier_ = 0;
  // Row-indexed, but only [window_start_, window_end_) is populated. Kept
  // row-indexed so the renderers stay a plain subscript.
  std::vector<std::vector<editor::SyntaxTokenKind>> tokens_;
  std::size_t window_start_ = 0;
  std::size_t window_end_ = 0;
  std::vector<std::vector<editor::SyntaxTokenKind>> pool_;
};

}  // namespace microide::workspace
