#include "TestSupport.h"
#include "TestSupportSdl.h"

#include "platform/FileIndexWatcher.h"
#include "workspace/BufferFindScope.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include "WorkspaceShellEventHelpers.h"

namespace microide::tests {
namespace {

using microide::platform::IndexUpdateBatch;
using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;

void WaitForProjectSearch(WorkspaceShell& shell) {
  const bool finished = WaitUntil(
      [&shell]() { return !WorkspaceShellTestAccess::ProjectSearchRunning(shell); },
      std::chrono::seconds(2), std::chrono::milliseconds(5),
      [&shell]() { WorkspaceShellTestAccess::ConsumeProjectSearchUpdates(shell); });
  Expect(finished, "workspace project search should finish");
}

// `/` on the search view's result list edits the query -- and, being a printable
// key, SDL follows the press with a TEXT_INPUT "/" that typed into the field it
// had just opened: the query read "alpha/" and the search went empty (found by
// the QA search-view-keys flow). The press's text is dropped; the next one types.
void TestWorkspaceShellSearchFieldShortcutsDoNotTypeThemselves() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "visible.txt", "alpha\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alpha", false);
  WaitForProjectSearch(shell);
  WorkspaceShellTestAccess::SetFocusSidebar(shell);
  if (WorkspaceShellTestAccess::ProjectSearchEditing(shell)) {
    SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE);
  }
  Expect(!WorkspaceShellTestAccess::ProjectSearchEditing(shell), "the list, not a field, has the keys");

  const auto press = [&](SDL_Keycode key, const char* text) {
    SendKeyDown(shell, key, SDL_KMOD_NONE);
    shell.HandleEvent(TextInputEvent(text));
  };
  const auto buffer = [&]() { return WorkspaceShellTestAccess::ProjectSearchEditBufferText(shell); };
  SendKeyDown(shell, SDLK_SLASH, SDL_KMOD_NONE);
  Expect(WorkspaceShellTestAccess::TextInputSwallowArmed(shell), "the / press arms the swallow");
  shell.HandleEvent(TextInputEvent("/"));
  Expect(WorkspaceShellTestAccess::ProjectSearchEditing(shell) &&
             WorkspaceShellTestAccess::ProjectSearchEditFieldValue(shell) ==
                 microide::workspace::ProjectSearchEditField::Query,
         "/ opens the query field");
  Expect(buffer() == "alpha", "the / itself is not typed into it (" + buffer() + ")");
  shell.HandleEvent(TextInputEvent("z"));
  Expect(buffer() == "alphaz", "the next character types (" + buffer() + ")");
  SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE);
  for (const auto& [key, text, field] : {
           std::tuple{SDLK_EQUALS, "=", microide::workspace::ProjectSearchEditField::Replace},
           std::tuple{SDLK_I, "i", microide::workspace::ProjectSearchEditField::Include},
           std::tuple{SDLK_X, "x", microide::workspace::ProjectSearchEditField::Exclude}}) {
    press(key, text);
    Expect(WorkspaceShellTestAccess::ProjectSearchEditing(shell) &&
               WorkspaceShellTestAccess::ProjectSearchEditFieldValue(shell) == field,
           std::string(text) + " opens its field");
    Expect(buffer().empty(), std::string(text) + " is not typed into it (" + buffer() + ")");
    SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE);
  }
  // A press that arms but is followed by another KEY_DOWN, not text: the arm is
  // spent, so the character after it still types.
  press(SDLK_SLASH, "/");
  SendKeyDown(shell, SDLK_END, SDL_KMOD_NONE);
  shell.HandleEvent(TextInputEvent("q"));
  Expect(buffer() == "alphaq", "a key after the shortcut disarms it (" + buffer() + ")");
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

// TD-2026-07-17-008/033: when the file index the candidate set is drawn from was
// itself truncated (project too large), project search pins index_incomplete at
// kickoff so the UI can flag that results are not authoritative over the whole
// tree. A later search over a complete index clears it.
void TestWorkspaceShellProjectSearchSurfacesIncompleteIndex() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "a.txt", "alpha\n");
  WriteFile(root / "b.txt", "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

  // Force the backing index into the "truncated" state (as a huge-tree initial
  // scan would), listing the on-disk files so the search still finds matches.
  IndexUpdateBatch batch;
  batch.is_initial = true;
  batch.truncated = true;
  for (const char* name : {"a.txt", "b.txt"}) {
    IndexUpdateBatch::Change change;
    change.kind = IndexUpdateBatch::Kind::CreatedOrModified;
    change.entry.relative_path = name;
    batch.changes.push_back(change);
  }
  WorkspaceShellTestAccess::ApplyFileIndexBatchForTesting(shell, std::move(batch));

  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alpha", false);
  WaitForProjectSearch(shell);
  Expect(WorkspaceShellTestAccess::ProjectSearchIndexIncomplete(shell),
         "project search over a truncated index must flag results as incomplete");
  Expect(!WorkspaceShellTestAccess::ProjectSearchResults(shell).empty(),
         "a truncated index still yields the matches it does contain");

  // A rescan of this small tree completes; the next search must clear the flag.
  WorkspaceShellTestAccess::ForceFileIndexRefreshAndDrain(shell);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alpha", false);
  WaitForProjectSearch(shell);
  Expect(!WorkspaceShellTestAccess::ProjectSearchIndexIncomplete(shell),
         "searching over a complete index clears the incomplete flag");
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

  // Stop as soon as results appear (the wake-driven update we assert on) or the
  // search finishes without any; `saw_results_while_running` records which.
  WaitUntil(
      [&shell]() {
        return WorkspaceShellTestAccess::ProjectSearchResults(shell).size() > 0 ||
               !WorkspaceShellTestAccess::ProjectSearchRunning(shell);
      },
      std::chrono::seconds(2), std::chrono::milliseconds(2),
      [&shell]() { WorkspaceShellTestAccess::ConsumeProjectSearchUpdates(shell); });
  const bool saw_results_while_running =
      WorkspaceShellTestAccess::ProjectSearchResults(shell).size() > 0;

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

// Find searches from the cursor (VS Code): the current match is the first one at
// or after the caret, wrapping to the top, not the first in the document.
void TestWorkspaceShellBufferSearchStartsFromTheCaret() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "alpha\nalpha\nalpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);

  editor.MoveCursorTo(1, 0);
  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL), "Ctrl+F should open the find widget");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "alpha"), "the query is typed");
  Expect(WorkspaceShellTestAccess::BufferSearchSelectedIndex(shell) == 1,
         "the match at the caret is the current one, not the first in the file");
  Expect(editor.cursor_line() == 1, "the caret stays on its line");

  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE), "Escape closes the widget");
  editor.MoveCursorTo(1, 2);  // inside the second occurrence: the next one is current
  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL), "Ctrl+F reopens the find widget");
  Expect(WorkspaceShellTestAccess::BufferSearchSelectedIndex(shell) == 2,
         "a match that starts before the caret is not current; the next one is");

  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE), "Escape closes the widget");
  editor.MoveCursorTo(2, 3);  // past every match start: wrap to the top
  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL), "Ctrl+F reopens the find widget");
  Expect(WorkspaceShellTestAccess::BufferSearchSelectedIndex(shell) == 0,
         "with no match at or after the caret the search wraps to the first");
}

