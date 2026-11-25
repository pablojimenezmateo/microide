#include "TestSupport.h"
#include "support/ProjectSearchWait.h"
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

void TestWorkspaceShellSearchResultsTypeToNavigate() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "visible.txt", "alpha one\n  beta alpha\ncharlie alpha\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alpha", false);
  WaitForProjectSearch(shell);
  WorkspaceShellTestAccess::SetFocusSidebar(shell);
  if (WorkspaceShellTestAccess::ProjectSearchEditing(shell)) {
    SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE);
  }
  Expect(!WorkspaceShellTestAccess::ProjectSearchEditing(shell), "the list, not a field, has the keys");
  WorkspaceShellTestAccess::SetProjectSearchSelectedIndex(shell, 0);

  std::uint64_t now_ns = 10'000'000'000;
  const auto type = [&](SDL_Keycode key) {
    SDL_Event event = KeyDownEvent(key, SDL_KMOD_NONE);
    event.key.timestamp = now_ns;
    now_ns += 2'000'000'000;
    return shell.HandleEvent(event).handled;
  };
  Expect(type(SDLK_C), "a letter in the results list is handled");
  Expect(WorkspaceShellTestAccess::ProjectSearchSelectedIndex(shell) == 2,
         "typing c selects the result whose line starts with charlie");
  Expect(!WorkspaceShellTestAccess::ProjectSearchEditing(shell), "typing opens no field");
  Expect(type(SDLK_B), "a letter in the results list is handled");
  Expect(WorkspaceShellTestAccess::ProjectSearchSelectedIndex(shell) == 1,
         "typing b (a fresh prefix after the pause) selects the indented beta line");
  for (const SDL_Keycode key : {SDLK_I, SDLK_X, SDLK_EQUALS, SDLK_SLASH}) {
    (void)type(key);
    Expect(!WorkspaceShellTestAccess::ProjectSearchEditing(shell), "a field shortcut letter opens nothing now");
  }

  Expect(!WorkspaceShellTestAccess::ProjectSearchScopeExpanded(shell), "the details start hidden");
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "search-toggle-details"),
         "Toggle Search Details should run");
  Expect(WorkspaceShellTestAccess::ProjectSearchScopeExpanded(shell),
         "Toggle Search Details shows the include/exclude fields");
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

void TestWorkspaceShellProjectSearchSurfacesIncompleteIndex() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "a.txt", "alpha\n");
  WriteFile(root / "b.txt", "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);

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

  WorkspaceShellTestAccess::ForceFileIndexRefreshAndDrain(shell);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alpha", false);
  WaitForProjectSearch(shell);
  Expect(!WorkspaceShellTestAccess::ProjectSearchIndexIncomplete(shell),
         "searching over a complete index clears the incomplete flag");
}

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

  const std::string query_before = WorkspaceShellTestAccess::BufferSearchQuery(shell);
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "Z"),
         "typing should be accepted by the focused editor");
  Expect(WorkspaceShellTestAccess::BufferSearchQuery(shell) == query_before,
         "typing while the editor is focused must not change the find query");
}

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

  SendKeyDown(shell, SDLK_A, SDL_KMOD_CTRL);
  Expect(WorkspaceShellTestAccess::BufferSearchSurfaceFocused(shell),
         "Ctrl+A in a focused single-line field must not move focus to the editor");

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
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE), "Enter on the last match should wrap");
  Expect(WorkspaceShellTestAccess::BufferSearchSelectedIndex(shell) == 0,
         "Enter past the last match should wrap to the first");
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

  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.MoveCursorTo(0, 0);
  editor.SelectWordAtCursor();

  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL), "Ctrl+F should open the find widget");
  Expect(WorkspaceShellTestAccess::BufferSearchQuery(shell) == "needle",
         "opening find with a selection should seed the query from it");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 2,
         "the seeded query should immediately find both occurrences");
}

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
  editor.MoveCursorTo(1, 2);
  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL), "Ctrl+F reopens the find widget");
  Expect(WorkspaceShellTestAccess::BufferSearchSelectedIndex(shell) == 2,
         "a match that starts before the caret is not current; the next one is");

  Expect(SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE), "Escape closes the widget");
  editor.MoveCursorTo(2, 3);
  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL), "Ctrl+F reopens the find widget");
  Expect(WorkspaceShellTestAccess::BufferSearchSelectedIndex(shell) == 0,
         "with no match at or after the caret the search wraps to the first");
}

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
  WorkspaceShellTestAccess::SetBufferSearchSelectedIndex(shell, 2);

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

