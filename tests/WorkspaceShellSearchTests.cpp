#include "TestSupport.h"

#include "workspace/WorkspaceShellTestAccess.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include "WorkspaceShellEventHelpers.h"

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

void WaitForProjectSearch(WorkspaceShell& shell) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    WorkspaceShellTestAccess::ConsumeProjectSearchUpdates(shell);
    if (!WorkspaceShellTestAccess::ProjectSearchRunning(shell)) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  Expect(false, "workspace project search should finish");
}

void TestWorkspaceShellProjectSearchHiddenToggleReruns() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "visible.txt", "alpha\n");
  WriteFile(root / ".hidden.txt", "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alpha", false);
  WaitForProjectSearch(shell);

  Expect(WorkspaceShellTestAccess::ProjectSearchError(shell).empty(),
         "initial hidden-toggle search should not error");
  Expect(!WorkspaceShellTestAccess::ProjectSearchTruncated(shell),
         "initial hidden-toggle search should not truncate");
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 1,
         "hidden files should be excluded before the toggle");
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell)[0].relative_path ==
             std::filesystem::path("visible.txt"),
         "hidden-toggle search should keep the visible match first");

  WorkspaceShellTestAccess::ToggleProjectSearchHiddenFiles(shell);
  WaitForProjectSearch(shell);

  const auto& hidden_results = WorkspaceShellTestAccess::ProjectSearchResults(shell);
  Expect(hidden_results.size() == 2,
         "hidden-file toggle should rerun the search and include hidden matches");
  const bool has_hidden = std::any_of(hidden_results.begin(), hidden_results.end(),
                                      [](const auto& result) {
                                        return result.relative_path ==
                                               std::filesystem::path(".hidden.txt");
                                      });
  Expect(has_hidden, "hidden-file toggle should surface the hidden result");
}

void TestWorkspaceShellProjectSearchPatternModeToggleReruns() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "notes.txt", "alp.a\nalpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alp.a", false);
  WaitForProjectSearch(shell);

  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 1,
         "literal project search should treat dots as plain characters");
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell)[0].line == 0,
         "literal project search should keep the literal-only match");

  WorkspaceShellTestAccess::ToggleProjectSearchPatternMode(shell);
  WaitForProjectSearch(shell);

  const auto& regex_results = WorkspaceShellTestAccess::ProjectSearchResults(shell);
  Expect(regex_results.size() == 2,
         "pattern-mode toggle should rerun the search in regex mode");
  Expect(regex_results[0].line == 0 && regex_results[1].line == 1,
         "regex project search should include both literal and regex-expanded matches");
}

void TestWorkspaceShellProjectSearchCaseModeCycleReruns() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "notes.txt", "Alpha alpha ALPHA\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "Alpha", false);
  WaitForProjectSearch(shell);

  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 1,
         "smart-case project search should start case-sensitive for uppercase queries");

  WorkspaceShellTestAccess::CycleProjectSearchCaseMode(shell);
  WaitForProjectSearch(shell);
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 1,
         "explicit sensitive case mode should preserve the exact-case result count");

  WorkspaceShellTestAccess::CycleProjectSearchCaseMode(shell);
  WaitForProjectSearch(shell);
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 3,
         "explicit insensitive case mode should rerun and match every case variant");

  WorkspaceShellTestAccess::CycleProjectSearchCaseMode(shell);
  WaitForProjectSearch(shell);
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 1,
         "cycling back to smart case should restore the original result count");
}

void TestWorkspaceShellProjectSearchRerunClearsTruncation() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  // Each file individually holds MORE than kMaxProjectSearchResults (200) matches, so
  // a single worker scanning one file sequentially is guaranteed to attempt a match
  // past the cap and flag truncation. A near-boundary fixture (e.g. 250 total across
  // 10 files) is racy: parallel early-stop can cancel stragglers before any worker
  // attempts the (cap+1)-th match, leaving `truncated` unset. 250 lines/file removes
  // that race deterministically.
  std::string repeated_lines;
  for (int line = 0; line < 250; ++line) {
    repeated_lines += "alpha\n";
  }
  for (int file_index = 0; file_index < 10; ++file_index) {
    const std::string label = "0" + std::to_string(file_index);
    WriteFile(root / ("file" + label + ".txt"), repeated_lines);
  }
  WriteFile(root / "nomatch.txt", "omega\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alpha", false);
  WaitForProjectSearch(shell);

  Expect(WorkspaceShellTestAccess::ProjectSearchTruncated(shell),
         "workspace search should remember when the backend capped results");
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 200,
         "workspace search should keep the full capped result set");

  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "omega", false);
  WaitForProjectSearch(shell);

  Expect(!WorkspaceShellTestAccess::ProjectSearchTruncated(shell),
         "rerunning project search with a smaller query should clear truncation state");
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 1,
         "rerunning project search should replace the old capped results");
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell)[0].relative_path ==
             std::filesystem::path("nomatch.txt"),
         "rerunning project search should publish only the new query results");
}