// Replace moves on to the NEXT occurrence (the caret sits after the replacement),
// not back to the first match in the document.
void TestWorkspaceShellBufferReplaceMovesToTheNextMatch() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "a a a a\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.MoveCursorTo(0, 0);

  Expect(SendKeyDown(shell, SDLK_H, SDL_KMOD_CTRL), "Ctrl+H should open the replace widget");
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "a");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 4, "four matches");
  WorkspaceShellTestAccess::SetBufferSearchSelectedIndex(shell, 2);  // the third match is current

  WorkspaceShellTestAccess::SetBufferReplaceText(shell, "b");
  WorkspaceShellTestAccess::ReplaceCurrentBufferSearchMatch(shell);
  Expect(editor.lines()[0] == "a a b a", "the third occurrence was replaced: " +
                                            std::string(editor.lines()[0]));
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 3, "three matches remain");
  Expect(WorkspaceShellTestAccess::BufferSearchSelectedIndex(shell) == 2,
         "the match after the replacement (the old fourth) is current, not the first");

  WorkspaceShellTestAccess::ReplaceCurrentBufferSearchMatch(shell);
  Expect(editor.lines()[0] == "a a b b", "the last occurrence was replaced next");
  Expect(WorkspaceShellTestAccess::BufferSearchSelectedIndex(shell) == 0,
         "past the last match the selection wraps to the first");
}

// In the replace widget, Enter in the find field steps to the next match (as in
// the find-only widget and VS Code); only Enter in the replace field replaces.
void TestWorkspaceShellBufferReplaceEnterInFindFieldStepsMatches() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "a a a\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.MoveCursorTo(0, 0);

  Expect(SendKeyDown(shell, SDLK_H, SDL_KMOD_CTRL), "Ctrl+H should open the replace widget");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "a"), "the query is typed");
  WorkspaceShellTestAccess::SetBufferReplaceText(shell, "b");
  Expect(WorkspaceShellTestAccess::BufferSearchSelectedIndex(shell) == 0, "the first match is current");

  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE), "Enter in the find field is handled");
  Expect(editor.lines()[0] == "a a a", "Enter in the find field does not edit the buffer");
  Expect(WorkspaceShellTestAccess::BufferSearchSelectedIndex(shell) == 1,
         "Enter in the find field steps to the next match");
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_SHIFT), "Shift+Enter steps back");
  Expect(WorkspaceShellTestAccess::BufferSearchSelectedIndex(shell) == 0, "back to the first");

  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_NONE), "Tab moves to the replace field");
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE), "Enter in the replace field is handled");
  Expect(editor.lines()[0] == "b a a", "Enter in the replace field replaces the current match: " +
                                          std::string(editor.lines()[0]));
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

// In-file regex matches across line breaks: `\n` finds the newlines, a multi-line
// pattern spans two lines, and replacing `\n` joins the lines (the reported bug —
// per-line search could never match `\n`).
void TestWorkspaceShellBufferRegexMultilineNewline() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  const std::filesystem::path source = root / "lines.txt";
  WriteFile(source, "a=1\nb=2\nc=3");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::SetBufferSearchRegexAndRefresh(shell, true);

  // `\n` matches the two line breaks between the three lines.
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "\\n");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 2,
         "regex `\\n` should find the two newlines (per-line search found none)");

  // A pattern that spans a line break matches across lines (digit, newline, word).
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "\\d\\n\\w");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 2,
         "a multi-line regex should match across line breaks");

  // Replacing `\n` joins the lines into one.
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "\\n");
  WorkspaceShellTestAccess::SetBufferReplaceText(shell, " | ");
  WorkspaceShellTestAccess::ReplaceAllBufferSearchMatches(shell);
  Expect(ActiveEditorDocText(shell) == "a=1 | b=2 | c=3",
         "replacing `\\n` should join the lines");
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


// The set of single-line text fields was hand-written at four sites — the key
// handler's Ctrl+A/C/X routing, its Ctrl+V routing, and the SelectAll and Paste
// availability rules — and the lists had diverged: the key handler counted the
// commit subject and the terminal find bar, the availability rules did not.
//
// The divergence is masked today, because both availability rules short-circuit
// on `active_viewport != nullptr` long before they consult the surface. This pins
// the shared predicate itself, which is the thing that can rot: every field the
// key handler routes chords for has to be in it, and the multi-line surfaces have
// to stay out.
void TestWorkspaceShellSingleLineTextSurfacePredicateCoversEveryField() {
  using microide::workspace::IsSingleLineTextInputSurface;
  using Surface = microide::workspace::TextInputSurface;

  const Surface single_line[] = {
      Surface::PromptInput,         Surface::FileFinder,
      Surface::BufferSearch,        Surface::BufferReplaceSearch,
      Surface::BufferReplaceReplace, Surface::ProjectSearchOverlay,
      Surface::CommitPicker,        Surface::QuickPick,
      Surface::CommandPalette,      Surface::SidebarSearchQuery,
      Surface::SidebarSearchReplace, Surface::SidebarSearchInclude,
      Surface::SidebarSearchExclude, Surface::CommitSubject,
      Surface::TerminalFind,        Surface::SettingsQuery,
      Surface::SettingsValueEdit,   Surface::DebugVariableEdit,
  };
  for (const Surface surface : single_line) {
    Expect(IsSingleLineTextInputSurface(surface),
           "every single-line field must be covered by the shared predicate");
  }

  // Multi-line surfaces and "no surface" must stay out: they have their own
  // selection and paste handling.
  const Surface not_single_line[] = {Surface::None, Surface::Editor, Surface::CommitBody,
                                     Surface::Terminal};
  for (const Surface surface : not_single_line) {
    Expect(!IsSingleLineTextInputSurface(surface),
           "multi-line surfaces must not be treated as single-line fields");
  }

  // The two the old lists disagreed about, called out so a revert is obvious.
  Expect(IsSingleLineTextInputSurface(Surface::CommitSubject) &&
             IsSingleLineTextInputSurface(Surface::TerminalFind),
         "the commit subject and the terminal find bar are single-line fields");
}

// The in-file find widget gained the `Aa` and `ab` toggles the terminal find bar
// already had. Aa applies in literal AND regex mode: regex used to be smart-case
// while literal was always insensitive, so flipping `.*` silently changed whether
// `Alpha` matched `alpha`.
void TestWorkspaceShellBufferFindMatchCaseAppliesToBothModes() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  const std::filesystem::path source = root / "case.txt";
  WriteFile(source, "Alpha alpha ALPHA\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL), "Ctrl+F should open the find widget");
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "Alpha");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 3,
         "with Aa off, a literal query matches every casing");

  Expect(SendKeyDown(shell, SDLK_C, SDL_KMOD_ALT), "Alt+C should toggle match case");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCase(shell), "Alt+C should enable match case");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 1,
         "with Aa on, a literal query matches only the exact casing");

  // Regex mode reads the same toggle rather than deciding case for itself.
  WorkspaceShellTestAccess::SetBufferSearchRegexAndRefresh(shell, true);
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 1,
         "regex should honour the Aa toggle, not its own smart-case rule");
  Expect(SendKeyDown(shell, SDLK_C, SDL_KMOD_ALT), "Alt+C should toggle match case back off");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 3,
         "turning Aa off should make the same regex case-insensitive");
}

void TestWorkspaceShellBufferFindWholeWordFiltersMatches() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  const std::filesystem::path source = root / "words.txt";
  WriteFile(source, "cat concat cat_x cat\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL), "Ctrl+F should open the find widget");
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "cat");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 4,
         "without ab, 'cat' also matches inside concat and cat_x");

  Expect(SendKeyDown(shell, SDLK_W, SDL_KMOD_ALT), "Alt+W should toggle whole word");
  Expect(WorkspaceShellTestAccess::BufferSearchWholeWord(shell),
         "Alt+W should enable whole-word matching");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 2,
         "with ab on, only the standalone 'cat' occurrences match");

  // Typing further would normally take the find-as-you-type refine fast path.
  // Under whole word that path is unsound (a longer query's standalone hits are
  // NOT a subset of a shorter prefix's), so it must cold-scan and still be right.
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "cat_x");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 1,
         "extending the query under whole word must not inherit the prefix filter");
}

