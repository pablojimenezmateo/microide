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
  std::string repeated_lines;
  for (int line = 0; line < 25; ++line) {
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

}  // namespace

void RegisterWorkspaceShellSearchTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/ProjectSearchHiddenToggleReruns",
          TestWorkspaceShellProjectSearchHiddenToggleReruns);
  AddTest(tests, "WorkspaceShell/ProjectSearchPatternModeToggleReruns",
          TestWorkspaceShellProjectSearchPatternModeToggleReruns);
  AddTest(tests, "WorkspaceShell/ProjectSearchCaseModeCycleReruns",
          TestWorkspaceShellProjectSearchCaseModeCycleReruns);
  AddTest(tests, "WorkspaceShell/ProjectSearchRerunClearsTruncation",
          TestWorkspaceShellProjectSearchRerunClearsTruncation);
  AddTest(tests, "WorkspaceShell/ProjectSearchStreamsWhileRunning",
          TestWorkspaceShellProjectSearchStreamsWhileRunning);
  AddTest(tests, "WorkspaceShell/ProjectSearchSidebarClickOpensResult",
          TestWorkspaceShellProjectSearchSidebarClickOpensResult);
  AddTest(tests, "WorkspaceShell/ProjectSearchSidebarClickMovesToCorrectLine",
          TestWorkspaceShellProjectSearchSidebarClickMovesToCorrectLine);
}

}  // namespace microide::tests