void TestWorkspaceShellBufferReplaceStepsPastAMatchItWouldNotChange() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "Foo foo Foo foo\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.MoveCursorTo(0, 0);

  Expect(SendKeyDown(shell, SDLK_H, SDL_KMOD_CTRL), "Ctrl+H should open the replace widget");
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "foo");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 4,
         "four case-insensitive matches");
  Expect(WorkspaceShellTestAccess::BufferSearchSelectedIndex(shell) == 0, "the first is current");
  WorkspaceShellTestAccess::SetBufferReplaceText(shell, "Foo");

  WorkspaceShellTestAccess::ReplaceCurrentBufferSearchMatch(shell);
  Expect(editor.lines()[0] == "Foo foo Foo foo", "the first already read `Foo`: no edit");
  Expect(WorkspaceShellTestAccess::BufferSearchSelectedIndex(shell) == 1,
         "Replace still moves on to the next match");

  WorkspaceShellTestAccess::ReplaceCurrentBufferSearchMatch(shell);
  Expect(editor.lines()[0] == "Foo Foo Foo foo", "the second is replaced");
  WorkspaceShellTestAccess::ReplaceCurrentBufferSearchMatch(shell);
  WorkspaceShellTestAccess::ReplaceCurrentBufferSearchMatch(shell);
  Expect(editor.lines()[0] == "Foo Foo Foo Foo",
         "four presses reach the last occurrence: " + std::string(editor.lines()[0]));
}

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

}

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

  Expect(SendKeyDown(shell, SDLK_END, SDL_KMOD_NONE),
         "End should be handled by the focused search sidebar");
  Expect(WorkspaceShellTestAccess::ProjectSearchSelectedIndex(shell) == result_count - 1,
         "End should select the last result");
  Expect(WorkspaceShellTestAccess::SidebarScrollRow(shell) > 0,
         "navigating to the last result should reveal it (scroll follows the selection)");

  WorkspaceShellTestAccess::SetSidebarScrollRow(shell, 0);
  WorkspaceShellTestAccess::ConsumeProjectSearchUpdates(shell);
  Expect(WorkspaceShellTestAccess::SidebarScrollRow(shell) == 0,
         "scrolling away from the selected entry should be preserved");

  Expect(SendKeyDown(shell, SDLK_HOME, SDL_KMOD_NONE),
         "Home should be handled by the focused search sidebar");
  Expect(WorkspaceShellTestAccess::ProjectSearchSelectedIndex(shell) == 0,
         "Home should select the first result");
  Expect(WorkspaceShellTestAccess::SidebarScrollRow(shell) == 0,
         "navigating to the first result should reveal it at the top");
}

namespace {

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

}

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

  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "\\n");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 2,
         "regex `\\n` should find the two newlines (per-line search found none)");

  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "\\d\\n\\w");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 2,
         "a multi-line regex should match across line breaks");

  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "\\n");
  WorkspaceShellTestAccess::SetBufferReplaceText(shell, " | ");
  WorkspaceShellTestAccess::ReplaceAllBufferSearchMatches(shell);
  Expect(ActiveEditorDocText(shell) == "a=1 | b=2 | c=3",
         "replacing `\\n` should join the lines");
}

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

  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "[unterminated");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 0,
         "an invalid regex should produce zero matches without crashing");
}


