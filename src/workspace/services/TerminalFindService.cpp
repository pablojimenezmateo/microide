#include "workspace/services/TerminalFindService.h"

#include <algorithm>
#include <utility>
#include <cstdlib>

#include "util/StringUtil.h"
#include "workspace/state/WorkspaceTabState.h"

namespace microide::workspace {

using util::AppendUnsigned;  // one definition, in util/StringUtil.h

void TerminalFindService::ClearMatches() {
  matches_.clear();
  selected_index_ = 0;
  truncated_ = false;
  scanned_session_ = nullptr;
}

void TerminalFindService::RebuildCountText() {
  count_text_.clear();
  if (query_.text().empty()) {
    return;
  }
  if (matches_.empty()) {
    count_text_ = "0/0";
    return;
  }
  AppendUnsigned(count_text_, selected_index_ + 1);
  count_text_ += '/';
  AppendUnsigned(count_text_, matches_.size());
  if (truncated_) {
    // The scan stopped at the cap, so the total is a floor, not a count.
    count_text_ += '+';
  }
}

void TerminalFindService::Open(TerminalTabState* tab, const std::string_view seed) {
  visible_ = true;
  focused_ = true;
  if (!seed.empty()) {
    query_.SetText(std::string(seed));
    Invalidate();
  }
  // Reopening preselects the query so the next keystroke replaces it, which is
  // what "Ctrl+F, type a new thing" expects.
  query_.SelectAll();
  Refresh(tab);
  RevealSelected(tab);
}

void TerminalFindService::Close() {
  visible_ = false;
  focused_ = false;
  ClearMatches();
  count_text_.clear();
}

void TerminalFindService::ToggleCaseSensitive(TerminalTabState* tab) {
  case_sensitive_ = !case_sensitive_;
  Refresh(tab);
}

void TerminalFindService::ToggleWholeWord(TerminalTabState* tab) {
  whole_word_ = !whole_word_;
  Refresh(tab);
}

void TerminalFindService::Refresh(TerminalTabState* tab) {
  if (!visible_ || tab == nullptr) {
    ClearMatches();
    count_text_.clear();
    return;
  }
  const std::string_view text = query_.text();
  if (text.empty()) {
    ClearMatches();
    count_text_.clear();
    return;
  }

  const bool same_scan = scanned_session_ == static_cast<const void*>(&tab->session) &&
                         scanned_query_ == text && scanned_case_sensitive_ == case_sensitive_ &&
                         scanned_whole_word_ == whole_word_;
  const terminal::TerminalSearchQuery query =
      terminal::MakeTerminalSearchQuery(text, case_sensitive_, whole_word_);

  std::size_t start_row = 0;
  std::uint64_t expected_trim_total = scanned_trim_total_;
  // Which hit was selected, so it survives a rescan: a match set that merely grew
  // at the tail must not jump the selection back to the top. Captured after the
  // trim rebase below so it is in the same row space as the rescanned matches.
  std::optional<terminal::TerminalSearchMatch> previous;
  if (same_scan) {
    // Only the visible grid can still be rewritten, so keep the settled
    // scrollback matches and rescan from that boundary. Rebase first: scrollback
    // trimming runs on the reader thread and shifts every absolute row down.
    const std::uint64_t trim_total = tab->session.ScrollbackTrimTotal();
    if (const std::uint64_t delta = trim_total - scanned_trim_total_; delta > 0) {
      std::erase_if(matches_, [&](const terminal::TerminalSearchMatch& match) {
        return match.row < delta;
      });
      for (terminal::TerminalSearchMatch& match : matches_) {
        match.row -= delta;
      }
      scanned_stable_row_end_ =
          scanned_stable_row_end_ > delta ? scanned_stable_row_end_ - delta : 0;
      selected_index_ = std::min(selected_index_, matches_.empty() ? 0 : matches_.size() - 1);
    }
    if (selected_index_ < matches_.size()) {
      previous = matches_[selected_index_];
    }
    start_row = scanned_stable_row_end_;
    expected_trim_total = trim_total;
    std::erase_if(matches_, [&](const terminal::TerminalSearchMatch& match) {
      return match.row >= start_row;
    });
  } else {
    matches_.clear();
  }

  const terminal::TerminalSession::SearchScan scan = tab->session.FindMatches(
      query, start_row, expected_trim_total, kMaxMatches, scratch_, matches_);

  scanned_session_ = &tab->session;
  scanned_query_.assign(text);
  scanned_case_sensitive_ = case_sensitive_;
  scanned_whole_word_ = whole_word_;
  scanned_trim_total_ = scan.trim_total;
  scanned_stable_row_end_ = scan.stable_row_end;
  truncated_ = scan.truncated;

  if (matches_.empty()) {
    selected_index_ = 0;
  } else if (previous.has_value() && !scan.full_rescan) {
    const auto it = std::lower_bound(
        matches_.begin(), matches_.end(), *previous,
        [](const terminal::TerminalSearchMatch& lhs, const terminal::TerminalSearchMatch& rhs) {
          return lhs.row < rhs.row || (lhs.row == rhs.row && lhs.column < rhs.column);
        });
    selected_index_ = it == matches_.end() ? matches_.size() - 1
                                           : static_cast<std::size_t>(it - matches_.begin());
  } else {
    // A fresh query starts at the newest hit: terminal output is read from the
    // bottom, so the interesting match is almost always the last one. Enter then
    // wraps forward and Shift+Enter walks back up through the history.
    selected_index_ = matches_.size() - 1;
  }
  RebuildCountText();
}

bool TerminalFindService::SelectRelative(TerminalTabState* tab, const int delta) {
  Refresh(tab);
  if (matches_.empty()) {
    return false;
  }
  const auto count = static_cast<std::ptrdiff_t>(matches_.size());
  auto index = static_cast<std::ptrdiff_t>(selected_index_) + delta;
  // Wrap at both ends, like every other find surface in the shell.
  index %= count;
  if (index < 0) {
    index += count;
  }
  selected_index_ = static_cast<std::size_t>(index);
  RebuildCountText();
  RevealSelected(tab);
  return true;
}

void TerminalFindService::RevealSelected(TerminalTabState* tab) const {
  if (tab == nullptr || selected_index_ >= matches_.size()) {
    return;
  }
  const std::size_t row = matches_[selected_index_].row;
  // The panel render path records how many rows it last drew, so the reveal can
  // center without the service needing the frame layout.
  const std::size_t visible_rows = std::max<std::size_t>(1, tab->visible_lines_max_rows);
  const auto first_visible = static_cast<std::size_t>(std::max(0, tab->scroll_row));
  if (row >= first_visible && row < first_visible + visible_rows) {
    return;  // already on screen: don't yank the view for a match the user can see
  }
  // Following the tail would scroll straight back to the bottom on the next frame.
  tab->follow_tail = false;
  const std::size_t centered = row > visible_rows / 2 ? row - visible_rows / 2 : 0;
  tab->scroll_row = static_cast<int>(centered);
}

}  // namespace microide::workspace