// TD-2026-07-17A-084/026: the grouped project-search line map is cached on
// ProjectSearchState keyed by a results revision, so render/hit-test/navigation
// share one build. A rerun that replaces the results must bump the revision and
// rebuild the map instead of serving the stale grouping.
void TestWorkspaceShellProjectSearchLineMapCacheInvalidatesOnRerun() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "a.txt", "alpha\nalpha\n");
  WriteFile(root / "b.txt", "alpha\n");
  WriteFile(root / "only.txt", "omega\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alpha", false);
  WaitForProjectSearch(shell);

  // 3 matches across 2 files => 2 file-header rows + 3 result rows = 5 lines.
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 3,
         "alpha search finds three matches");
  const auto map_before = WorkspaceShellTestAccess::ProjectSearchLineMap(shell);
  Expect(map_before.size() == 5, "line map groups three results under two file headers");
  const std::uint64_t revision_before =
      WorkspaceShellTestAccess::ProjectSearchResultsRevision(shell);

  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "omega", false);
  WaitForProjectSearch(shell);

  Expect(WorkspaceShellTestAccess::ProjectSearchResultsRevision(shell) != revision_before,
         "rerunning the search bumps the results revision");
  const auto map_after = WorkspaceShellTestAccess::ProjectSearchLineMap(shell);
  // 1 match in 1 file => 1 header + 1 result = 2 lines. A stale cache would still
  // report the 5-line alpha grouping.
  Expect(map_after.size() == 2, "line map rebuilds for the new result set, not served stale");
}

void TestWorkspaceShellProjectSearchStreamsWhileRunning() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  std::string repeated_lines;
  for (int line = 0; line < 40; ++line) {
    repeated_lines += "alpha\n";
  }
  for (int file_index = 0; file_index < 220; ++file_index) {
    const std::string label = file_index < 10 ? "00" + std::to_string(file_index)
                                              : (file_index < 100
                                                     ? "0" + std::to_string(file_index)
                                                     : std::to_string(file_index));
    WriteFile(root / ("file" + label + ".txt"), repeated_lines);
  }

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alpha", false);

  bool saw_results_while_running = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    WorkspaceShellTestAccess::ConsumeProjectSearchUpdates(shell);
    const bool running = WorkspaceShellTestAccess::ProjectSearchRunning(shell);
    const std::size_t count = WorkspaceShellTestAccess::ProjectSearchResults(shell).size();
    if (count > 0) {
      saw_results_while_running = true;
      break;
    }
    if (!running) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  WaitForProjectSearch(shell);
  Expect(saw_results_while_running,
         "workspace search should display wake-driven result updates before completion handling");
}

void TestWorkspaceShellProjectSearchSidebarClickOpensResult() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "alpha\nbeta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alpha", false);
  WaitForProjectSearch(shell);

  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 1,
         "search click fixture should expose one result");
  const SDL_FRect result_rect = WorkspaceShellTestAccess::ProjectSearchResultRect(shell, 0);
  Expect(SendMouseDown(
             shell, result_rect.x + result_rect.w * 0.5f,
             result_rect.y + result_rect.h * 0.5f, SDL_BUTTON_LEFT),
         "clicking a project search result should be handled");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == source.lexically_normal(),
         "clicking a project search result should open the matched file");
  // Opening a result also seeds the in-file find surface with the search term so the
  // user can keep moving between matches in the file they just opened.
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell) &&
             WorkspaceShellTestAccess::ActiveOverlayMode(shell) ==
                 WorkspaceShell::OverlayMode::BufferSearch,
         "clicking a project search result should open the in-file find surface");
  Expect(WorkspaceShellTestAccess::FocusIsOverlay(shell),
         "clicking a project search result should focus the in-file find surface");
  Expect(WorkspaceShellTestAccess::BufferSearchQuery(shell) == "alpha",
         "the in-file find surface should be seeded with the project search term");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 1,
         "the in-file find surface should expose the in-file matches for the term");
}