// Replace-all has to change exactly the matches the widget highlighted. Under Aa
// or ab the whole-buffer fallback scan means something different from the match
// set, so the two must not be allowed to disagree.
void TestWorkspaceShellBufferFindReplaceAllHonoursOptions() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  const std::filesystem::path source = root / "replace.txt";
  WriteFile(source, "cat concat Cat\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL), "Ctrl+F should open the find widget");
  WorkspaceShellTestAccess::ToggleBufferSearchOption(
      shell, microide::workspace::BufferFindToggle::MatchCase);
  WorkspaceShellTestAccess::ToggleBufferSearchOption(
      shell, microide::workspace::BufferFindToggle::WholeWord);
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "cat");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 1,
         "Aa + ab should leave exactly the standalone lowercase 'cat'");

  WorkspaceShellTestAccess::SetBufferReplaceText(shell, "dog");
  WorkspaceShellTestAccess::ReplaceAllBufferSearchMatches(shell);
  Expect(ActiveEditorDocText(shell) == "dog concat Cat\n",
         "replace-all should change only what the options matched");
}

// Find in Selection (Alt+L, VS Code's fourth toggle): the matches, the n-of-m and
// Replace All are limited to the selection the toggle was turned on over, and
// that scope follows edits -- a Replace All inside it changes its end, a line
// typed above it moves it down.
void TestWorkspaceShellBufferFindInSelectionScopesMatchesAndReplace() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  const std::filesystem::path source = root / "scope.txt";
  WriteFile(source, "cat 0\ncat cat 1\ncat 2\ncat 3\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  editor::TextViewport& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  viewport.JumpCursorTo(1, 0);
  viewport.JumpCursorTo(2, 5, /*extend_selection=*/true);  // lines 1-2

  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL), "Ctrl+F should open the find widget");
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "cat");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 5, "unscoped, all five cats match");
  // Typing the query selected its current match; Alt+L still scopes to the
  // selection the widget opened over, not to the widget's own selection.
  Expect(SendKeyDown(shell, SDLK_L, SDL_KMOD_ALT), "Alt+L should toggle Find in Selection");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 3,
         "in the selection, only the three cats of lines 1-2 match");

  // Typing onto the query takes the refine fast path; the scope must hold there.
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "cat ");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 3,
         "a refined query keeps to the scope");
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "cat");

  // A line typed above the scope moves it down with its text.
  viewport.JumpCursorTo(0, 0);
  viewport.InsertText("new\n");
  WorkspaceShellTestAccess::RefreshBufferSearchIfStale(shell);
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 3,
         "the scope follows its text down past an inserted line");

  WorkspaceShellTestAccess::SetBufferReplaceText(shell, "dog dog");
  WorkspaceShellTestAccess::ReplaceAllBufferSearchMatches(shell);
  Expect(ActiveEditorDocText(shell) == "new\ncat 0\ndog dog dog dog 1\ndog dog 2\ncat 3\n",
         "Replace All in the selection changes only the cats inside it: " + ActiveEditorDocText(shell));
  // The replacement grew the scope's text; its end moved with it, so a second
  // pass over the same scope finds every dog it wrote.
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "dog");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 6,
         "the scope's end follows a Replace All inside it");

  // Off again: the whole file.
  Expect(SendKeyDown(shell, SDLK_L, SDL_KMOD_ALT), "Alt+L should toggle Find in Selection off");
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "cat");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 2,
         "with the toggle off the cats outside the old scope match again");
}

// The Find in Selection scope is carried through each edit the way diagnostics
// are: an edit before it moves it, one inside it moves its end, one across its
// start pulls the start onto the inserted text, and an edit that does not
// describe itself (multi-range) keeps the end where the document's tail says.
void TestBufferFindScopeFollowsEdits() {
  using microide::workspace::BufferSearchState;
  editor::TextViewport viewport;
  viewport.LoadContent("zero\none two\nthree\nfour\n", "/tmp/find-scope-fixture.txt");
  viewport.JumpCursorTo(1, 4);
  viewport.JumpCursorTo(2, 5, /*extend_selection=*/true);  // "two\nthree"
  BufferSearchState::Scope scope = microide::workspace::FindScopeAt(viewport);
  const auto expect_range = [&](std::size_t sl, std::size_t sc, std::size_t el, std::size_t ec,
                                const char* what) {
    Expect(scope.range == editor::SelectionRange{{sl, sc}, {el, ec}},
           std::string(what) + ": got " + std::to_string(scope.range.start.line) + ":" +
               std::to_string(scope.range.start.column) + "-" + std::to_string(scope.range.end.line) + ":" +
               std::to_string(scope.range.end.column));
  };
  expect_range(1, 4, 2, 5, "the selection is the scope");

  Expect(viewport.ReplaceRange({{0, 0}, {0, 0}}, "new\n"), "insert above");
  Expect(microide::workspace::FollowFindScope(scope, viewport), "same document");
  expect_range(2, 4, 3, 5, "a line inserted above moves the scope down");

  Expect(viewport.ReplaceRange({{3, 0}, {3, 5}}, "3"), "edit inside, at the end line");
  Expect(microide::workspace::FollowFindScope(scope, viewport), "same document");
  expect_range(2, 4, 3, 1, "an edit ending at the scope's end keeps the end after the inserted text");

  Expect(viewport.ReplaceRange({{2, 0}, {2, 6}}, "X"), "edit across the start");
  Expect(microide::workspace::FollowFindScope(scope, viewport), "same document");
  expect_range(2, 1, 3, 1, "an edit across the start pulls it onto the inserted text's end");
  Expect(microide::workspace::FollowFindScope(scope, viewport), "a second follow at the same revision");
  expect_range(2, 1, 3, 1, "following twice for one edit does not shift twice");

  // A multi-range edit inside the scope (Replace All) leaves no single edit.
  const std::optional<std::size_t> replaced =
      viewport.ReplaceAllRanges({{{2, 1}, {2, 2}}, {{3, 0}, {3, 1}}}, "long");
  Expect(replaced == std::optional<std::size_t>{2}, "two ranges replaced");
  Expect(!viewport.last_applied_edit().has_value(),
         "a multi-range edit leaves no single edit behind (it once left the previous one)");
  Expect(microide::workspace::FollowFindScope(scope, viewport), "same document");
  expect_range(2, 1, 3, 4, "the end follows the document's tail through a multi-range edit");

  editor::TextViewport other;
  other.LoadContent("other\n", "/tmp/find-scope-other.txt");
  Expect(!microide::workspace::FollowFindScope(scope, other), "another document drops the scope");
}