void TestWorkspaceShellSingleLineTextSurfacePredicateCoversEveryField() {
  using microide::workspace::IsSingleLineTextInputSurface;
  using Surface = microide::workspace::TextInputSurface;

  const Surface single_line[] = {
      Surface::PromptInput,         Surface::FileFinder,
      Surface::BufferSearch,        Surface::BufferReplaceSearch,
      Surface::BufferReplaceReplace,
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

  const Surface not_single_line[] = {Surface::None, Surface::Editor, Surface::CommitBody,
                                     Surface::Terminal};
  for (const Surface surface : not_single_line) {
    Expect(!IsSingleLineTextInputSurface(surface),
           "multi-line surfaces must not be treated as single-line fields");
  }

  Expect(IsSingleLineTextInputSurface(Surface::CommitSubject) &&
             IsSingleLineTextInputSurface(Surface::TerminalFind),
         "the commit subject and the terminal find bar are single-line fields");
}

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

  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "cat_x");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 1,
         "extending the query under whole word must not inherit the prefix filter");
}

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
  viewport.JumpCursorTo(2, 5, true);

  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL), "Ctrl+F should open the find widget");
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "cat");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 5, "unscoped, all five cats match");
  Expect(SendKeyDown(shell, SDLK_L, SDL_KMOD_ALT), "Alt+L should toggle Find in Selection");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 3,
         "in the selection, only the three cats of lines 1-2 match");

  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "cat ");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 3,
         "a refined query keeps to the scope");
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "cat");

  viewport.JumpCursorTo(0, 0);
  viewport.InsertText("new\n");
  WorkspaceShellTestAccess::RefreshBufferSearchIfStale(shell);
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 3,
         "the scope follows its text down past an inserted line");

  WorkspaceShellTestAccess::SetBufferReplaceText(shell, "dog dog");
  WorkspaceShellTestAccess::ReplaceAllBufferSearchMatches(shell);
  Expect(ActiveEditorDocText(shell) == "new\ncat 0\ndog dog dog dog 1\ndog dog 2\ncat 3\n",
         "Replace All in the selection changes only the cats inside it: " + ActiveEditorDocText(shell));
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "dog");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 6,
         "the scope's end follows a Replace All inside it");

  Expect(SendKeyDown(shell, SDLK_L, SDL_KMOD_ALT), "Alt+L should toggle Find in Selection off");
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "cat");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 2,
         "with the toggle off the cats outside the old scope match again");
}

void TestBufferFindScopeFollowsEdits() {
  using microide::workspace::BufferSearchState;
  editor::TextViewport viewport;
  viewport.LoadContent("zero\none two\nthree\nfour\n", "/tmp/find-scope-fixture.txt");
  viewport.JumpCursorTo(1, 4);
  viewport.JumpCursorTo(2, 5, true);
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

  const std::optional<std::size_t> replaced =
      viewport.ReplaceAllRanges({{{2, 1}, {2, 2}}, {{3, 0}, {3, 1}}}, "long");
  Expect(replaced == std::optional<std::size_t>{2}, "two ranges replaced");
  Expect(!viewport.last_applied_edit().has_value(),
         "a multi-range edit leaves no single edit behind (it once left the previous one)");
  Expect(microide::workspace::FollowFindScope(scope, viewport), "same document");
  expect_range(2, 1, 3, 4, "the end follows the document's tail through a multi-range edit");

  Expect(viewport.ReplaceRange({{0, 0}, {0, 0}}, "a\nb\n"), "an insertion above");
  Expect(viewport.ReplaceRange({{0, 0}, {0, 0}}, "c\nd\n"), "and another, before the follow");
  Expect(microide::workspace::FollowFindScope(scope, viewport), "same document");
  Expect(scope.range.end.line == 7, "the end follows both insertions, not the last only (end line " +
                                        std::to_string(scope.range.end.line) + ")");

  editor::TextViewport other;
  other.LoadContent("other\n", "/tmp/find-scope-other.txt");
  Expect(!microide::workspace::FollowFindScope(scope, other), "another document drops the scope");
}

