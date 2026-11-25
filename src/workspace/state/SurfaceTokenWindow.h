#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <utility>
#include <vector>

#include "editor/SyntaxHighlighter.h"

namespace microide::workspace {

inline constexpr std::chrono::milliseconds kSyntaxWalkTimePerFrame{4};

class SurfaceTokenWindow {
 public:
  static constexpr std::size_t kWindowMarginRows = 128;
  static constexpr std::size_t kMaxPooledBuffers = 512;

  void Reset(std::size_t line_count, const editor::SyntaxState& initial_state) {
    RetireWindow();
    initial_state_ = initial_state;
    frontier_ = 0;
    end_states_.assign(line_count, editor::SyntaxState{});
    tokens_.resize(line_count);
    window_start_ = 0;
    window_end_ = 0;
  }

  void ResetFrom(std::size_t first_changed, std::size_t line_count, const editor::SyntaxState& initial_state) {
    if (!(initial_state == initial_state_) || first_changed == 0) {
      Reset(line_count, initial_state);
      return;
    }
    first_changed = std::min(first_changed, line_count);
    for (std::size_t row = std::max(window_start_, first_changed); row < window_end_ && row < tokens_.size(); ++row) {
      RetireRow(row);
    }
    window_end_ = std::max(window_start_, std::min(window_end_, first_changed));
    frontier_ = std::min(frontier_, first_changed);
    end_states_.resize(line_count);
    tokens_.resize(line_count);
  }

  struct WalkBudget {
    std::size_t rows = 0;
    std::chrono::steady_clock::time_point until{};
    WalkBudget() = default;
    WalkBudget(std::size_t walk_rows) : rows(walk_rows) {}  // NOLINT(google-explicit-constructor): a row count is a budget
    WalkBudget(std::size_t walk_rows, std::chrono::steady_clock::time_point walk_until)
        : rows(walk_rows), until(walk_until) {}
  };

  std::size_t line_count() const { return end_states_.size(); }
  std::size_t frontier() const { return frontier_; }

  const std::vector<editor::SyntaxTokenKind>& Tokens(std::size_t row) const {
    static const std::vector<editor::SyntaxTokenKind> kEmpty;
    return row < tokens_.size() ? tokens_[row] : kEmpty;
  }

  const editor::SyntaxState& StateBefore(std::size_t row) const {
    return row == 0 ? initial_state_ : end_states_[row - 1];
  }

  template <typename LineAt, typename FillFilter>
  void EnsureWindow(std::size_t start,
                    std::size_t end,
                    const std::filesystem::path& path,
                    WalkBudget walk_budget,
                    LineAt&& line_at,
                    FillFilter&& should_fill) {
    const std::size_t rows = end_states_.size();
    if (rows == 0) {
      return;
    }
    end = std::min(end, rows);
    start = std::min(start, end);

    const std::size_t step = walk_budget.rows;
    for (bool first_step = true;
         step > 0 && frontier_ < end && (first_step || std::chrono::steady_clock::now() < walk_budget.until);
         first_step = false) {
      const std::size_t target = std::min(end, frontier_ + step);
      while (frontier_ < target) {
        end_states_[frontier_] =
            editor::SyntaxHighlighter::AdvanceState(line_at(frontier_), path,
                                                    StateBefore(frontier_));
        ++frontier_;
      }
    }

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

    const std::size_t fill_end = std::min(end, frontier_);
    for (std::size_t row = start; row < fill_end; ++row) {
      if (!tokens_[row].empty() || !should_fill(row)) {
        continue;
      }
      TakePooledBufferInto(tokens_[row]);
      editor::SyntaxHighlighter::HighlightLineInto(line_at(row), path, StateBefore(row),
                                                   &tokens_[row]);
    }
  }

  template <typename LineAt>
  void EnsureWindow(std::size_t start,
                    std::size_t end,
                    const std::filesystem::path& path,
                    WalkBudget walk_budget,
                    LineAt&& line_at) {
    EnsureWindow(start, end, path, walk_budget, std::forward<LineAt>(line_at),
                 [](std::size_t) { return true; });
  }

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
      tokens_[row] = std::vector<editor::SyntaxTokenKind>{};
      return;
    }
    tokens_[row].clear();
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
  std::vector<editor::SyntaxState> end_states_;
  std::size_t frontier_ = 0;
  std::vector<std::vector<editor::SyntaxTokenKind>> tokens_;
  std::size_t window_start_ = 0;
  std::size_t window_end_ = 0;
  std::vector<std::vector<editor::SyntaxTokenKind>> pool_;
};

}