// The regex Replace All in a scope expands each match against the whole buffer
// (capture groups work) and touches nothing outside the scope.
void TestWorkspaceShellBufferFindInSelectionRegexReplace() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  const std::filesystem::path source = root / "scope.txt";
  WriteFile(source, "a1 a2\na3 a4\na5\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  editor::TextViewport& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);

  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL), "Ctrl+F should open the find widget");
  WorkspaceShellTestAccess::SetBufferSearchRegexAndRefresh(shell, true);
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "a(\\d)");
  viewport.JumpCursorTo(0, 3);
  viewport.JumpCursorTo(1, 2, /*extend_selection=*/true);  // "a2\na3"
  WorkspaceShellTestAccess::ToggleBufferSearchOption(shell, microide::workspace::BufferFindToggle::InSelection);
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 2, "a2 and a3 are in the selection");
  WorkspaceShellTestAccess::SetBufferReplaceText(shell, "<$1>");
  WorkspaceShellTestAccess::ReplaceAllBufferSearchMatches(shell);
  Expect(ActiveEditorDocText(shell) == "a1 <2>\n<3> a4\na5\n",
         "a regex Replace All in the selection expands groups inside it only: " + ActiveEditorDocText(shell));

  // With no selection the scope is the caret's line; a fresh open is unscoped.
  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE), "Escape should close the find widget");
  viewport.JumpCursorTo(2, 0);
  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL), "Ctrl+F should reopen the find widget");
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "a(\\d)");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 3,
         "reopening the widget starts unscoped: a1, a4, a5");
  viewport.JumpCursorTo(2, 0);
  WorkspaceShellTestAccess::ToggleBufferSearchOption(shell, microide::workspace::BufferFindToggle::InSelection);
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 1,
         "with no selection the scope is the caret's line (a5)");
}

// The scope section is collapsed by default (the result list keeps its space) and
// the "..." button expands it, exactly as VS Code's search view does.
void TestWorkspaceShellProjectSearchScopeToggleRevealsGlobFields() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "main.cpp", "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alpha", false);
  WaitForProjectSearch(shell);

  Expect(!WorkspaceShellTestAccess::ProjectSearchScopeExpanded(shell),
         "the scope section should start collapsed");
  Expect(WorkspaceShellTestAccess::SearchSidebarIncludeFieldRect(shell).w == 0.0f,
         "a collapsed include field should have no hit area");

  const SDL_FRect scope_rect = WorkspaceShellTestAccess::SearchSidebarScopeButtonRect(shell);
  Expect(scope_rect.w > 0.0f, "the scope toggle should be laid out");
  Expect(SendMouseDown(shell, scope_rect.x + scope_rect.w * 0.5f,
                       scope_rect.y + scope_rect.h * 0.5f, SDL_BUTTON_LEFT),
         "clicking the scope toggle should be handled");
  Expect(WorkspaceShellTestAccess::ProjectSearchScopeExpanded(shell),
         "clicking the scope toggle should expand the glob fields");

  const SDL_FRect include_rect = WorkspaceShellTestAccess::SearchSidebarIncludeFieldRect(shell);
  const SDL_FRect exclude_rect = WorkspaceShellTestAccess::SearchSidebarExcludeFieldRect(shell);
  Expect(include_rect.w > 0.0f && exclude_rect.w > 0.0f,
         "expanding should lay out both glob fields");
  Expect(exclude_rect.y > include_rect.y, "exclude should sit below include");

  Expect(SendMouseDown(shell, include_rect.x + 4.0f, include_rect.y + include_rect.h * 0.5f,
                       SDL_BUTTON_LEFT),
         "clicking the include field should be handled");
  Expect(WorkspaceShellTestAccess::ProjectSearchEditing(shell),
         "clicking a glob field should begin editing it");
  Expect(WorkspaceShellTestAccess::ProjectSearchEditFieldValue(shell) ==
             microide::workspace::ProjectSearchEditField::Include,
         "clicking the include field should focus the include field");

  // Tab walks query -> replace -> include -> exclude and wraps.
  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_NONE), "Tab should be handled while editing");
  Expect(WorkspaceShellTestAccess::ProjectSearchEditFieldValue(shell) ==
             microide::workspace::ProjectSearchEditField::Exclude,
         "Tab should advance from include to exclude");
  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_NONE), "Tab should wrap");
  Expect(WorkspaceShellTestAccess::ProjectSearchEditFieldValue(shell) ==
             microide::workspace::ProjectSearchEditField::Query,
         "Tab should wrap from the last field back to the query");
  Expect(SendKeyDown(shell, SDLK_TAB, SDL_KMOD_SHIFT), "Shift+Tab should be handled");
  Expect(WorkspaceShellTestAccess::ProjectSearchEditFieldValue(shell) ==
             microide::workspace::ProjectSearchEditField::Exclude,
         "Shift+Tab should walk backwards");
}

// The globs the panel holds must reach the worker: the same query returns a
// different result set once a scope is set, and clearing it restores the full set.
void TestWorkspaceShellProjectSearchScopeGlobsRerunAndRestrictResults() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "src" / "main.cpp", "alpha\n");
  WriteFile(root / "src" / "main.h", "alpha\n");
  WriteFile(root / "docs" / "notes.md", "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alpha", false);
  WaitForProjectSearch(shell);
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 3,
         "the unscoped search should see every file");

  WorkspaceShellTestAccess::SetProjectSearchScopeGlobs(shell, "*.cpp,*.h", "");
  WorkspaceShellTestAccess::RefreshProjectSearch(shell);
  WaitForProjectSearch(shell);
  Expect(WorkspaceShellTestAccess::ProjectSearchError(shell).empty(),
         "a scoped rerun should not error");
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 2,
         "an include scope should drop the out-of-scope file");

  WorkspaceShellTestAccess::SetProjectSearchScopeGlobs(shell, "*.cpp,*.h", "**/*.h");
  WorkspaceShellTestAccess::RefreshProjectSearch(shell);
  WaitForProjectSearch(shell);
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 1,
         "an exclude scope should subtract from the include set");
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell)[0].relative_path ==
             std::filesystem::path("src/main.cpp"),
         "only the file passing both filters should remain");

  WorkspaceShellTestAccess::SetProjectSearchScopeGlobs(shell, "", "");
  WorkspaceShellTestAccess::RefreshProjectSearch(shell);
  WaitForProjectSearch(shell);
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 3,
         "clearing the scope should restore the full result set");
}

// Replace-all falls back to the whole indexed catalog when the cached results are
// not authoritative. That fallback must still honor the scope, or replace-all would
// rewrite exactly the files the user excluded.
void TestWorkspaceShellProjectSearchReplaceAllHonorsScopeGlobs() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "src" / "main.cpp", "alpha\n");
  WriteFile(root / "vendor" / "dep.cpp", "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alpha", false);
  WaitForProjectSearch(shell);

  WorkspaceShellTestAccess::SetProjectSearchScopeGlobs(shell, "", "vendor");
  WorkspaceShellTestAccess::RefreshProjectSearch(shell);
  WaitForProjectSearch(shell);
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 1,
         "the excluded file should not appear in results");

  WorkspaceShellTestAccess::SetProjectSearchReplaceText(shell, "beta");
  WorkspaceShellTestAccess::ReplaceAllProjectSearchMatches(shell);
  const bool replaced = WaitUntil(
      [&root]() { return ReadFile(root / "src" / "main.cpp") == "beta\n"; },
      std::chrono::seconds(2), std::chrono::milliseconds(5));
  Expect(replaced, "replace-all should rewrite the in-scope file");
  Expect(ReadFile(root / "vendor" / "dep.cpp") == "alpha\n",
         "replace-all must not rewrite a file the scope excluded");
}

// Ctrl+Shift+F on a visible sidebar switches it to the search view for good:
// opening a result keeps the results on screen (VS Code). It used to be a
// "temporary" peek that restored the tree on the first open, so the result list
// was gone after one hit. Over a hidden sidebar it is still a peek (Escape closes).
void TestWorkspaceShellSearchViewStaysAfterOpeningAResult() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "a.txt", "alpha\nalpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowTreeSidebar(shell);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alpha", /*temporary=*/true);
  WaitForProjectSearch(shell);
  WorkspaceShellTestAccess::SetFocusSidebar(shell);
  (void)shell.HandleEvent(KeyDownEvent(SDLK_RETURN));
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == microide::workspace::SidebarMode::Search,
         "opening a result should keep the search view the user switched to");

}