void TestWorkspaceShellProjectSearchHitsFollowEdits() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "alpha\nbeta alpha\n");
  WriteFile(root / "other.txt", "alpha\n");
  WriteFile(root / "zeta.txt", "nothing here\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alpha", false);
  WaitForProjectSearch(shell);
  const auto lines_of = [&](std::string_view file) {
    std::vector<std::size_t> lines;
    for (const auto& hit : WorkspaceShellTestAccess::ProjectSearchResults(shell)) {
      if (hit.relative_path_string == file) {
        lines.push_back(hit.line);
      }
    }
    return lines;
  };
  Expect(lines_of("notes.txt") == std::vector<std::size_t>{0, 1}, "the search finds both alphas in notes.txt");

  WorkspaceShellTestAccess::SetFocusEditor(shell);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 0);
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE), "Enter should insert a line above the hits");
  Expect(lines_of("notes.txt") == std::vector<std::size_t>{1, 2},
         "a line inserted above moves the hits down with their text");
  Expect(lines_of("other.txt") == std::vector<std::size_t>{0}, "another file's hits stay as they were");

  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 0);
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "alpha"), "typing should be handled");
  Expect(lines_of("notes.txt") == std::vector<std::size_t>{0, 1, 2}, "a match typed in adds a hit");

  WorkspaceShellTestAccess::OpenFile(shell, root / "zeta.txt");
  WorkspaceShellTestAccess::SetFocusEditor(shell);
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(0, 0);
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "alpha "), "typing should be handled");
  Expect(lines_of("zeta.txt") == std::vector<std::size_t>{0}, "a file gaining its first match gets a hit");
  Expect(WorkspaceShellTestAccess::ProjectSearchResults(shell).back().relative_path_string == "zeta.txt",
         "and it sits where the search orders it, after the files before it");

  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "beta", false);
  WaitForProjectSearch(shell);
  Expect(lines_of("notes.txt") == std::vector<std::size_t>{2},
         "the dirty buffer is searched as it stands (beta is on line 3 there, line 2 on disk)");
}

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
  viewport.JumpCursorTo(1, 2, true);
  WorkspaceShellTestAccess::ToggleBufferSearchOption(shell, microide::workspace::BufferFindToggle::InSelection);
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 2, "a2 and a3 are in the selection");
  WorkspaceShellTestAccess::SetBufferReplaceText(shell, "<$1>");
  WorkspaceShellTestAccess::ReplaceAllBufferSearchMatches(shell);
  Expect(ActiveEditorDocText(shell) == "a1 <2>\n<3> a4\na5\n",
         "a regex Replace All in the selection expands groups inside it only: " + ActiveEditorDocText(shell));

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

void TestWorkspaceShellSearchViewStaysAfterOpeningAResult() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "a.txt", "alpha\nalpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowTreeSidebar(shell);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "alpha", true);
  WaitForProjectSearch(shell);
  WorkspaceShellTestAccess::SetFocusSidebar(shell);
  (void)shell.HandleEvent(KeyDownEvent(SDLK_RETURN));
  Expect(WorkspaceShellTestAccess::SidebarMode(shell) == microide::workspace::SidebarMode::Search,
         "opening a result should keep the search view the user switched to");

}

void TestWorkspaceShellProjectSearchSearchesOnType() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "a.txt", "alpha\nbeta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "", false);
  WorkspaceShellTestAccess::SetFocusSidebar(shell);
  (void)shell.HandleEvent(KeyDownEvent(SDLK_SLASH));
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
  if (WorkspaceShellTestAccess::ProjectSearchEditing(shell)) {
    SendKeyDown(shell, SDLK_ESCAPE, SDL_KMOD_NONE);
  }

  (void)shell.HandleEvent(KeyDownEvent(SDLK_R, SDL_KMOD_SHIFT));
  Expect(!WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "Shift+R in the results list is typing, not Replace All");
  Expect(SendKeyDown(shell, SDLK_H, SDL_KMOD_CTRL | SDL_KMOD_SHIFT), "Ctrl+Shift+H should be handled");
  Expect(WorkspaceShellTestAccess::ProjectSearchEditing(shell) &&
             WorkspaceShellTestAccess::ProjectSearchEditFieldValue(shell) ==
                 microide::workspace::ProjectSearchEditField::Replace,
         "Replace in Files puts the keyboard in the replace field");
  (void)shell.HandleEvent(KeyDownEvent(SDLK_RETURN, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_ALT)));
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "Ctrl+Alt+Enter in the replace field should ask before replacing every match");
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

  Expect(SendKeyDown(shell, SDLK_F, SDL_KMOD_CTRL | SDL_KMOD_SHIFT), "Ctrl+Shift+F should focus the query field");
  Expect(WorkspaceShellTestAccess::ProjectSearchEditing(shell),
         "Ctrl+Shift+F should begin editing the query");
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

