#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/SingleLineEditor.h"
#include "terminal/TerminalSearch.h"

namespace microide::workspace {

struct TerminalPaneState;

class TerminalFindService {
 public:
  static constexpr std::size_t kMaxMatches = 5000;

  bool visible() const { return visible_; }
  bool focused() const { return visible_ && focused_; }
  void SetFocused(bool focused) { focused_ = focused; }
  editor::SingleLineEditor& query() { return query_; }
  const editor::SingleLineEditor& query() const { return query_; }
  bool case_sensitive() const { return case_sensitive_; }
  bool whole_word() const { return whole_word_; }
  const std::vector<terminal::TerminalSearchMatch>& matches() const { return matches_; }
  std::size_t selected_index() const { return selected_index_; }
  std::string_view count_text() const { return count_text_; }
  std::string_view display_text() const { return display_text_; }
  void SetDisplayText(std::string text) { display_text_ = std::move(text); }

  void Open(TerminalPaneState* tab, std::string_view seed = {});
  void Close();
  void ToggleCaseSensitive(TerminalPaneState* tab);
  void ToggleWholeWord(TerminalPaneState* tab);

  void Refresh(TerminalPaneState* tab);
  void Invalidate() { scanned_session_ = nullptr; }

  bool SelectRelative(TerminalPaneState* tab, int delta);

 private:
  void ClearMatches();
  void RebuildCountText();
  void RevealSelected(TerminalPaneState* tab) const;

  bool visible_ = false;
  bool focused_ = false;
  editor::SingleLineEditor query_;
  bool case_sensitive_ = false;
  bool whole_word_ = false;

  std::vector<terminal::TerminalSearchMatch> matches_;
  std::size_t selected_index_ = 0;
  bool truncated_ = false;
  std::string count_text_;
  std::string display_text_;

  const void* scanned_session_ = nullptr;
  std::string scanned_query_;
  bool scanned_case_sensitive_ = false;
  bool scanned_whole_word_ = false;
  std::uint64_t scanned_trim_total_ = 0;
  std::uint64_t scanned_row_shift_sequence_ = 0;
  std::size_t scanned_stable_row_end_ = 0;

  terminal::TerminalSearchScratch scratch_;
};

}
