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

struct TerminalTabState;

// Find-in-scrollback for the terminal panel, VSCode's terminal find widget in
// shape: a floating bar over the terminal body, all matches highlighted, an
// "n/m" counter, and Enter / Shift+Enter to step between hits.
//
// The state lives here rather than in ProjectWorkspaceState because none of it
// is persisted or per-project, and the scan needs private caching (the reused
// flatten scratch and the incremental-rescan identity) that has no business
// being snapshotted with project state. There is one bar for the panel, bound to
// whichever terminal tab is active — switching tabs re-runs the query against
// the new tab rather than keeping a separate hit list per terminal.
//
// Match rows are absolute session rows, matching the terminal tab's scroll and
// selection mirrors, and are rebased the same way when scrollback is trimmed.
class TerminalFindService {
 public:
  // Ceiling on retained matches. Highlighting more than this is not useful and a
  // pathological query ("a" against a full 100k-line scrollback) would otherwise
  // retain millions of entries; the counter renders as "n/m+" when it is hit.
  static constexpr std::size_t kMaxMatches = 5000;

  bool visible() const { return visible_; }
  // The bar is non-modal, exactly like the in-file find widget: it floats over a
  // live terminal, and focus moves back to the shell when the user clicks the
  // terminal body or presses Escape. Only a focused bar claims text input.
  bool focused() const { return visible_ && focused_; }
  void SetFocused(bool focused) { focused_ = focused; }
  editor::SingleLineEditor& query() { return query_; }
  const editor::SingleLineEditor& query() const { return query_; }
  bool case_sensitive() const { return case_sensitive_; }
  bool whole_word() const { return whole_word_; }
  const std::vector<terminal::TerminalSearchMatch>& matches() const { return matches_; }
  std::size_t selected_index() const { return selected_index_; }
  // "3/12", "12/5000+" when the cap was hit, "0/0" with a query and no hits, and
  // empty with no query. Composed here so the render path draws a prebuilt view.
  std::string_view count_text() const { return count_text_; }
  // Caret-relative tail the focused query field renders. Only the shell can
  // measure text, so it composes this during frame prep; the storage lives here
  // beside count_text_ so every string the bar draws has one owner that outlives
  // the frame.
  std::string_view display_text() const { return display_text_; }
  void SetDisplayText(std::string text) { display_text_ = std::move(text); }

  // Shows the bar and scans `tab`. `seed` replaces the query when non-empty
  // (reopening with a selection preloads it, as VSCode does); otherwise the
  // previous query is kept and re-run.
  void Open(TerminalTabState* tab, std::string_view seed = {});
  void Close();
  void ToggleCaseSensitive(TerminalTabState* tab);
  void ToggleWholeWord(TerminalTabState* tab);

  // Rescans when anything the match set depends on has changed. Cheap to call
  // every frame: an unchanged query over unchanged output does nothing, and new
  // output only rescans the visible grid rather than the whole scrollback.
  void Refresh(TerminalTabState* tab);
  // Drops the cached scan identity so the next Refresh rescans from row 0. Used
  // when the query text itself changed under the caller's fingers.
  void Invalidate() { scanned_session_ = nullptr; }

  // Steps the selection by `delta` matches, wrapping at both ends, and scrolls
  // the terminal so the new match is on screen. Returns false when there is
  // nothing to step to.
  bool SelectRelative(TerminalTabState* tab, int delta);

 private:
  void ClearMatches();
  void RebuildCountText();
  // Detaches the terminal from its tail and scrolls the selected match into
  // view, centering it when the panel is tall enough to bother.
  void RevealSelected(TerminalTabState* tab) const;

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

  // Identity of the scan that produced `matches_`. A mismatch on any of these
  // forces a full rescan; matching identity with a newer snapshot generation
  // takes the incremental path from `scanned_stable_row_end_`.
  const void* scanned_session_ = nullptr;
  std::string scanned_query_;
  bool scanned_case_sensitive_ = false;
  bool scanned_whole_word_ = false;
  std::uint64_t scanned_trim_total_ = 0;
  std::size_t scanned_stable_row_end_ = 0;

  // Reused across rows and across scans, so a steady-state rescan allocates
  // nothing. Kept here (not on the stack) precisely for that.
  terminal::TerminalSearchScratch scratch_;
};

}  // namespace microide::workspace