void TestWorkspaceShellProjectSearchSidebarClickMovesToCorrectLine() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  const std::filesystem::path source = root / "src" / "main.cpp";
  WriteFile(source, "target-one\nmiddle\ntarget-two\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "target", false);
  WaitForProjectSearch(shell);

  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 2,
         "search line fixture should expose two results in one file");
  const SDL_FRect second_result_rect = WorkspaceShellTestAccess::ProjectSearchResultRect(shell, 1);
  Expect(SendMouseDown(shell, second_result_rect.x + second_result_rect.w * 0.5f,
                       second_result_rect.y + second_result_rect.h * 0.5f, SDL_BUTTON_LEFT),
         "clicking the second project search result should be handled");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).cursor_line() == 2,
         "clicking the second project search result should move to its line");
}

void TestWorkspaceShellProjectSearchReusesCachedResultsOnReturn() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "notes.txt", "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alpha", false);
  WaitForProjectSearch(shell);
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 1,
         "initial search should find the single match");

  // Switch the sidebar to another panel, then return to search. The completed
  // results must be reused: the search must not re-run (which previously cleared
  // results and restarted the worker every time the panel was reopened).
  WorkspaceShellTestAccess::ShowProblemsSidebar(shell);
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) != WorkspaceShell::SidebarMode::Search,
         "switching away should leave the search panel");

  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "", false);
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Search,
         "returning should show the search panel again");
  Expect(!WorkspaceShellTestAccess::ProjectSearchRunning(shell),
         "returning to the search panel must not re-run the completed search");
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 1,
         "returning to the search panel must keep the cached results");
  Expect(WorkspaceShellTestAccess::ProjectSearchQuery(shell) == "alpha",
         "returning to the search panel must keep the query text");
}

void TestWorkspaceShellBufferSearchIsNonModal() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "alpha alpha\nbeta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL), "Ctrl+F should open the find widget");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "alpha"),
         "the find widget should accept a query");
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell) &&
             WorkspaceShellTestAccess::ActiveOverlayMode(shell) ==
                 WorkspaceShell::OverlayMode::BufferSearch,
         "the find widget should be open in buffer-search mode");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 2,
         "find should locate both matches in the buffer");
  Expect(WorkspaceShellTestAccess::FocusIsOverlay(shell) &&
             WorkspaceShellTestAccess::BufferSearchSurfaceFocused(shell),
         "the freshly opened find widget should hold focus");

  // Click in the editor, far from the top-right widget: the widget must stay open
  // (non-modal) while focus moves to the editor underneath it.
  const SDL_FRect editor_area = WorkspaceShellTestAccess::EditorAreaRect(shell);
  Expect(SendMouseDown(shell, editor_area.x + 24.0f, editor_area.y + editor_area.h - 24.0f,
                       SDL_BUTTON_LEFT),
         "clicking the editor under the floating widget should be handled");
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell),
         "clicking the editor must NOT dismiss the non-modal find widget");
  Expect(WorkspaceShellTestAccess::FocusIsEditor(shell),
         "clicking the editor should move focus to the editor");
  Expect(WorkspaceShellTestAccess::TextInputSurfaceIsEditor(shell),
         "with the editor focused the text-input surface belongs to the editor, not the query");

  // Typing now edits the document, not the find query.
  const std::string query_before = WorkspaceShellTestAccess::BufferSearchQuery(shell);
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "Z"),
         "typing should be accepted by the focused editor");
  Expect(WorkspaceShellTestAccess::BufferSearchQuery(shell) == query_before,
         "typing while the editor is focused must not change the find query");
}