// The search view searches as you type, after a short pause (VS Code's
// search.searchOnType): typing a query and waiting showed nothing until Enter.
void TestWorkspaceShellProjectSearchSearchesOnType() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "a.txt", "alpha\nbeta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "", false);
  WorkspaceShellTestAccess::SetFocusSidebar(shell);
  (void)shell.HandleEvent(KeyDownEvent(SDLK_SLASH));  // edit the query field
  (void)shell.HandleEvent(TextInputEvent("beta"));
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).empty(),
         "nothing should run while the typing has not paused");
  const bool searched = WaitUntil(
      [&]() {
        (void)shell.HandleScheduledWake();
        WorkspaceShellTestAccess::ConsumeProjectSearchUpdates(shell);
        return WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 1;
      },
      std::chrono::seconds(3), std::chrono::milliseconds(20));
  Expect(searched, "a pause after typing should run the search on the typed text");
  bool still_editing = false;
  const util::JsonValue ui = WorkspaceShellTestAccess::ControlUi(shell);
  for (const util::JsonValue& element : ui["elements"].AsArray()) {
    if (element["name"].AsString() == "field:search/query") {
      still_editing = element["active"].AsBool() && element["label"].AsString() == "beta";
    }
  }
  Expect(still_editing, "and the query field stays open for more typing");
}

// Replace All rewrites files the user has not opened, and an empty replacement
// deletes every match, so it confirms first with the counts (VS Code's "Replace
// N occurrences across M files"). Shift+R in the results list used to run it on
// the spot. The button beside the replace field and Ctrl+Alt+Enter in it ask too.
void TestWorkspaceShellProjectSearchReplaceAllConfirmsFirst() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "a.txt", "alpha alpha\n");
  WriteFile(root / "b.txt", "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alpha", false);
  WaitForProjectSearch(shell);
  WorkspaceShellTestAccess::SetProjectSearchReplaceText(shell, "");
  WorkspaceShellTestAccess::SetFocusSidebar(shell);

  (void)shell.HandleEvent(KeyDownEvent(SDLK_R, SDL_KMOD_SHIFT));
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "Shift+R should ask before replacing every match");
  const util::JsonValue ui = WorkspaceShellTestAccess::ControlUi(shell);
  Expect(ui["prompt"]["message"].AsString() ==
             "Replace 3 occurrences across 2 files with nothing (deleting them)?",
         "the confirm should give the counts and the replacement: " + ui["prompt"]["message"].AsString());
  (void)shell.HandleEvent(KeyDownEvent(SDLK_ESCAPE));
  Expect(!WorkspaceShellTestAccess::PromptSurfaceVisible(shell), "Escape should cancel the confirm");
  Expect(ReadFile(root / "a.txt") == "alpha alpha\n" && ReadFile(root / "b.txt") == "alpha\n",
         "cancelling must leave every file alone");

  WorkspaceShellTestAccess::SetProjectSearchReplaceText(shell, "beta");
  WorkspaceShellTestAccess::RequestProjectSearchReplaceAll(shell);
  Expect(WorkspaceShellTestAccess::ControlUi(shell)["prompt"]["message"].AsString() ==
             "Replace 3 occurrences across 2 files with \"beta\"?",
         "the confirm should name the replacement");
  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);
  const bool replaced = WaitUntil(
      [&root]() { return ReadFile(root / "a.txt") == "beta beta\n" && ReadFile(root / "b.txt") == "beta\n"; },
      std::chrono::seconds(2), std::chrono::milliseconds(5));
  Expect(replaced, "confirming should replace every match");
}

// TD-2026-07-30-001: the search panel's three option toggles were reachable only
// from the mouse. Alt+R / Alt+C / Alt+H are VSCode's search-box chords; they must
// work from the results list AND from inside the query field, and a toggle
// pressed mid-typing must re-run against the text on screen rather than the last
// committed query (that is what the flush-without-committing step buys).
void TestWorkspaceShellProjectSearchAltChordsToggleOptions() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "visible.txt", "alp.a\nALPHA\n");
  WriteFile(root / ".hidden.txt", "alp.a\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alp.a", false);
  WaitForProjectSearch(shell);
  Expect(!WorkspaceShellTestAccess::ProjectSearchEditing(shell),
         "opening with a query should land on the results list, not the editor");

  using microide::project::ProjectSearchCaseMode;
  using microide::project::ProjectSearchPatternMode;
  const auto& options = WorkspaceShellTestAccess::ProjectSearchOptions(shell);
  Expect(options.pattern_mode == ProjectSearchPatternMode::Literal &&
             options.case_mode == ProjectSearchCaseMode::Smart && !options.show_hidden,
         "the panel should start literal / smart-case / hidden-excluded");

  Expect(SendKeyDown(shell, SDLK_R, SDL_KMOD_ALT), "Alt+R should be handled by the panel");
  WaitForProjectSearch(shell);
  Expect(options.pattern_mode == ProjectSearchPatternMode::Regex,
         "Alt+R should switch the panel into regex mode");

  Expect(SendKeyDown(shell, SDLK_C, SDL_KMOD_ALT), "Alt+C should be handled by the panel");
  WaitForProjectSearch(shell);
  Expect(options.case_mode == ProjectSearchCaseMode::Sensitive,
         "Alt+C should cycle smart case to case-sensitive");

  Expect(SendKeyDown(shell, SDLK_H, SDL_KMOD_ALT), "Alt+H should be handled by the panel");
  WaitForProjectSearch(shell);
  Expect(options.show_hidden, "Alt+H should include hidden files");
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 2,
         "including hidden files should rerun and surface the dotfile match");

  // Now from inside the query field: type a new query, then chord without
  // committing. The rerun must see the typed text, and the caret must stay in
  // the field rather than being kicked back to the results list.
  Expect(SendKeyDown(shell, SDLK_SLASH, SDL_KMOD_NONE), "'/' should focus the query field");
  Expect(WorkspaceShellTestAccess::ProjectSearchEditing(shell),
         "'/' should begin editing the query");
  Expect(SendKeyDown(shell, SDLK_A, SDL_KMOD_CTRL), "Ctrl+A should select the field text");
  SDL_Event text_event{};
  text_event.type = SDL_EVENT_TEXT_INPUT;
  const std::string typed = "ALPHA";
  text_event.text.text = typed.c_str();
  Expect(shell.HandleEvent(text_event).handled, "typing into the query field should be handled");

  Expect(SendKeyDown(shell, SDLK_C, SDL_KMOD_ALT),
         "Alt+C should be handled while the query field has focus");
  WaitForProjectSearch(shell);
  Expect(WorkspaceShellTestAccess::ProjectSearchEditing(shell),
         "an option chord must not close the field editor");
  Expect(options.case_mode == ProjectSearchCaseMode::Insensitive,
         "Alt+C should cycle case-sensitive to case-insensitive");
  Expect(WorkspaceShellTestAccess::ProjectSearchQuery(shell) == typed,
         "the chord must flush the in-flight query before rerunning");
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).size() == 1,
         "the rerun should match the just-typed query, not the previous one");
}