void TestWorkspaceShellSearchSidebarEscapeCancelsEditBeforeClosing() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "workspace";
  WriteFile(root / "main.cpp", "alpha\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "sidebar-hide"), "sidebar-hide should run");
  WorkspaceShellTestAccess::ShowSearchSidebar(shell, "", true);
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
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() != source.lexically_normal(),
         "right-clicking a hit should not open it");

  const auto labels = WorkspaceShellTestAccess::TreeContextMenuLabels(
      shell, microide::workspace::TreeContextTargetKind::ResultRow);
  Expect(std::find(labels.begin(), labels.end(), "Copy Relative Path") != labels.end() &&
             std::find(labels.begin(), labels.end(), "Reveal in File Tree") != labels.end(),
         "the row menu should offer the path-scoped items");

  Expect(WorkspaceShellTestAccess::ExecuteContextMenuAction(
             shell, WorkspaceShell::ActionId::RevealInFileTree),
         "Reveal in File Tree should run from the row menu");
  Expect(WorkspaceShellTestAccess::SelectedTreePath(shell) == source.lexically_normal(),
         "Reveal should select the hit's file in the tree");
}

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

  Expect(SendKeyDown(shell, SDLK_DOWN, SDL_KMOD_NONE), "Down should be consumed by the menu");
  Expect(WorkspaceShellTestAccess::TreeContextMenuActiveItemIndex(shell) > 0,
         "Down should still advance one item from the top");
}


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

  viewport.MoveCursorTo(1, 5);
  Expect(SendKeyDown(shell, SDLK_F3, SDL_KMOD_NONE), "F3 from between matches is handled");
  Expect(caret_at(2, 0), "F3 from mid-line 1 jumps to the match on line 2, not line 0");
  viewport.MoveCursorTo(1, 5);
  Expect(SendKeyDown(shell, SDLK_F3, SDL_KMOD_SHIFT), "Shift+F3 from between matches is handled");
  Expect(caret_at(1, 0), "Shift+F3 from mid-line 1 jumps back to the match on line 1");
}

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

void TestWorkspaceShellFindMatchCountFollowsAnAnnouncedEdit() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const auto file = root / "words.txt";
  WriteFile(file, "one target\ntwo target\nthree\n");
  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  WorkspaceShellTestAccess::OpenFile(shell, file);

  Expect(WorkspaceShellTestAccess::HandleKeyDown(shell, SDLK_F, SDL_KMOD_CTRL),
         "Ctrl+F opens the find widget");
  Expect(WorkspaceShellTestAccess::ActiveOverlayMode(shell) ==
             WorkspaceShell::OverlayMode::BufferSearch,
         "the in-file find surface is open");
  WorkspaceShellTestAccess::SetBufferSearchQueryAndRefresh(shell, "target");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 2,
         "two matches to start with");

  auto& viewport = WorkspaceShellTestAccess::ActiveEditor(shell);
  viewport.MoveCursorTo(0, 0);
  Expect(WorkspaceShellTestAccess::ExecuteAction(shell, WorkspaceShell::ActionId::DuplicateLine, {}),
         "duplicate-line dispatches");
  Expect(WorkspaceShellTestAccess::BufferSearchMatchCount(shell) == 3,
         "the widget should count the new occurrence, got " +
             std::to_string(WorkspaceShellTestAccess::BufferSearchMatchCount(shell)));

  Expect(viewport.cursor_line() == 1,
         "duplicate-line leaves the caret on the copy, not on a search hit; got line " +
             std::to_string(viewport.cursor_line()));
}

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

  Expect(WorkspaceShellTestAccess::SplitEditorGroup(
             shell, microide::workspace::EditorSplitOrientation::Vertical),
         "the fixture splits the editor");
  if (WorkspaceShellTestAccess::FocusedGroupIndex(shell) != 0) {
    WorkspaceShellTestAccess::FocusOtherEditorGroup(shell);
  }
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

  WorkspaceShellTestAccess::EnsureTerminalTab(shell);
  WorkspaceShellTestAccess::SetFocusPanel(shell);
  const std::size_t before_terminal = WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell);
  (void)SendKeyDown(shell, SDLK_W, SDL_KMOD_CTRL);
  Expect(WorkspaceShellTestAccess::FocusedGroupOpenTabCount(shell) == before_terminal,
         "Ctrl+W in a focused terminal goes to the shell, not to the editor tabs");
}

void RegisterWorkspaceShellSearchTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/SearchResultsTypeToNavigate",
          TestWorkspaceShellSearchResultsTypeToNavigate);
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
  AddTest(tests, "WorkspaceShell/ProjectSearchHitsFollowEdits", TestWorkspaceShellProjectSearchHitsFollowEdits);
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
  AddTest(tests, "WorkspaceShell/BufferReplaceStepsPastAMatchItWouldNotChange",
          TestWorkspaceShellBufferReplaceStepsPastAMatchItWouldNotChange);
  AddTest(tests, "WorkspaceShell/BufferReplaceEnterInFindFieldStepsMatches",
          TestWorkspaceShellBufferReplaceEnterInFindFieldStepsMatches);
}

}