// Regression: Cut/SelectAll shortcuts in a focused single-line surface (find
// widget, file finder, commit subject, ...) must be consumed by that surface
// even when it has nothing to act on -- they must NOT fall through to the
// background editor. The worst case was Ctrl+X with no selection deleting a line
// from the editor document behind the widget.
void TestWorkspaceShellCutInSingleLineSurfaceDoesNotEditEditor() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "alpha alpha\nbeta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  const std::size_t lines_before = WorkspaceShellTestAccess::ActiveEditor(shell).lines().LineCount();

  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL), "Ctrl+F should open the find widget");
  Expect(WorkspaceShellTestAccess::BufferSearchSurfaceFocused(shell),
         "the freshly opened find query field should hold focus");

  // Ctrl+A on the empty query must not Select-All the editor or steal focus to it.
  SendKeyDown(shell, SDLK_A, SDL_KMOD_CTRL);
  Expect(WorkspaceShellTestAccess::BufferSearchSurfaceFocused(shell),
         "Ctrl+A in a focused single-line field must not move focus to the editor");

  // Type a query (no selection), then Ctrl+X. This must not delete a line from
  // the editor behind the widget (regression: it ran viewport->DeleteCurrentLine()).
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "alpha"),
         "the find widget should accept a query");
  SendKeyDown(shell, SDLK_X, SDL_KMOD_CTRL);
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).lines().LineCount() == lines_before,
         "Ctrl+X in a focused single-line field must not edit the background editor");
  Expect(!WorkspaceShellTestAccess::ActiveEditor(shell).dirty(),
         "the background editor must stay clean after Ctrl+X in a single-line field");
}

void TestWorkspaceShellBufferSearchEnterCyclesMatches() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "alpha\nalpha\nalpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL), "Ctrl+F should open the find widget");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "alpha"),
         "the find widget should accept a query");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 3,
         "the query should match all three lines");
  Expect(WorkspaceShellTestAccess::BufferSearchSelectedIndex(shell) == 0,
         "the first match should be selected initially");

  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE), "Enter should advance to the next match");
  Expect(WorkspaceShellTestAccess::BufferSearchSelectedIndex(shell) == 1,
         "Enter should move to the second match");
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell),
         "Enter must keep the non-modal find widget open");
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE), "Enter should advance again");
  Expect(WorkspaceShellTestAccess::BufferSearchSelectedIndex(shell) == 2,
         "Enter should move to the third match");
  // Next on the last match wraps to the first.
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE), "Enter on the last match should wrap");
  Expect(WorkspaceShellTestAccess::BufferSearchSelectedIndex(shell) == 0,
         "Enter past the last match should wrap to the first");
  // Previous on the first match wraps to the last.
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_SHIFT),
         "Shift+Enter on the first match should wrap");
  Expect(WorkspaceShellTestAccess::BufferSearchSelectedIndex(shell) == 2,
         "Shift+Enter before the first match should wrap to the last");
}

void TestWorkspaceShellBufferSearchReopenKeepsQueryAndRefocuses() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "beta gamma\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL), "Ctrl+F should open the find widget");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "beta"),
         "the find widget should accept a query");

  // Move focus to the editor (widget stays open), then re-press Ctrl+F.
  const SDL_FRect editor_area = WorkspaceShellTestAccess::EditorAreaRect(shell);
  Expect(SendMouseDown(shell, editor_area.x + 24.0f, editor_area.y + editor_area.h - 24.0f,
                       SDL_BUTTON_LEFT),
         "clicking the editor should be handled");
  Expect(WorkspaceShellTestAccess::FocusIsEditor(shell), "clicking the editor should focus it");

  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL),
         "Ctrl+F should re-focus the already-open find widget");
  Expect(WorkspaceShellTestAccess::FocusIsOverlay(shell),
         "re-pressing Ctrl+F should return focus to the widget");
  Expect(WorkspaceShellTestAccess::BufferSearchQuery(shell) == "beta",
         "re-opening the find widget should keep the existing query");
}

void TestWorkspaceShellBufferSearchSeedsFromSelection() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "needle haystack needle\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  // Select the first word, then open find: the selection should seed the query.
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.MoveCursorTo(0, 0);
  editor.SelectWordAtCursor();

  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL), "Ctrl+F should open the find widget");
  Expect(WorkspaceShellTestAccess::BufferSearchQuery(shell) == "needle",
         "opening find with a selection should seed the query from it");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 2,
         "the seeded query should immediately find both occurrences");
}

}  // namespace