// Escape must peel one layer at a time. "Close a temporary sidebar on Escape"
// used to be duplicated in the surface-navigation fallback (Search only, and it
// ran first) and in the per-mode sidebar handler, so Escape while typing a query
// tore the whole panel down and the search field's own Escape was dead code.
void TestWorkspaceShellSearchSidebarEscapeCancelsEditBeforeClosing() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "main.cpp", "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  // A peek: the search panel opens over a HIDDEN sidebar (on a visible one,
  // Ctrl+Shift+F switches the view for good, as VS Code does).
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "sidebar-hide"), "sidebar-hide should run");
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "", /*temporary=*/true);
  Expect(WorkspaceShellTestAccess::SidebarVisible(shell), "the search sidebar should be open");
  Expect(WorkspaceShellTestAccess::ProjectSearchEditing(shell),
         "an empty query should open straight into the field editor");

  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE), "Escape should be handled");
  Expect(!WorkspaceShellTestAccess::ProjectSearchEditing(shell),
         "the first Escape should cancel the field edit");
  Expect(WorkspaceShellTestAccess::SidebarVisible(shell),
         "the first Escape should leave the panel open");

  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE), "the second Escape should be handled");
  Expect(!WorkspaceShellTestAccess::SidebarVisible(shell),
         "the second Escape should dismiss the temporary search panel");
}

// The file tree and the git sidebar have had row context menus from the start;
// the search results list swallowed the right button entirely, so the path of a
// hit could only be copied by opening the file and using the tab menu.
void TestWorkspaceShellSearchResultRightClickOpensRowMenu() {
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
         "the fixture should produce one hit");

  const SDL_FRect result_rect = WorkspaceShellTestAccess::ProjectSearchResultRect(shell, 0);
  Expect(SendMouseDown(shell, result_rect.x + result_rect.w * 0.5f,
                       result_rect.y + result_rect.h * 0.5f, SDL_BUTTON_RIGHT),
         "right-clicking a search hit should be handled");
  Expect(WorkspaceShellTestAccess::TreeContextMenuTarget(shell) ==
             microide::workspace::TreeContextTargetKind::ResultRow,
         "right-clicking a search hit should open the shared row menu");
  Expect(WorkspaceShellTestAccess::TreeContextMenuPath(shell).lexically_normal() ==
             source.lexically_normal(),
         "the menu should target the hit's file, not the tree selection");
  // Right-click must not also open the file — that is the left button's job.
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() != source.lexically_normal(),
         "right-clicking a hit should not open it");

  const auto labels = WorkspaceShellTestAccess::TreeContextMenuLabels(
      shell, microide::workspace::TreeContextTargetKind::ResultRow);
  Expect(std::find(labels.begin(), labels.end(), "Copy Relative Path") != labels.end() &&
             std::find(labels.begin(), labels.end(), "Reveal in File Tree") != labels.end(),
         "the row menu should offer the path-scoped items");

  // Reveal must act on the row the menu named. Before this it read the active tab
  // path, so it either did nothing or revealed an unrelated file.
  Expect(WorkspaceShellTestAccess::ExecuteContextMenuAction(
             shell, WorkspaceShell::ActionId::RevealInFileTree),
         "Reveal in File Tree should run from the row menu");
  Expect(WorkspaceShellTestAccess::SelectedTreePath(shell) == source.lexically_normal(),
         "Reveal should select the hit's file in the tree");
}

// The context menu was the last list in the shell that answered Up/Down and
// nothing else. Home/End cannot go through ListNavigationKeyDelta here — that
// resolver expresses them as a ±count delta, which lands on the ends only for a
// mover that clamps, and this one wraps.
void TestWorkspaceShellContextMenuAnswersHomeAndEnd() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "src" / "alpha.cpp";
  WriteFile(source, "int alpha() { return 0; }\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alpha", false);
  WaitForProjectSearch(shell);

  const SDL_FRect result_rect = WorkspaceShellTestAccess::ProjectSearchResultRect(shell, 0);
  Expect(SendMouseDown(shell, result_rect.x + result_rect.w * 0.5f,
                       result_rect.y + result_rect.h * 0.5f, SDL_BUTTON_RIGHT),
         "right-clicking a search hit should open the row menu");
  const std::size_t item_count = WorkspaceShellTestAccess::TreeContextMenuItemCount(
      microide::workspace::TreeContextTargetKind::ResultRow);
  Expect(item_count > 2, "the row menu should have several items");

  Expect(SendKeyDown(shell, SDLK_END, SDL_KMOD_NONE), "End should be consumed by the menu");
  const int last = WorkspaceShellTestAccess::TreeContextMenuActiveItemIndex(shell);
  Expect(last == static_cast<int>(item_count) - 1,
         "End should activate the last item in the menu");

  Expect(SendKeyDown(shell, SDLK_HOME, SDL_KMOD_NONE), "Home should be consumed by the menu");
  Expect(WorkspaceShellTestAccess::TreeContextMenuActiveItemIndex(shell) == 0,
         "Home should activate the first item in the menu");

  // Down from the first item still walks one step, so Home/End did not replace
  // the existing contract.
  Expect(SendKeyDown(shell, SDLK_DOWN, SDL_KMOD_NONE), "Down should be consumed by the menu");
  Expect(WorkspaceShellTestAccess::TreeContextMenuActiveItemIndex(shell) > 0,
         "Down should still advance one item from the top");
}


// F3 / Shift+F3 step through the current find term from the editor, whether the
// widget is open or closed (VS Code nextMatchFindAction); with no term yet F3
// opens the widget. Neither key did anything before.
// The find widget selects its current match (VS Code's find), so with the widget
// up the editor's selection is the widget's own answer. Ctrl+H from the find field
// re-seeded the query from it, turning a regex query into the literal text of its
// match (QA find-options: `id=(\d+)` became `id=12`, and `$1` replaced nothing).
void TestWorkspaceShellCtrlHInTheOpenWidgetKeepsTheQuery() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "ids.txt";
  WriteFile(source, "id=12 id=345\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL), "Ctrl+F opens find");
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "id=");
  const auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(viewport.SelectedText() == "id=", "the widget selects its current match");
  Expect(SendKeyDown(shell, SDLK_H, SDL_KMOD_CTRL), "Ctrl+H from the find field");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 2,
         "the query is still the user's, matching both ids");
}

void TestWorkspaceShellF3StepsMatchesWithTheWidgetClosed() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  const std::filesystem::path source = root / "words.txt";
  WriteFile(source, "foo one\nfoo two\nfoo three\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);

  Expect(SendKeyDown(shell, SDLK_F3, SDL_KMOD_NONE), "F3 with no term is handled");
  Expect(WorkspaceShellTestAccess::OverlayVisible(shell), "F3 with no term opens the find widget");
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "foo");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 3, "three matches");
  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE), "Esc closes the widget");
  Expect(!WorkspaceShellTestAccess::OverlayVisible(shell), "the widget is closed");

  // Revealing a match selects it, as VS Code's find does: the selection starts on
  // the match. Match 0 is at 0:0, so F3 steps to the next one.
  const auto caret_at = [&](std::size_t line, std::size_t column) {
    const auto selection = viewport.selection_range();
    return selection.has_value() ? selection->start.line == line && selection->start.column == column &&
                                       selection->end.line == line && selection->end.column == column + 3
                                 : viewport.cursor_line() == line && viewport.cursor_column() == column;
  };
  Expect(caret_at(0, 0), "the caret starts on the first match");
  Expect(SendKeyDown(shell, SDLK_F3, SDL_KMOD_NONE), "F3 with the widget closed is handled");
  Expect(caret_at(1, 0), "F3 jumps to the next match (line 1)");
  Expect(!WorkspaceShellTestAccess::OverlayVisible(shell), "F3 does not reopen the widget");
  Expect(SendKeyDown(shell, SDLK_F3, SDL_KMOD_NONE), "a second F3 is handled");
  Expect(caret_at(2, 0), "a second F3 jumps to the match on line 2");
  Expect(SendKeyDown(shell, SDLK_F3, SDL_KMOD_SHIFT), "Shift+F3 is handled");
  Expect(caret_at(1, 0), "Shift+F3 steps back to line 1");
  Expect(SendKeyDown(shell, SDLK_F3, SDL_KMOD_NONE) && SendKeyDown(shell, SDLK_F3, SDL_KMOD_NONE),
         "two more F3 presses are handled");
  Expect(caret_at(0, 0), "F3 wraps from the last match to the first");

  // A caret parked between matches: F3 goes to the first match after it, not past it,
  // and Shift+F3 to the one before it.
  viewport.MoveCursorTo(1, 5);
  Expect(SendKeyDown(shell, SDLK_F3, SDL_KMOD_NONE), "F3 from between matches is handled");
  Expect(caret_at(2, 0), "F3 from mid-line 1 jumps to the match on line 2, not line 0");
  viewport.MoveCursorTo(1, 5);
  Expect(SendKeyDown(shell, SDLK_F3, SDL_KMOD_SHIFT), "Shift+F3 from between matches is handled");
  Expect(caret_at(1, 0), "Shift+F3 from mid-line 1 jumps back to the match on line 1");
}