void TestWorkspaceShellProjectSearchSidebarScrollPastSelection() {
  EnsureDummySdlVideoInitialized();
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  std::string content;
  for (int i = 0; i < 90; ++i) {
    content += "needle here\n";
  }
  WriteFile(root / "many.txt", content);

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "needle", false);
  WaitForProjectSearch(shell);
  const std::size_t result_count = WorkspaceShellTestAccess::ProjectSearchResults(shell).size();
  Expect(result_count >= 60,
         "scroll fixture should produce enough results to overflow the visible sidebar");
  Expect(WorkspaceShellTestAccess::SidebarScrollRow(shell) == 0,
         "a fresh search should start scrolled to the top");

  WorkspaceShellTestAccess::SetFocusSidebar(shell);

  // End navigates to the last result and scrolls it into view.
  Expect(SendKeyDown(shell, SDLK_END, SDL_KMOD_NONE),
         "End should be handled by the focused search sidebar");
  Expect(WorkspaceShellTestAccess::ProjectSearchSelectedIndex(shell) == result_count - 1,
         "End should select the last result");
  Expect(WorkspaceShellTestAccess::SidebarScrollRow(shell) > 0,
         "navigating to the last result should reveal it (scroll follows the selection)");

  // The user can now scroll back up past the (still-selected) last row, and a
  // non-navigation event must not force the selection back into view.
  WorkspaceShellTestAccess::SetSidebarScrollRow(shell, 0);
  WorkspaceShellTestAccess::ConsumeProjectSearchUpdates(shell);
  Expect(WorkspaceShellTestAccess::SidebarScrollRow(shell) == 0,
         "scrolling away from the selected entry should be preserved");

  // Home navigates back to the first result and reveals it at the top.
  Expect(SendKeyDown(shell, SDLK_HOME, SDL_KMOD_NONE),
         "Home should be handled by the focused search sidebar");
  Expect(WorkspaceShellTestAccess::ProjectSearchSelectedIndex(shell) == 0,
         "Home should select the first result");
  Expect(WorkspaceShellTestAccess::SidebarScrollRow(shell) == 0,
         "navigating to the first result should reveal it at the top");
}

namespace {

// Reconstruct the active editor's document text (lines joined with '\n') for
// assertions.
std::string ActiveEditorDocText(WorkspaceShell& shell) {
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  std::string out;
  for (std::size_t i = 0; i < editor.lines().LineCount(); ++i) {
    if (i != 0) {
      out += '\n';
    }
    out += std::string(editor.lines().LineView(i));
  }
  return out;
}

}  // namespace

// In-file regex replace-all expands capture groups across every match and applies
// as ONE undo group, so a single undo restores the original buffer.
void TestWorkspaceShellBufferRegexReplaceAll() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  const std::filesystem::path source = root / "kv.txt";
  WriteFile(source, "a=1\nb=2\nc=3");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  WorkspaceShellTestAccess::SetBufferSearchRegexAndRefresh(shell, true);
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "(\\w)=(\\d)");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 3,
         "regex find should match all three key=value lines");

  WorkspaceShellTestAccess::SetBufferReplaceText(shell, "$2$1");
  WorkspaceShellTestAccess::ReplaceAllBufferSearchMatches(shell);
  Expect(ActiveEditorDocText(shell) == "1a\n2b\n3c",
         "regex replace-all should expand $1/$2 on every match");

  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).Undo(),
         "a single undo should be available after regex replace-all");
  Expect(ActiveEditorDocText(shell) == "a=1\nb=2\nc=3",
         "one undo should restore the whole regex replace-all as a single group");
}

// In-file regex "Replace" (current match) expands only the selected occurrence.
void TestWorkspaceShellBufferRegexReplaceCurrent() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  const std::filesystem::path source = root / "nums.txt";
  WriteFile(source, "x12 y34");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  WorkspaceShellTestAccess::SetBufferSearchRegexAndRefresh(shell, true);
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "(\\d+)");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 2,
         "regex find should match both digit runs");

  WorkspaceShellTestAccess::SetBufferReplaceText(shell, "[$1]");
  WorkspaceShellTestAccess::SetBufferSearchSelectedIndex(shell, 0);
  WorkspaceShellTestAccess::ReplaceCurrentBufferSearchMatch(shell);
  Expect(ActiveEditorDocText(shell) == "x[12] y34",
         "replace-current should expand only the selected match");
}

// Alt+R toggles regex mode in the find widget and recomputes matches; an invalid
// pattern yields no matches (0/0) without crashing.
void TestWorkspaceShellBufferRegexToggleViaAltR() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  const std::filesystem::path source = root / "digits.txt";
  WriteFile(source, "a1 b2\nc3\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL), "Ctrl+F should open the find widget");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "\\d"),
         "the find widget should accept a regex query");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 0,
         "in literal mode the query '\\d' matches nothing");

  Expect(SendKeyDown(shell, SDLK_R, SDL_KMOD_ALT), "Alt+R should toggle regex mode");
  Expect(WorkspaceShellTestAccess::BufferSearchRegexEnabled(shell),
         "Alt+R should enable regex mode");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 3,
         "after enabling regex, '\\d' should match every digit");

  // An invalid pattern must not crash and yields no matches.
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "[unterminated");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 0,
         "an invalid regex should produce zero matches without crashing");
}

void RegisterWorkspaceShellSearchTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/BufferRegexReplaceAll",
          TestWorkspaceShellBufferRegexReplaceAll);
  AddTest(tests, "WorkspaceShell/BufferRegexReplaceCurrent",
          TestWorkspaceShellBufferRegexReplaceCurrent);
  AddTest(tests, "WorkspaceShell/BufferRegexToggleViaAltR",
          TestWorkspaceShellBufferRegexToggleViaAltR);
  AddTest(tests, "WorkspaceShell/ProjectSearchSidebarScrollPastSelection",
          TestWorkspaceShellProjectSearchSidebarScrollPastSelection);
  AddTest(tests, "WorkspaceShell/ProjectSearchHiddenToggleReruns",
          TestWorkspaceShellProjectSearchHiddenToggleReruns);
  AddTest(tests, "WorkspaceShell/ProjectSearchPatternModeToggleReruns",
          TestWorkspaceShellProjectSearchPatternModeToggleReruns);
  AddTest(tests, "WorkspaceShell/ProjectSearchCaseModeCycleReruns",
          TestWorkspaceShellProjectSearchCaseModeCycleReruns);
  AddTest(tests, "WorkspaceShell/ProjectSearchRerunClearsTruncation",
          TestWorkspaceShellProjectSearchRerunClearsTruncation);
  AddTest(tests, "WorkspaceShell/ProjectSearchLineMapCacheInvalidatesOnRerun",
          TestWorkspaceShellProjectSearchLineMapCacheInvalidatesOnRerun);
  AddTest(tests, "WorkspaceShell/ProjectSearchStreamsWhileRunning",
          TestWorkspaceShellProjectSearchStreamsWhileRunning);
  AddTest(tests, "WorkspaceShell/ProjectSearchSidebarClickOpensResult",
          TestWorkspaceShellProjectSearchSidebarClickOpensResult);
  AddTest(tests, "WorkspaceShell/ProjectSearchReusesCachedResultsOnReturn",
          TestWorkspaceShellProjectSearchReusesCachedResultsOnReturn);
  AddTest(tests, "WorkspaceShell/ProjectSearchSidebarClickMovesToCorrectLine",
          TestWorkspaceShellProjectSearchSidebarClickMovesToCorrectLine);
  AddTest(tests, "WorkspaceShell/BufferSearchIsNonModal", TestWorkspaceShellBufferSearchIsNonModal);
  AddTest(tests, "WorkspaceShell/CutInSingleLineSurfaceDoesNotEditEditor",
          TestWorkspaceShellCutInSingleLineSurfaceDoesNotEditEditor);
  AddTest(tests, "WorkspaceShell/BufferSearchEnterCyclesMatches",
          TestWorkspaceShellBufferSearchEnterCyclesMatches);
  AddTest(tests, "WorkspaceShell/BufferSearchReopenKeepsQueryAndRefocuses",
          TestWorkspaceShellBufferSearchReopenKeepsQueryAndRefocuses);
  AddTest(tests, "WorkspaceShell/BufferSearchSeedsFromSelection",
          TestWorkspaceShellBufferSearchSeedsFromSelection);
}

}  // namespace microide::tests