// The find widget's match set is a list of LINE:COLUMN ranges into the buffer, so
// an edit under an open widget leaves every one of them describing text that has
// moved. Replace then rewrote whichever line had inherited the old coordinates
// and left the real match alone -- silent data loss, in the one verb whose whole
// job is to change text the user pointed at.
void TestWorkspaceShellFindMatchesFollowAnEditUnderTheWidget() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const auto file = root / "words.txt";
  WriteFile(file, "alpha\nbravo\ntarget\ncharlie\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, file);
  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  const auto joined = [&viewport]() {
    std::string out;
    for (std::size_t i = 0; i < viewport.line_count(); ++i) {
      out += viewport.lines().LineView(i);
      out += '\n';
    }
    return out;
  };

  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "target");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 1,
         "one match for `target`");
  WorkspaceShellTestAccess::SetBufferReplaceText(shell, "REPLACED");

  // An edit that does NOT go through the widget: everything below moves down one
  // line, so the stored match now points at `bravo`.
  viewport.MoveCursorTo(0, 0);
  viewport.InsertText("inserted\n");
  Expect(std::string(viewport.lines().LineView(3)) == "target",
         "`target` sits on line 3 after the insert, got " +
             std::string(viewport.lines().LineView(3)));

  WorkspaceShellTestAccess::ReplaceCurrentBufferSearchMatch(shell);
  Expect(joined() == "inserted\nalpha\nbravo\nREPLACED\ncharlie\n\n",
         "Replace must rewrite the match, not the line that inherited its "
         "coordinates: " +
             joined());
}

// The other half: an edit that DOES announce itself keeps the match set current,
// so the highlights and the n-of-m counter describe the buffer on screen rather
// than the one it was two keystrokes ago. Checked through the shell's own edit
// path (a typed character), not by calling the refresh.
void TestWorkspaceShellFindMatchCountFollowsAnAnnouncedEdit() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const auto file = root / "words.txt";
  WriteFile(file, "one target\ntwo target\nthree\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, file);

  // Through the real verb, because the refresh-on-edit is gated on the widget
  // being open -- which is also exactly what gates the highlight paint and the
  // n-of-m counter, so a match set nobody can see is not worth a scan.
  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_F, SDL_KMOD_CTRL),
         "Ctrl+F opens the find widget");
  Expect(WorkspaceShellTestAccess::ActiveOverlayMode(shell) ==
             WorkspaceShell::OverlayMode::BufferSearch,
         "the in-file find surface is open");
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "target");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 2,
         "two matches to start with");

  // Duplicate line 0 through the action layer: a third `target` appears, and the
  // widget must say so without anyone re-typing the query.
  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  viewport.MoveCursorTo(0, 0);
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::DuplicateLine, {}),
         "duplicate-line dispatches");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 3,
         "the widget should count the new occurrence, got " +
             std::to_string(WorkspaceShellTestAccess::BufferSearchMatchCount(shell)));

  // And it must not have yanked the caret onto a match on the way: the edit's own
  // caret is where the user left it.
  Expect(viewport.cursor_line() == 1,
         "duplicate-line leaves the caret on the copy, not on a search hit; got line " +
             std::to_string(viewport.cursor_line()));
}

// Workbench shortcuts work from wherever focus is, as in VS Code. Find in files,
// close/next tab and the rest were registered for the EDITOR context, so with the
// tree focused Ctrl+Shift+F did nothing -- and the query the user then typed went
// to the tree as single-key commands. A focused terminal still keeps Ctrl+W for
// its shell.
void TestWorkspaceShellWorkbenchShortcutsWorkFromTheTree() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "a.txt", "alpha\n");
  WriteFile(root / "b.txt", "beta\n");
  WriteFile(root / "c.txt", "gamma\n");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "the shortcut fixture should open its project");
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  for (const char* name : {"a.txt", "b.txt", "c.txt"}) {
    Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, root / name), "the fixture opens its files");
  }
  const std::size_t tabs = WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell);

  WorkspaceShellTestAccess::ShowTreeSidebar(shell);
  WorkspaceShellTestAccess::SetFocusSidebar(shell);
  const std::size_t active = WorkspaceShellTestAccess::ActiveTabIndex(shell);
  Expect(SendKeyDown(shell, SDLK_PAGEDOWN, SDL_KMOD_CTRL), "Ctrl+PageDown is handled from the tree");
  Expect(WorkspaceShellTestAccess::ActiveTabIndex(shell) == (active + 1) % tabs,
         "Ctrl+PageDown from the tree moves to the next tab");

  WorkspaceShellTestAccess::SetFocusSidebar(shell);
  Expect(SendKeyDown(shell, SDLK_W, SDL_KMOD_CTRL), "Ctrl+W is handled from the tree");
  Expect(WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell) == tabs - 1,
         "Ctrl+W from the tree closes the active tab");

  WorkspaceShellTestAccess::SetFocusSidebar(shell);
  Expect(SendKeyDown(shell, SDLK_F, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT)),
         "Ctrl+Shift+F is handled from the tree");
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == WorkspaceShell::SidebarMode::Search,
         "Ctrl+Shift+F from the tree opens Search");

  // Ctrl+K Ctrl+<arrow> (focus the editor pane that way) is a workbench chord too.
  Expect(WorkspaceShellTestAccess::SplitEditorGroup(
             shell, microide::workspace::EditorSplitOrientation::Vertical),
         "the fixture splits the editor");
  if (WorkspaceShellTestAccess::FocusedGroupIndex(shell) != 0) {
    WorkspaceShellTestAccess::FocusOtherEditorGroup(shell);
  }
  // Toward the second pane, whichever way the split laid it out.
  const auto panes = WorkspaceShellTestAccess::EditorGroupRectsFor(
      shell, WorkspaceShellTestAccess::CurrentLayout(shell));
  Expect(panes.groups.size() == 2, "the split has two panes");
  const SDL_Keycode toward_second =
      panes.groups[1].editor_surface.x > panes.groups[0].editor_surface.x ? SDLK_RIGHT : SDLK_DOWN;
  WorkspaceShellTestAccess::SetFocusSidebar(shell);
  Expect(SendKeyDown(shell, SDLK_K, SDL_KMOD_CTRL), "the Ctrl+K leader is taken from the tree");
  Expect(SendKeyDown(shell, toward_second, SDL_KMOD_CTRL), "Ctrl+K Ctrl+<arrow> completes the chord");
  Expect(WorkspaceShellTestAccess::FocusedGroupIndex(shell) == 1,
         "Ctrl+K Ctrl+<arrow> from the tree focuses the pane that way");

  // A focused terminal owns Ctrl+W (the shell's delete-word).
  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::SetFocusPanel(shell);
  const std::size_t before_terminal = WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell);
  (void)SendKeyDown(shell, SDLK_W, SDL_KMOD_CTRL);
  Expect(WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell) == before_terminal,
         "Ctrl+W in a focused terminal goes to the shell, not to the editor tabs");
}

void RegisterWorkspaceShellSearchTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/SearchFieldShortcutsDoNotTypeThemselves",
          TestWorkspaceShellSearchFieldShortcutsDoNotTypeThemselves);
  AddTest(tests, "WorkspaceShell/WorkbenchShortcutsWorkFromTheTree",
          TestWorkspaceShellWorkbenchShortcutsWorkFromTheTree);
  AddTest(tests, "WorkspaceShell/FindMatchesFollowAnEditUnderTheWidget",
          TestWorkspaceShellFindMatchesFollowAnEditUnderTheWidget);
  AddTest(tests, "WorkspaceShell/FindMatchCountFollowsAnAnnouncedEdit",
          TestWorkspaceShellFindMatchCountFollowsAnAnnouncedEdit);
  AddTest(tests, "WorkspaceShell/F3StepsMatchesWithTheWidgetClosed",
          TestWorkspaceShellF3StepsMatchesWithTheWidgetClosed);
  AddTest(tests, "WorkspaceShell/CtrlHInTheOpenWidgetKeepsTheQuery", TestWorkspaceShellCtrlHInTheOpenWidgetKeepsTheQuery);
  AddTest(tests, "WorkspaceShell/SearchSidebarEscapeCancelsEditBeforeClosing",
          TestWorkspaceShellSearchSidebarEscapeCancelsEditBeforeClosing);
  AddTest(tests, "WorkspaceShell/ContextMenuAnswersHomeAndEnd",
          TestWorkspaceShellContextMenuAnswersHomeAndEnd);
  AddTest(tests, "WorkspaceShell/SearchResultRightClickOpensRowMenu",
          TestWorkspaceShellSearchResultRightClickOpensRowMenu);
  AddTest(tests, "WorkspaceShell/BufferRegexReplaceAll",
          TestWorkspaceShellBufferRegexReplaceAll);
  AddTest(tests, "WorkspaceShell/BufferRegexReplaceCurrent",
          TestWorkspaceShellBufferRegexReplaceCurrent);
  AddTest(tests, "WorkspaceShell/BufferRegexMultilineNewline",
          TestWorkspaceShellBufferRegexMultilineNewline);
  AddTest(tests, "WorkspaceShell/BufferRegexToggleViaAltR",
          TestWorkspaceShellBufferRegexToggleViaAltR);
  AddTest(tests, "WorkspaceShell/SingleLineTextSurfacePredicateCoversEveryField",
          TestWorkspaceShellSingleLineTextSurfacePredicateCoversEveryField);
  AddTest(tests, "WorkspaceShell/BufferFindMatchCaseAppliesToBothModes",
          TestWorkspaceShellBufferFindMatchCaseAppliesToBothModes);
  AddTest(tests, "WorkspaceShell/BufferFindWholeWordFiltersMatches",
          TestWorkspaceShellBufferFindWholeWordFiltersMatches);
  AddTest(tests, "WorkspaceShell/BufferFindReplaceAllHonoursOptions",
          TestWorkspaceShellBufferFindReplaceAllHonoursOptions);
  AddTest(tests, "WorkspaceShell/BufferFindInSelectionScopesMatchesAndReplace",
          TestWorkspaceShellBufferFindInSelectionScopesMatchesAndReplace);
  AddTest(tests, "BufferFindScope/FollowsEdits", TestBufferFindScopeFollowsEdits);
  AddTest(tests, "WorkspaceShell/BufferFindInSelectionRegexReplace",
          TestWorkspaceShellBufferFindInSelectionRegexReplace);
  AddTest(tests, "WorkspaceShell/ProjectSearchSidebarScrollPastSelection",
          TestWorkspaceShellProjectSearchSidebarScrollPastSelection);
  AddTest(tests, "WorkspaceShell/ProjectSearchAltChordsToggleOptions",
          TestWorkspaceShellProjectSearchAltChordsToggleOptions);
  AddTest(tests, "WorkspaceShell/ProjectSearchHiddenToggleReruns",
          TestWorkspaceShellProjectSearchHiddenToggleReruns);
  AddTest(tests, "WorkspaceShell/ProjectSearchPatternModeToggleReruns",
          TestWorkspaceShellProjectSearchPatternModeToggleReruns);
  AddTest(tests, "WorkspaceShell/ProjectSearchCaseModeCycleReruns",
          TestWorkspaceShellProjectSearchCaseModeCycleReruns);
  AddTest(tests, "WorkspaceShell/ProjectSearchSurfacesIncompleteIndex",
          TestWorkspaceShellProjectSearchSurfacesIncompleteIndex);
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
  AddTest(tests, "WorkspaceShell/ProjectSearchScopeToggleRevealsGlobFields",
          TestWorkspaceShellProjectSearchScopeToggleRevealsGlobFields);
  AddTest(tests, "WorkspaceShell/ProjectSearchScopeGlobsRerunAndRestrictResults",
          TestWorkspaceShellProjectSearchScopeGlobsRerunAndRestrictResults);
  AddTest(tests, "WorkspaceShell/ProjectSearchSearchesOnType", TestWorkspaceShellProjectSearchSearchesOnType);
  AddTest(tests, "WorkspaceShell/SearchViewStaysAfterOpeningAResult",
          TestWorkspaceShellSearchViewStaysAfterOpeningAResult);
  AddTest(tests, "WorkspaceShell/ProjectSearchReplaceAllConfirmsFirst",
          TestWorkspaceShellProjectSearchReplaceAllConfirmsFirst);
  AddTest(tests, "WorkspaceShell/ProjectSearchReplaceAllHonorsScopeGlobs",
          TestWorkspaceShellProjectSearchReplaceAllHonorsScopeGlobs);
  AddTest(tests, "WorkspaceShell/BufferSearchIsNonModal", TestWorkspaceShellBufferSearchIsNonModal);
  AddTest(tests, "WorkspaceShell/CutInSingleLineSurfaceDoesNotEditEditor",
          TestWorkspaceShellCutInSingleLineSurfaceDoesNotEditEditor);
  AddTest(tests, "WorkspaceShell/BufferSearchEnterCyclesMatches",
          TestWorkspaceShellBufferSearchEnterCyclesMatches);
  AddTest(tests, "WorkspaceShell/BufferSearchReopenKeepsQueryAndRefocuses",
          TestWorkspaceShellBufferSearchReopenKeepsQueryAndRefocuses);
  AddTest(tests, "WorkspaceShell/BufferSearchSeedsFromSelection",
          TestWorkspaceShellBufferSearchSeedsFromSelection);
  AddTest(tests, "WorkspaceShell/BufferSearchStartsFromTheCaret",
          TestWorkspaceShellBufferSearchStartsFromTheCaret);
  AddTest(tests, "WorkspaceShell/BufferReplaceMovesToTheNextMatch",
          TestWorkspaceShellBufferReplaceMovesToTheNextMatch);
  AddTest(tests, "WorkspaceShell/BufferReplaceEnterInFindFieldStepsMatches",
          TestWorkspaceShellBufferReplaceEnterInFindFieldStepsMatches);
}

}  // namespace microide::tests
