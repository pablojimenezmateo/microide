#include "TestSupport.h"

#include "platform/AppDirectories.h"
#include "workspace/WorkspaceShellTestAccess.h"
#include "workspace/WorkspacePersistenceLegacyFormat.h"
#include "workspace/WorkspaceProjectPresentation.h"
#include "project/GitCompareService.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "WorkspaceShellEventHelpers.h"

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;
using microide::compare::MergeChoice;

bool RectsIntersect(const SDL_FRect& lhs, const SDL_FRect& rhs) {
  return lhs.x < rhs.x + rhs.w && lhs.x + lhs.w > rhs.x && lhs.y < rhs.y + rhs.h &&
         lhs.y + lhs.h > rhs.y;
}

bool AnyRectIntersects(const std::vector<SDL_FRect>& rects, const SDL_FRect& target) {
  return std::any_of(rects.begin(), rects.end(),
                     [&](const SDL_FRect& rect) { return RectsIntersect(rect, target); });
}

float MaxRectHeight(const std::vector<SDL_FRect>& rects) {
  float max_height = 0.0f;
  for (const SDL_FRect& rect : rects) {
    max_height = std::max(max_height, rect.h);
  }
  return max_height;
}

std::filesystem::path ProjectStateDirectoryFor(const std::filesystem::path& project_root) {
  const std::filesystem::path state_root =
      microide::platform::ResolveAppDirectory(microide::platform::UserDirectoryKind::State, "microide");
  return state_root / "projects" / microide::workspace::ProjectStateDirectoryName(project_root);
}

void TestWorkspaceShellRestoreSessionPreservesBranchCompareState() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "history.txt";
  WriteFile(source, "base line\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedEnvVar scoped_xdg_state_home("XDG_STATE_HOME", xdg_state_home.string());
  ScopedEnvVar scoped_xdg_config_home("XDG_CONFIG_HOME", xdg_config_home.string());

  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");
  WriteFile(source, "head line\n");
  CommitAll(root, "head fixture", "head fixture");

  const auto history = microide::project::CollectGitFileHistory(root, source);
  Expect(history.size() == 2, "session restore fixture should have two commits");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenBranchHeadComparison(shell, source, history[1].hash, "base",
                                                            history[0].hash, "head"),
         "branch comparison should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const std::size_t expected_row =
      compare.model.rows.empty() ? 0 : std::min<std::size_t>(1, compare.model.rows.size() - 1);
  compare.selected_row = expected_row;
  compare.scroll_row = 2;
  compare.horizontal_scroll = 4;
  compare.divider_fraction = 0.68f;
  WorkspaceShellTestAccess::SaveSessionState(shell);

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "project-local session restore should succeed");
  Expect(WorkspaceShellTestAccess::OpenTabs(restored).size() == 1,
         "restored branch comparison should reopen as a single tab");

  const auto& rebuilt = WorkspaceShellTestAccess::ActiveCompare(restored);
  Expect(rebuilt.path == source.lexically_normal(),
         "restored branch comparison should keep the original path");
  Expect(rebuilt.commit_hash == history[1].hash,
         "restored branch comparison should preserve the left-side ref");
  Expect(rebuilt.right_ref == history[0].hash,
         "restored branch comparison should preserve the right-side ref");
  Expect(rebuilt.left_label == "base",
         "restored branch comparison should preserve the left label");
  Expect(rebuilt.right_label == "head",
         "restored branch comparison should preserve the right label");
  Expect(rebuilt.selected_row == expected_row,
         "restored branch comparison should preserve the selected row");
  Expect(rebuilt.scroll_row == 2,
         "restored branch comparison should preserve vertical scroll");
  Expect(rebuilt.horizontal_scroll == 4,
         "restored branch comparison should preserve horizontal scroll");
  Expect(std::fabs(rebuilt.divider_fraction - 0.68f) < 0.0001f,
         "restored branch comparison should preserve divider fraction");
  Expect(rebuilt.persistable,
         "restored branch comparison should preserve its session-persistable flag");
}

void TestWorkspaceShellImportsLegacyUserConfigAndArchivesSource() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedEnvVar scoped_xdg_state_home("XDG_STATE_HOME", xdg_state_home.string());
  ScopedEnvVar scoped_xdg_config_home("XDG_CONFIG_HOME", xdg_config_home.string());

  const std::filesystem::path config_root =
      microide::platform::ResolveAppDirectory(microide::platform::UserDirectoryKind::Config, "microide");
  const std::filesystem::path legacy_path = config_root / "user.config";
  WriteFile(legacy_path,
            microide::workspace::SerializeUserConfig(microide::workspace::PersistedUserConfigState{
                .ui_scale = 1.6f,
                .settings = {},
                .disabled_keybinding_ids = {},
            }));

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::RestoreUserConfig(shell),
         "legacy user config should import successfully");
  Expect(std::fabs(shell.UiScale() - 1.6f) < 0.0001f,
         "imported user config should update ui scale");
  Expect(std::filesystem::exists(legacy_path.string() + ".legacy"),
         "legacy user config should be archived");
  Expect(std::filesystem::exists(config_root / "config"),
         "structured user config should be written to the new target path");
}

void TestWorkspaceShellImportsLegacyProjectStateAndArchivesSource() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path sample = root / "main.cpp";
  WriteFile(sample, "int main() { return 0; }\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedEnvVar scoped_xdg_state_home("XDG_STATE_HOME", xdg_state_home.string());
  ScopedEnvVar scoped_xdg_config_home("XDG_CONFIG_HOME", xdg_config_home.string());

  const std::filesystem::path project_state_dir = ProjectStateDirectoryFor(root);
  const std::filesystem::path legacy_path = project_state_dir / "project.state";
  WriteFile(legacy_path,
            microide::workspace::SerializeProjectConfig(
                microide::workspace::PersistedProjectConfigState{
                    .editor_tab_size = 7,
                    .editor_indent_width = 3,
                    .editor_soft_tabs = true,
                    .colorscheme_name = "default",
                    .project_base_color = std::nullopt,
                    .settings = {},
                    .sidebar_policies = {},
                }));

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::RestoreConfigState(shell),
         "legacy project.state should import successfully");
  const auto& state = WorkspaceShellTestAccess::CurrentProjectState(shell);
  Expect(state.editor_preferences.tab_size == 7 &&
             state.editor_preferences.indent_width == 3 &&
             state.editor_preferences.soft_tabs,
         "imported project config should apply editor preferences");
  Expect(std::filesystem::exists(legacy_path.string() + ".legacy"),
         "legacy project.state should be archived");
  Expect(std::filesystem::exists(project_state_dir / "config"),
         "structured project config should be written to the new target path");
}

void TestWorkspaceShellImportsLegacyWorkspaceSessionAndArchivesSource() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path first_root = temp_dir.path() / "project-a";
  const std::filesystem::path second_root = temp_dir.path() / "project-b";
  WriteFile(first_root / "a.txt", "alpha\n");
  WriteFile(second_root / "b.txt", "beta\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedEnvVar scoped_xdg_state_home("XDG_STATE_HOME", xdg_state_home.string());
  ScopedEnvVar scoped_xdg_config_home("XDG_CONFIG_HOME", xdg_config_home.string());

  const std::filesystem::path state_root =
      microide::platform::ResolveAppDirectory(microide::platform::UserDirectoryKind::State, "microide");
  const std::filesystem::path legacy_path = state_root / "session.workspace";
  WriteFile(legacy_path,
            microide::workspace::SerializeWorkspaceSession(
                microide::workspace::PersistedWorkspaceSessionState{
                    .project_roots = {first_root, second_root},
                    .active_project_index = 1,
                }));

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::RestoreWorkspaceSession(shell),
         "legacy workspace session should import successfully");
  Expect(WorkspaceShellTestAccess::ProjectCount(shell) == 2 &&
             WorkspaceShellTestAccess::ActiveProjectIndex(shell) == 1,
         "imported workspace session should restore project catalog state");
  Expect(std::filesystem::exists(legacy_path.string() + ".legacy"),
         "legacy session.workspace should be archived");
  Expect(std::filesystem::exists(state_root / "workspace-session"),
         "structured workspace session should be written to the new target path");
}

void TestWorkspaceShellImportsLegacyChatConversationsAndArchivesSource() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "main.cpp", "int main() { return 0; }\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedEnvVar scoped_xdg_state_home("XDG_STATE_HOME", xdg_state_home.string());
  ScopedEnvVar scoped_xdg_config_home("XDG_CONFIG_HOME", xdg_config_home.string());

  const std::filesystem::path project_state_dir = ProjectStateDirectoryFor(root);
  const std::filesystem::path legacy_path = project_state_dir / "chat.conversations";
  WriteFile(legacy_path,
            "chat-active-conversation conv-1\n"
            "conv-begin conv-1\n"
            "conv-schema 5\n"
            "conv-title Chat\n"
            "msg-begin msg-1\n"
            "msg-role assistant\n"
            "msg-content reply\n"
            "conv-end\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(shell),
         "legacy chat conversations should import into project session");
  const auto& state = WorkspaceShellTestAccess::CurrentProjectState(shell);
  Expect(state.conversations.conversations().size() == 1 &&
             state.panel.chat.conversation_id == "conv-1",
         "imported chat conversations should restore active conversation");
  Expect(std::filesystem::exists(legacy_path.string() + ".legacy"),
         "legacy chat.conversations should be archived");
  Expect(std::filesystem::exists(project_state_dir / "session"),
         "structured project session should be written to the target path");
}

void TestWorkspaceShellRestoreWorkspaceSessionAcrossProjects() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path repo_root = temp_dir.path() / "repo";
  const std::filesystem::path repo_file = repo_root / "src" / "compare.txt";
  WriteFile(repo_file, "base line\n");

  const std::filesystem::path second_root = temp_dir.path() / "notes-project";
  const std::filesystem::path second_file = second_root / "notes.txt";
  WriteFile(second_file, "notes\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedEnvVar scoped_xdg_state_home("XDG_STATE_HOME", xdg_state_home.string());
  ScopedEnvVar scoped_xdg_config_home("XDG_CONFIG_HOME", xdg_config_home.string());

  InitializeGitRepo(repo_root);
  CommitAll(repo_root, "base fixture", "base fixture");
  WriteFile(repo_file, "head line\n");
  CommitAll(repo_root, "head fixture", "head fixture");

  const auto history = microide::project::CollectGitFileHistory(repo_root, repo_file);
  Expect(history.size() == 2, "workspace restore fixture should have two commits");

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, repo_root, false, false),
         "first project should open");
  Expect(WorkspaceShellTestAccess::OpenBranchHeadComparison(shell, repo_file, history[1].hash, "base",
                                                            history[0].hash, "head"),
         "branch comparison should open in the first project");
  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const std::size_t expected_row =
      compare.model.rows.empty() ? 0 : std::min<std::size_t>(1, compare.model.rows.size() - 1);
  compare.selected_row = expected_row;
  compare.scroll_row = 3;
  compare.horizontal_scroll = 6;

  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, second_root, false, false),
         "second project should open");
  WorkspaceShellTestAccess::OpenFile(shell, second_file);
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 1,
         "second project should persist one editor tab");
  WorkspaceShellTestAccess::SaveSessionState(shell);
  WorkspaceShellTestAccess::SaveWorkspaceSession(shell);

  WorkspaceShell restored;
  Expect(WorkspaceShellTestAccess::RestoreWorkspaceSession(restored),
         "workspace session restore should succeed");
  Expect(WorkspaceShellTestAccess::ProjectCount(restored) == 2,
         "workspace restore should reopen both projects");
  Expect(WorkspaceShellTestAccess::ActiveProjectIndex(restored) == 1,
         "workspace restore should preserve the active project index");
  Expect(WorkspaceShellTestAccess::ProjectRoot(restored) == second_root.lexically_normal(),
         "workspace restore should reactivate the second project");
  Expect(WorkspaceShellTestAccess::ActiveEditor(restored).path() == second_file.lexically_normal(),
         "workspace restore should reopen the active project's editor tab");

  Expect(WorkspaceShellTestAccess::SwitchProject(restored, 0, false),
         "switching to the restored first project should succeed");
  Expect(WorkspaceShellTestAccess::ProjectRoot(restored) == repo_root.lexically_normal(),
         "switch should activate the first restored project");
  Expect(WorkspaceShellTestAccess::OpenTabs(restored).size() == 1,
         "first restored project should reopen the compare tab");

  const auto& rebuilt = WorkspaceShellTestAccess::ActiveCompare(restored);
  Expect(rebuilt.path == repo_file.lexically_normal(),
         "restored first project should keep the compare path");
  Expect(rebuilt.commit_hash == history[1].hash,
         "restored first project should preserve the compare left ref");
  Expect(rebuilt.right_ref == history[0].hash,
         "restored first project should preserve the compare right ref");
  Expect(rebuilt.left_label == "base",
         "restored first project should preserve the compare left label");
  Expect(rebuilt.right_label == "head",
         "restored first project should preserve the compare right label");
  Expect(rebuilt.selected_row == expected_row,
         "restored first project should preserve the compare selected row");
  Expect(rebuilt.scroll_row == 3,
         "restored first project should preserve compare vertical scroll");
  Expect(rebuilt.horizontal_scroll == 6,
         "restored first project should preserve compare horizontal scroll");
}

void TestWorkspaceShellShutdownPreservesDistinctWorkspaceProjectRoots() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path first_root = temp_dir.path() / "project-a";
  const std::filesystem::path first_file = first_root / "a.txt";
  WriteFile(first_file, "alpha\n");

  const std::filesystem::path second_root = temp_dir.path() / "project-b";
  const std::filesystem::path second_file = second_root / "b.txt";
  WriteFile(second_file, "beta\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedEnvVar scoped_xdg_state_home("XDG_STATE_HOME", xdg_state_home.string());
  ScopedEnvVar scoped_xdg_config_home("XDG_CONFIG_HOME", xdg_config_home.string());

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, first_root, false, false),
         "first workspace project should open");
  WorkspaceShellTestAccess::OpenFile(shell, first_file);
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, second_root, false, false),
         "second workspace project should open");
  WorkspaceShellTestAccess::OpenFile(shell, second_file);
  Expect(WorkspaceShellTestAccess::SwitchProject(shell, 0, false),
         "fixture should make the first project active before shutdown");

  shell.Shutdown();

  WorkspaceShell restored;
  Expect(WorkspaceShellTestAccess::RestoreWorkspaceSession(restored),
         "workspace session restore after shutdown should succeed");
  Expect(WorkspaceShellTestAccess::ProjectCount(restored) == 2,
         "workspace restore should keep both project entries");
  Expect(WorkspaceShellTestAccess::ProjectRoot(restored) == first_root.lexically_normal(),
         "workspace restore should reactivate the first project");

  const auto restored_roots = WorkspaceShellTestAccess::ProjectRoots(restored);
  Expect(restored_roots.size() == 2,
         "restored workspace should expose two distinct project roots");
  Expect(restored_roots[0] == first_root.lexically_normal(),
         "restored workspace should preserve the first saved project root");
  Expect(restored_roots[1] == second_root.lexically_normal(),
         "restored workspace should preserve the second saved project root");
}

void TestWorkspaceShellMergeHorizontalNavigationInvalidatesResultPane() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "output.txt";
  WriteFile(base, "line 1\nline 2\nline 3\n");
  WriteFile(incoming, "line 1\nincoming 2\nline 3\n");
  WriteFile(current, "line 1\ncurrent 2\nline 3\n");
  WriteFile(output, "<<<<<<< ours\ncurrent 2\n=======\nincoming 2\n>>>>>>> theirs\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge invalidation fixture should open");
  (void)shell.ConsumePendingRenderInvalidation();

  const auto surface = WorkspaceShellTestAccess::ActiveMergeSurfaceLayout(shell);
  const SDL_FRect result_rect = WorkspaceShellTestAccess::ActiveMergeResultRect(shell);
  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);
  const SDL_FRect left_rect =
      microide::workspace::MakeRect(surface.left_x, layout.editor_surface.y,
                                    surface.gutter_width + surface.left_width,
                                    layout.editor_surface.h);

  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.key = SDLK_RIGHT;
  const auto result = shell.HandleEvent(event);
  const auto redraw_rect = result.redraw.SingleRectIfOnlyOne();

  Expect(result.handled, "merge horizontal navigation should be handled");
  Expect(!result.redraw.full && redraw_rect.has_value(),
         "merge horizontal navigation should stay on a partial redraw path");
  Expect(RectsIntersect(*redraw_rect, result_rect),
         "merge horizontal navigation should repaint the result pane");
  Expect(!RectsIntersect(*redraw_rect, left_rect),
         "merge horizontal navigation should avoid repainting the incoming pane");
}

void TestWorkspaceShellMoveMergeSelectionInvalidatesConflictBand() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "output.txt";
  WriteFile(base, "line 1\nline 2\nline 3\nline 4\nline 5\n");
  WriteFile(incoming, "line 1\nincoming 2\nline 3\nincoming 4\nline 5\n");
  WriteFile(current, "line 1\ncurrent 2\nline 3\ncurrent 4\nline 5\n");
  WriteFile(output,
            "<<<<<<< ours\ncurrent 2\n=======\nincoming 2\n>>>>>>> theirs\n"
            "line 3\n"
            "<<<<<<< ours\ncurrent 4\n=======\nincoming 4\n>>>>>>> theirs\n"
            "line 5\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge conflict invalidation fixture should open");
  (void)shell.ConsumePendingRenderInvalidation();

  const auto previous_conflict_rect = WorkspaceShellTestAccess::ActiveMergeConflictRect(shell, 0);
  WorkspaceShellTestAccess::MoveMergeSelection(shell, 1);
  const auto redraw = shell.ConsumePendingRenderInvalidation();
  const auto next_conflict_rect = WorkspaceShellTestAccess::ActiveMergeConflictRect(shell, 1);
  const auto layout = WorkspaceShellTestAccess::CurrentLayout(shell);

  Expect(!redraw.full && !redraw.rects.empty(),
         "merge conflict navigation should stay on a partial redraw path");
  Expect(previous_conflict_rect.has_value() && AnyRectIntersects(redraw.rects, *previous_conflict_rect),
         "merge conflict navigation should repaint the previously selected conflict");
  Expect(next_conflict_rect.has_value() && AnyRectIntersects(redraw.rects, *next_conflict_rect),
         "merge conflict navigation should repaint the newly selected conflict");
  Expect(MaxRectHeight(redraw.rects) < layout.editor_surface.h,
         "merge conflict navigation should redraw less than the full merge surface height");
}

void TestWorkspaceShellRestoreSessionPreservesRenamedWorkingTreeCompareState() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "compare.txt";
  WriteFile(source, "zero\none\ntwo\nthree\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedEnvVar scoped_xdg_state_home("XDG_STATE_HOME", xdg_state_home.string());
  ScopedEnvVar scoped_xdg_config_home("XDG_CONFIG_HOME", xdg_config_home.string());

  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");
  WriteFile(source, "zero\none changed\ntwo changed\nthree changed\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open before rename");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  const std::size_t expected_row =
      compare.model.rows.empty() ? 0 : std::min<std::size_t>(2, compare.model.rows.size() - 1);
  compare.selected_row = expected_row;
  compare.scroll_row = 2;
  compare.horizontal_scroll = 5;

  WorkspaceShellTestAccess::PrepareRenamePrompt(shell, root / "src", "renamed-src");
  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);

  const std::filesystem::path renamed = root / "renamed-src" / "compare.txt";
  Expect(std::filesystem::is_regular_file(renamed),
         "renamed working-tree compare fixture should create the destination file");
  WorkspaceShellTestAccess::SaveSessionState(shell);

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "session restore should rebuild the renamed working-tree compare");
  Expect(WorkspaceShellTestAccess::OpenTabs(restored).size() == 1,
         "renamed working-tree compare should restore as a single tab");

  const auto& rebuilt = WorkspaceShellTestAccess::ActiveCompare(restored);
  Expect(rebuilt.path == renamed.lexically_normal(),
         "restored compare should preserve the renamed live path");
  Expect(rebuilt.left_path == source.lexically_normal(),
         "restored compare should preserve the original commit-side path");
  Expect(rebuilt.right_path == renamed.lexically_normal(),
         "restored compare should preserve the renamed working-tree path");
  Expect(rebuilt.commit_hash == "HEAD",
         "restored compare should preserve the left-side ref");
  Expect(rebuilt.right_ref == "WORKTREE",
         "restored compare should preserve the working-tree right-side ref");
  Expect(rebuilt.selected_row == expected_row,
         "restored compare should preserve the selected row");
  Expect(rebuilt.scroll_row == 2,
         "restored compare should preserve vertical scroll");
  Expect(rebuilt.horizontal_scroll == 5,
         "restored compare should preserve horizontal scroll");
  const bool kept_compare_content = std::any_of(
      rebuilt.model.rows.begin(), rebuilt.model.rows.end(), [](const auto& row) {
        return row.left_text == "one" && row.right_text == "one changed";
      });
  Expect(kept_compare_content,
         "restored compare should preserve the pre-rename commit-vs-working-tree content");
}

void TestWorkspaceShellCompareSyntaxTokensAreDeferredUntilRender() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "src" / "compare.txt";
  WriteFile(source, "alpha\nbeta\ngamma\n");

  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");
  WriteFile(source, "alpha changed\nbeta changed\ngamma changed\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open for deferred syntax-token fixture");

  const auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  Expect(compare.left_tokens_by_row.size() == compare.model.rows.size(),
         "deferred compare syntax should size left token cache to compare rows");
  Expect(compare.right_tokens_by_row.size() == compare.model.rows.size(),
         "deferred compare syntax should size right token cache to compare rows");
  Expect(compare.syntax_rows_tokenized == 0,
         "deferred compare syntax should avoid eager tokenization during tab open");
}

void TestWorkspaceShellReopenFileReloadsCleanEditorTab() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "alpha long editor line\nbeta long editor line\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.SetViewportSize(1, 8);
  editor.MoveCursorTo(1, 2);
  editor.SetScrollLine(1);
  editor.SetHorizontalScroll(1);

  WriteFile(source, "alpha refreshed editor line\nbeta refreshed editor line\n");
  WorkspaceShellTestAccess::OpenFile(shell, source);

  const auto& reopened = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 1,
         "reopening a clean editor should reuse the existing tab");
  Expect(reopened.lines()[0] == "alpha refreshed editor line",
         "reopening a clean editor should reload the latest disk content");
  Expect(reopened.lines()[1] == "beta refreshed editor line",
         "reopening a clean editor should refresh every line from disk");
  Expect(reopened.cursor_line() == 1 && reopened.cursor_column() == 2,
         "reopening a clean editor should preserve cursor state");
  Expect(!reopened.dirty(),
         "reopening a clean editor should keep the tab clean");
}

void TestWorkspaceShellRefreshReloadsCleanOpenEditorBuffers() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "alpha\nbeta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);

  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.MoveCursorTo(1, 1);
  editor.SetScrollLine(1);

  WriteFile(source, "alpha refreshed\nbeta refreshed\n");
  Expect(WorkspaceShellTestAccess::ExecuteTreeRefresh(shell),
         "tree refresh should execute");

  const auto& refreshed = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(refreshed.lines()[0] == "alpha refreshed",
         "tree refresh should reload clean open editor buffers from disk");
  Expect(refreshed.lines()[1] == "beta refreshed",
         "tree refresh should refresh every line in the clean open buffer");
  Expect(refreshed.cursor_line() == 1 && refreshed.cursor_column() == 1,
         "tree refresh should preserve cursor state while reloading the buffer");
}

void TestWorkspaceShellRestoreSessionPreservesDirtyEditorBufferContent() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "alpha\nbeta\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedEnvVar scoped_xdg_state_home("XDG_STATE_HOME", xdg_state_home.string());
  ScopedEnvVar scoped_xdg_config_home("XDG_CONFIG_HOME", xdg_config_home.string());

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, source);
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  editor.MoveCursorTo(1, 0);
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "dirty "),
         "dirty editor-session fixture should accept text input");
  editor.SetScrollLine(1);
  WorkspaceShellTestAccess::SaveSessionState(shell);

  WriteFile(source, "alpha disk replacement\nbeta disk replacement\n");

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "dirty editor-session restore should succeed");
  WorkspaceShellTestAccess::ActivateTab(restored, 0);

  const auto& reopened = WorkspaceShellTestAccess::ActiveEditor(restored);
  Expect(reopened.lines()[1] == "dirty beta",
         "dirty editor-session restore should prefer the unsaved in-memory buffer over disk changes");
  Expect(reopened.dirty(),
         "dirty editor-session restore should preserve the dirty state");
  Expect(reopened.cursor_line() == 1 && reopened.cursor_column() == 6,
         "dirty editor-session restore should preserve the caret location");
  Expect(reopened.scroll_line() == 1,
         "dirty editor-session restore should preserve the scroll position");
}

void TestWorkspaceShellRestoreSessionPreservesDirtyUntitledBufferContent() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "README.md", "root\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedEnvVar scoped_xdg_state_home("XDG_STATE_HOME", xdg_state_home.string());
  ScopedEnvVar scoped_xdg_config_home("XDG_CONFIG_HOME", xdg_config_home.string());

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "tab"),
         "untitled session fixture should open a new untitled tab");
  auto& editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "scratch buffer"),
         "untitled session fixture should accept text input");
  editor.SetScrollLine(0);
  WorkspaceShellTestAccess::SaveSessionState(shell);

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "untitled session restore should succeed");
  WorkspaceShellTestAccess::ActivateTab(restored, 0);

  const auto& reopened = WorkspaceShellTestAccess::ActiveEditor(restored);
  Expect(reopened.path().empty(),
         "untitled session restore should preserve the missing file path");
  Expect(reopened.lines().size() == 1 && reopened.lines()[0] == "scratch buffer",
         "untitled session restore should reopen the unsaved untitled buffer contents");
  Expect(reopened.dirty(),
         "untitled session restore should preserve the dirty state");
}

void TestWorkspaceShellQuitShutdownPersistsDirtyEditorBuffers() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "alpha\nbeta\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedEnvVar scoped_xdg_state_home("XDG_STATE_HOME", xdg_state_home.string());
  ScopedEnvVar scoped_xdg_config_home("XDG_CONFIG_HOME", xdg_config_home.string());

  WorkspaceShell shell;
  Expect(WorkspaceShellTestAccess::OpenProjectTab(shell, root, false, false),
         "quit session fixture should open the project");
  WorkspaceShellTestAccess::OpenFile(shell, source);
  auto& file_editor = WorkspaceShellTestAccess::ActiveEditor(shell);
  file_editor.MoveCursorTo(1, 0);
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "dirty "),
         "quit session fixture should dirty the file-backed editor");
  file_editor.SetScrollLine(1);

  Expect(WorkspaceShellTestAccess::ExecuteCommandLine(shell, "tab"),
         "quit session fixture should open an untitled tab");
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "scratch buffer"),
         "quit session fixture should dirty the untitled editor");

  shell.RequestQuit();
  Expect(!WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "quit should not show the dirty prompt for dirty editors");
  Expect(shell.ConsumeQuitRequested(),
         "quit should signal shutdown immediately");

  shell.Shutdown();

  Expect(ReadFile(source) == "alpha\nbeta\n",
         "quit session persistence should not write dirty file-backed buffers to disk");

  WorkspaceShell restored;
  Expect(WorkspaceShellTestAccess::RestoreWorkspaceSession(restored),
         "workspace session restore after quit should succeed");
  Expect(WorkspaceShellTestAccess::ProjectCount(restored) == 1,
         "workspace session restore after quit should reopen the saved project");
  Expect(WorkspaceShellTestAccess::ProjectRoot(restored) == root.lexically_normal(),
         "workspace session restore after quit should reactivate the original project");

  const auto& tabs = WorkspaceShellTestAccess::OpenTabs(restored);
  Expect(tabs.size() == 2,
         "workspace session restore after quit should reopen both dirty editor tabs");

  std::size_t file_tab_index = tabs.size();
  std::size_t untitled_tab_index = tabs.size();
  for (std::size_t i = 0; i < tabs.size(); ++i) {
    if (tabs[i].path == source.lexically_normal()) {
      file_tab_index = i;
    }
    if (tabs[i].path.empty()) {
      untitled_tab_index = i;
    }
  }

  Expect(file_tab_index < tabs.size(),
         "workspace session restore after quit should reopen the dirty file-backed editor");
  Expect(untitled_tab_index < tabs.size(),
         "workspace session restore after quit should reopen the dirty untitled editor");

  WorkspaceShellTestAccess::ActivateTab(restored, file_tab_index);
  const auto& reopened_file = WorkspaceShellTestAccess::ActiveEditor(restored);
  Expect(reopened_file.lines()[1] == "dirty beta",
         "workspace session restore after quit should preserve the unsaved file-backed buffer");
  Expect(reopened_file.dirty(),
         "workspace session restore after quit should keep the file-backed editor dirty");
  Expect(reopened_file.cursor_line() == 1 && reopened_file.cursor_column() == 6,
         "workspace session restore after quit should preserve the file-backed caret position");
  Expect(reopened_file.scroll_line() == 1,
         "workspace session restore after quit should preserve the file-backed scroll position");

  WorkspaceShellTestAccess::ActivateTab(restored, untitled_tab_index);
  const auto& reopened_untitled = WorkspaceShellTestAccess::ActiveEditor(restored);
  Expect(reopened_untitled.path().empty(),
         "workspace session restore after quit should preserve the untitled editor path");
  Expect(reopened_untitled.lines().size() == 1 &&
             reopened_untitled.lines()[0] == "scratch buffer",
         "workspace session restore after quit should preserve the untitled buffer contents");
  Expect(reopened_untitled.dirty(),
         "workspace session restore after quit should keep the untitled editor dirty");
}

void TestWorkspaceShellReopenWorkingTreeComparisonRefreshesExistingTab() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "repo";
  const std::filesystem::path source = root / "compare.txt";
  WriteFile(source, "alpha\nbeta\n");

  InitializeGitRepo(root);
  CommitAll(root, "base fixture", "base fixture");
  WriteFile(source, "alpha working\nbeta\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "working-tree comparison should open");

  auto& compare = WorkspaceShellTestAccess::ActiveCompare(shell);
  compare.selected_row = compare.model.rows.empty()
                             ? 0
                             : std::min<std::size_t>(1, compare.model.rows.size() - 1);
  compare.scroll_row = 1;
  compare.horizontal_scroll = 3;
  const std::size_t expected_selected_row = compare.selected_row;

  WriteFile(source, "alpha refreshed\nbeta\n");
  Expect(WorkspaceShellTestAccess::OpenWorkingTreeComparison(shell, source, "HEAD", "HEAD"),
         "reopening the same working-tree comparison should succeed");

  const auto& reopened = WorkspaceShellTestAccess::ActiveCompare(shell);
  const bool saw_refreshed_right_side =
      std::any_of(reopened.model.rows.begin(), reopened.model.rows.end(), [](const auto& row) {
        return row.right_text == "alpha refreshed";
      });
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 1,
         "reopening a comparison should refresh the existing tab in place");
  Expect(saw_refreshed_right_side,
         "reopening a comparison should rebuild the working-tree side from disk");
  Expect(reopened.selected_row == expected_selected_row,
         "reopening a comparison should preserve the selected row");
  Expect(reopened.scroll_row == 1 && reopened.horizontal_scroll == 3,
         "reopening a comparison should preserve scroll state");
}

void TestWorkspaceShellMergeSyntaxTokensAreDeferredUntilRender() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.cpp";
  const std::filesystem::path incoming = root / "incoming.cpp";
  const std::filesystem::path current = root / "current.cpp";
  const std::filesystem::path output = root / "result.cpp";
  WriteFile(base, "int main() {\n  return 0;\n}\n");
  WriteFile(incoming, "int main() {\n  return 1;\n}\n");
  WriteFile(current, "int main() {\n  return 2;\n}\n");
  WriteFile(output, "int main() {\n  return 0;\n}\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge editor should open for deferred syntax-token fixture");

  const auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(merge.incoming_tokens.size() == merge.model.incoming_lines.size(),
         "deferred merge syntax should size incoming token cache to incoming lines");
  Expect(merge.current_tokens.size() == merge.model.current_lines.size(),
         "deferred merge syntax should size current token cache to current lines");
  Expect(merge.incoming_syntax_rows_tokenized == 0,
         "deferred merge syntax should avoid eager incoming tokenization during tab open");
  Expect(merge.current_syntax_rows_tokenized == 0,
         "deferred merge syntax should avoid eager current tokenization during tab open");
}

void TestWorkspaceShellReopenMergeEditorRefreshesCleanTabFromOutput() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  WriteFile(base, "header\nshared\nfooter\n");
  WriteFile(incoming, "header\nincoming line 1\nincoming line 2\nfooter\n");
  WriteFile(current, "header\ncurrent line 1\ncurrent line 2\nfooter\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, current),
         "merge editor should open when the output path is the current file");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(!merge.result_viewport.dirty(),
         "merge output seeded from disk should start clean");
  Expect(merge.result_viewport.lines()[1] == "current line 1",
         "merge result should load the existing output buffer from disk");
  Expect(!merge.conflicts.empty() && merge.conflicts.front().last_choice == MergeChoice::Current,
         "merge result loaded from disk should infer the matching conflict choice");
  merge.result_viewport.SetViewportSize(1, 8);
  merge.selected_hunk = 0;
  merge.scroll_row = 1;
  merge.horizontal_scroll = 2;
  merge.left_divider_fraction = 0.29f;
  merge.right_divider_fraction = 0.73f;
  merge.result_viewport.SetScrollLine(1);
  merge.result_viewport.SetHorizontalScroll(2);

  WriteFile(current, "header\ncurrent line 1 updated\ncurrent line 2 updated\ncurrent line 3 updated\nfooter\n");
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, current),
         "reopening the same merge editor should succeed");

  const auto& reopened = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 1,
         "reopening a clean merge tab should refresh the existing tab in place");
  Expect(!reopened.result_viewport.dirty(),
         "reopening a clean merge tab should keep the result clean");
  Expect(reopened.result_viewport.lines()[1] == "current line 1 updated",
         "reopening a clean merge tab should reload the latest output content");
  Expect(reopened.result_viewport.lines()[3] == "current line 3 updated",
         "reopening a clean merge tab should handle multiline current-side output");
  Expect(!reopened.conflicts.empty() && reopened.conflicts.front().last_choice == MergeChoice::Current,
         "reopened merge output should continue to infer the current-side conflict choice");
  Expect(reopened.selected_hunk == 0,
         "reopening a clean merge tab should preserve the selected conflict");
  Expect(reopened.scroll_row == 1 && reopened.horizontal_scroll == 2,
         "reopening a clean merge tab should preserve scroll state");
  Expect(std::fabs(reopened.left_divider_fraction - 0.29f) < 0.0001f &&
             std::fabs(reopened.right_divider_fraction - 0.73f) < 0.0001f,
         "reopening a clean merge tab should preserve divider positions");
}

void TestWorkspaceShellMergeEditorUsesWorkingTreeConflictMarkers() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "result.txt";
  WriteFile(base, "section 1\nvalue: base 1\ncontext: unchanged1\n");
  WriteFile(incoming, "section 1\nvalue: feature 1\ncontext: unchanged1\n");
  WriteFile(current, "section 1\nvalue: main 1\ncontext: unchanged1\n");
  WriteFile(output,
            "section 1\n"
            "<<<<<<< HEAD\n"
            "value: main 1\n"
            "let's see what happens with many conflicts\n"
            "this is multiline by design\n"
            "=======\n"
            "value: feature 1\n"
            ">>>>>>> feature-branch\n"
            "context: unchanged1\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge editor should open for conflict-marker fixture");

  const auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(!merge.result_viewport.dirty(),
         "opening from working-tree conflict markers should not dirty the merge tab");
  Expect(merge.result_viewport.lines().size() == 5,
         "working-tree conflict markers should collapse into a markerless result buffer");
  Expect(merge.result_viewport.lines()[1] == "value: main 1",
         "working-tree conflict markers should preserve the current block");
  Expect(merge.result_viewport.lines()[2] == "let's see what happens with many conflicts",
         "working-tree conflict markers should preserve multiline edits in the current block");
  Expect(merge.result_viewport.lines()[3] == "this is multiline by design",
         "working-tree conflict markers should preserve every extra current-side line");
  Expect(!merge.conflicts.empty() && merge.conflicts.front().valid,
         "working-tree conflict markers should keep the conflict span actionable");
  Expect(merge.conflicts.front().last_choice == MergeChoice::Current,
         "working-tree conflict markers with current-side edits should infer the current choice");
  Expect(merge.conflicts.front().start_line == 1 && merge.conflicts.front().end_line == 4,
         "working-tree conflict markers should size the tracked conflict span to the edited block");
}

void TestWorkspaceShellMergeEditorParsesLargeWorkingTreeConflictBlock() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "result.txt";
  WriteFile(base,
            "section 1\nvalue: base 1\ncontext: unchanged1\n\n"
            "section 2\nvalue: base 2\ncontext: unchanged2\n");
  WriteFile(incoming,
            "section 1\nvalue: feature 1\ncontext: unchanged1\n\n"
            "section 2\nvalue: feature 2\ncontext: unchanged2\n");
  WriteFile(current,
            "section 1\nvalue: main 1\ncontext: unchanged1\n\n"
            "section 2\nvalue: main 2\ncontext: unchanged2\n");
  WriteFile(output,
            "section 1\n"
            "<<<<<<< HEAD\n"
            "value: main 1\n"
            "this is multiline by design\n"
            "context: unchanged1\n"
            "\n"
            "section 2\n"
            "value: main 2\n"
            "context: unchanged2\n"
            "=======\n"
            "value: feature 1\n"
            "context: unchanged1\n"
            "\n"
            "section 2\n"
            "value: feature 2\n"
            "context: unchanged2\n"
            ">>>>>>> feature-branch\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge editor should open for large conflict-block fixture");

  const auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(!merge.result_viewport.dirty(),
         "large working-tree conflict blocks should open clean");
  Expect(merge.result_viewport.lines()[1] == "value: main 1",
         "large working-tree conflict blocks should preserve current-side content");
  Expect(merge.result_viewport.lines()[2] == "this is multiline by design",
         "large working-tree conflict blocks should preserve extra current-side lines");
  Expect(merge.result_viewport.lines()[6] == "value: main 2",
         "large working-tree conflict blocks should continue with later current-side conflicts");
  Expect(merge.result_viewport.lines()[7] == "context: unchanged2",
         "large working-tree conflict blocks should keep later context lines in place");
  Expect(merge.conflicts.size() == 2,
         "large working-tree conflict blocks should still map back to individual merge conflicts");
  Expect(merge.conflicts[0].valid,
         "large working-tree conflict blocks should keep the first conflict actionable");
  Expect(merge.conflicts[0].start_line == 1 && merge.conflicts[0].end_line == 3,
         "large working-tree conflict blocks should expand the first conflict span around inserted lines");
  Expect(merge.conflicts[1].valid,
         "large working-tree conflict blocks should keep later conflicts actionable");
  Expect(merge.conflicts[1].last_choice == MergeChoice::Current,
         "later conflicts in a large block should infer the current-side choice");
}

void TestWorkspaceShellMergeChoicePreservesManualEditsAroundConflicts() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "result.txt";
  WriteFile(base, "zero\none\ntwo\nthree\nfour\n");
  WriteFile(incoming, "zero\none incoming\ntwo\nthree incoming\nfour\n");
  WriteFile(current, "zero\none current\ntwo\nthree current\nfour\n");
  WriteFile(output, "zero\none\ntwo\nthree\nfour\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge editor should open for manual-edit preservation fixture");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  merge.result_viewport.MoveCursorTo(2, 0);
  Expect(WorkspaceShellTestAccess::HandleTextInput(shell, "edited "),
         "merge result should accept direct text input");

  merge.selected_hunk = 1;
  WorkspaceShellTestAccess::ApplyMergeChoice(shell, MergeChoice::Incoming);

  const auto& lines = merge.result_viewport.lines();
  Expect(lines[1] == "one",
         "accepting a later conflict should not rebuild earlier untouched conflicts");
  Expect(lines[2] == "edited two",
         "accepting a later conflict should preserve surrounding manual result edits");
  Expect(lines[3] == "three incoming",
         "accepting a conflict should patch only the selected conflict span");
}

void TestWorkspaceShellMergeConflictTrackingShiftsAfterInsertion() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "result.txt";
  WriteFile(base, "zero\none\ntwo\nthree\nfour\n");
  WriteFile(incoming, "zero\none incoming\ntwo\nthree incoming\nfour\n");
  WriteFile(current, "zero\none current\ntwo\nthree current\nfour\n");
  WriteFile(output, "zero\none\ntwo\nthree\nfour\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge editor should open for conflict-tracking fixture");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  merge.result_viewport.MoveCursorTo(2, 0);
  Expect(SendKeyDown(shell, SDLK_RETURN, SDL_KMOD_NONE),
         "merge result should support inserting lines");

  merge.selected_hunk = 1;
  WorkspaceShellTestAccess::ApplyMergeChoice(shell, MergeChoice::Current);

  const auto& lines = merge.result_viewport.lines();
  Expect(lines[2].empty(),
         "manual result insertions should remain in place after tracked-span updates");
  Expect(lines[4] == "three current",
         "later conflict accepts should follow the shifted tracked span");
}

void TestWorkspaceShellMergeHoverPreviewDoesNotCommitState() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "result.txt";
  WriteFile(base,
            "alpha long content 0123456789 abcdefghijklmnopqrstuvwxyz alpha long content "
            "0123456789 abcdefghijklmnopqrstuvwxyz\none\ntwo\nthree\nfour\nfive\nsix\n");
  WriteFile(incoming,
            "incoming long content 0123456789 abcdefghijklmnopqrstuvwxyz incoming long content "
            "0123456789 abcdefghijklmnopqrstuvwxyz\none\ntwo\nthree\nfour\nfive\nsix\n");
  WriteFile(current,
            "current long content 0123456789 abcdefghijklmnopqrstuvwxyz current long content "
            "0123456789 abcdefghijklmnopqrstuvwxyz\none\ntwo\nthree\nfour\nfive\nsix\n");
  WriteFile(output,
            "alpha long content 0123456789 abcdefghijklmnopqrstuvwxyz alpha long content "
            "0123456789 abcdefghijklmnopqrstuvwxyz\none\ntwo\nthree\nfour\nfive\nsix\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1200, 900);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge editor should open for hover-preview fixture");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  const auto before_lines = merge.result_viewport.lines();
  const bool before_dirty = merge.result_viewport.dirty();
  const auto surface = WorkspaceShellTestAccess::ActiveMergeSurfaceLayout(shell);
  const auto& conflict = merge.conflicts.front();
  const float x = surface.left_x + surface.gutter_width + 12.0f;
  const float y =
      surface.rows_y +
      (static_cast<float>(conflict.incoming_start_line) - static_cast<float>(merge.scroll_row) + 0.5f) *
          surface.line_height;

  Expect(SendMouseMotion(shell, x, y, 0),
         "merge hover fixture should register pointer motion");

  const auto& hover = WorkspaceShellTestAccess::ActiveMergeHoverState(shell);
  Expect(hover.has_value(), "merge hover fixture should produce a hover state");
  Expect(WorkspaceShellTestAccess::ActiveMergeHoverIsIncomingConflict(shell),
         "hovering incoming content should preview the incoming choice");
  Expect(WorkspaceShellTestAccess::ActiveMergeHoverPreviewChoice(shell) == MergeChoice::Incoming,
         "incoming hover preview should advertise the incoming merge choice");
  Expect(merge.result_viewport.lines() == before_lines,
         "hover preview should not mutate the committed result buffer");
  Expect(merge.result_viewport.dirty() == before_dirty,
         "hover preview should not dirty the merge result");
}

void OpenMergeHoverFixture(WorkspaceShell& shell,
                           TemporaryDirectory& temp_dir,
                           std::filesystem::path* base_path,
                           std::filesystem::path* incoming_path,
                           std::filesystem::path* current_path,
                           std::filesystem::path* output_path) {
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "result.txt";
  WriteFile(base, "top\nbase change\nbottom\n");
  WriteFile(incoming, "top\nincoming change\nbottom\n");
  WriteFile(current, "top\ncurrent change\nbottom\n");
  WriteFile(output, "top\nbase change\nbottom\n");

  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 900);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge hover fixture should open a merge editor");

  if (base_path != nullptr) {
    *base_path = base;
  }
  if (incoming_path != nullptr) {
    *incoming_path = incoming;
  }
  if (current_path != nullptr) {
    *current_path = current;
  }
  if (output_path != nullptr) {
    *output_path = output;
  }
}

void OpenTallMergeMouseFixture(WorkspaceShell& shell, TemporaryDirectory& temp_dir) {
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "result.txt";

  std::string base_text;
  std::string incoming_text;
  std::string current_text;
  for (int i = 0; i < 120; ++i) {
    const std::string line = "line " + std::to_string(i) + "\n";
    base_text += line;
    incoming_text += i == 60 ? "incoming change\n" : line;
    current_text += i == 60 ? "current change\n" : line;
  }

  WriteFile(base, base_text);
  WriteFile(incoming, incoming_text);
  WriteFile(current, current_text);
  WriteFile(output, base_text);

  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 360);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "tall merge mouse fixture should open a merge editor");
}

void TestWorkspaceShellMergeHoverPrefersIncomingAcceptButton() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  OpenMergeHoverFixture(shell, temp_dir, nullptr, nullptr, nullptr, nullptr);

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(!merge.conflicts.empty(), "incoming-accept hover fixture should expose a merge conflict");

  const SDL_FRect accept_rect = WorkspaceShellTestAccess::MergeSourceAcceptRect(shell, 0, true);
  const float hover_x = accept_rect.x + accept_rect.w * 0.5f;
  const float hover_y = accept_rect.y + accept_rect.h * 0.5f;

  Expect(SendMouseMotion(shell, hover_x, hover_y, 0),
         "hovering the incoming accept button should request a redraw");

  const auto& hover = WorkspaceShellTestAccess::ActiveMergeHoverState(shell);
  Expect(hover.has_value(), "incoming accept button hover should produce a hover state");
  Expect(hover->kind == microide::workspace::MergeHoverState::Kind::IncomingAccept,
         "incoming accept button hover should take precedence over the source conflict hover");
  Expect(hover->preview_choice == MergeChoice::Incoming,
         "incoming accept button hover should advertise the incoming merge choice");
}

void TestWorkspaceShellMergeHoverPrefersResultActionButton() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  OpenMergeHoverFixture(shell, temp_dir, nullptr, nullptr, nullptr, nullptr);

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(!merge.conflicts.empty(), "result-action hover fixture should expose a merge conflict");

  const std::array<SDL_FRect, 4> action_rects =
      WorkspaceShellTestAccess::MergeResultActionRects(shell, 0);
  const float hover_x = action_rects[3].x + action_rects[3].w * 0.5f;
  const float hover_y = action_rects[3].y + action_rects[3].h * 0.5f;

  Expect(SendMouseMotion(shell, hover_x, hover_y, 0),
         "hovering a merge result action button should request a redraw");

  const auto& hover = WorkspaceShellTestAccess::ActiveMergeHoverState(shell);
  Expect(hover.has_value(), "result action button hover should produce a hover state");
  Expect(hover->kind == microide::workspace::MergeHoverState::Kind::ResultAction,
         "result action button hover should take precedence over generic result conflict hover");
  Expect(hover->preview_choice == MergeChoice::Both,
         "result action button hover should advertise the hovered merge choice");
}

void TestWorkspaceShellMergeResultDragSelectionTracksPointer() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  OpenMergeHoverFixture(shell, temp_dir, nullptr, nullptr, nullptr, nullptr);

  const auto interaction = WorkspaceShellTestAccess::ActiveMergeInteractionLayout(shell);
  const float y = interaction.result.text.first_line_y +
                  interaction.result.text.line_height * 0.5f;
  const float start_x = interaction.result.text.text_x +
                        interaction.result.text.char_width * 0.1f;
  const float end_x = interaction.result.text.text_x +
                      interaction.result.text.char_width * 3.1f;

  Expect(SendMouseDown(shell, start_x, y, SDL_BUTTON_LEFT),
         "pressing inside the merge result pane should start selection");
  Expect(SendMouseMotion(shell, end_x, y, SDL_BUTTON_LMASK),
         "dragging inside the merge result pane should update selection");
  Expect(SendMouseUp(shell, end_x, y, SDL_BUTTON_LEFT),
         "releasing after a merge result drag should be handled");
  Expect(WorkspaceShellTestAccess::ActiveMergeHasSelection(shell),
         "dragging across merge result text should create a selection");
  Expect(WorkspaceShellTestAccess::ActiveMergeSelectedText(shell) == "top",
         "merge result drag selection should capture the dragged text");
}

void TestWorkspaceShellMergeDividerDragUpdatesPaneFractions() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  OpenMergeHoverFixture(shell, temp_dir, nullptr, nullptr, nullptr, nullptr);

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  const float before_fraction = merge.left_divider_fraction;
  const auto divider_rects = WorkspaceShellTestAccess::MergeDividerRects(shell);
  const float start_x = divider_rects[0].x + divider_rects[0].w * 0.5f;
  const float y = divider_rects[0].y + divider_rects[0].h * 0.5f;

  Expect(SendMouseDown(shell, start_x, y, SDL_BUTTON_LEFT),
         "pressing the merge divider should be handled");
  Expect(SendMouseMotion(shell, start_x + 80.0f, y,
                                                     SDL_BUTTON_LMASK),
         "dragging the merge divider should be handled");
  Expect(SendMouseUp(shell, start_x + 80.0f, y,
                                                       SDL_BUTTON_LEFT),
         "releasing the merge divider drag should be handled");
  Expect(merge.left_divider_fraction > before_fraction,
         "dragging the merge divider should update the left pane fraction");
}

void TestWorkspaceShellMergeWheelScrollsRows() {
  TemporaryDirectory temp_dir;
  WorkspaceShell shell;
  OpenTallMergeMouseFixture(shell, temp_dir);

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  const auto surface = WorkspaceShellTestAccess::ActiveMergeSurfaceLayout(shell);
  Expect(merge.result_viewport.lines().size() > static_cast<std::size_t>(surface.visible_rows),
         "merge wheel fixture should overflow the viewport");

  const int before_scroll = merge.scroll_row;
  const SDL_FRect result_rect = WorkspaceShellTestAccess::ActiveMergeResultRect(shell);
  const float wheel_x = result_rect.x + 24.0f;
  const float wheel_y = result_rect.y + 24.0f;
  Expect(SendMouseWheel(shell, wheel_x, wheel_y, -1),
         "scrolling the merge result pane should be handled");
  Expect(merge.scroll_row > before_scroll,
         "scrolling the merge result pane should advance the visible row");
}

void TestWorkspaceShellMergeToolbarButtonsNavigateConflicts() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  WriteFile(base, "top\nbase a\nmid\nbase b\nbottom\n");
  WriteFile(incoming, "top\nincoming a\nmid\nincoming b\nbottom\n");
  WriteFile(current, "top\ncurrent a\nmid\ncurrent b\nbottom\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, 1280, 720);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, current),
         "merge editor should open for toolbar navigation fixture");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(merge.conflicts.size() == 2,
         "toolbar navigation fixture should expose two merge conflicts");
  Expect(merge.selected_hunk == 0,
         "toolbar navigation fixture should start on the first conflict");

  const auto nav_rects = WorkspaceShellTestAccess::MergeToolbarNavigationRects(shell);
  const float next_x = nav_rects[1].x + nav_rects[1].w * 0.5f;
  const float next_y = nav_rects[1].y + nav_rects[1].h * 0.5f;
  Expect(SendMouseDown(shell, next_x, next_y, SDL_BUTTON_LEFT),
         "clicking the merge next button should be handled");
  Expect(merge.selected_hunk == 1,
         "clicking the merge next button should select the next conflict");

  const float prev_x = nav_rects[0].x + nav_rects[0].w * 0.5f;
  const float prev_y = nav_rects[0].y + nav_rects[0].h * 0.5f;
  Expect(SendMouseDown(shell, prev_x, prev_y, SDL_BUTTON_LEFT),
         "clicking the merge prev button should be handled");
  Expect(merge.selected_hunk == 0,
         "clicking the merge prev button should select the previous conflict");
}

void TestWorkspaceShellRestoreSessionPreservesMergeNavigationState() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path base = root / "base.txt";
  const std::filesystem::path incoming = root / "incoming.txt";
  const std::filesystem::path current = root / "current.txt";
  const std::filesystem::path output = root / "result.txt";
  WriteFile(base, "alpha\nshared\n");
  WriteFile(incoming, "incoming\nshared\n");
  WriteFile(current, "current\nshared\n");
  WriteFile(output, "alpha\nshared\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedEnvVar scoped_xdg_state_home("XDG_STATE_HOME", xdg_state_home.string());
  ScopedEnvVar scoped_xdg_config_home("XDG_CONFIG_HOME", xdg_config_home.string());

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  Expect(WorkspaceShellTestAccess::OpenMergeEditor(shell, base, incoming, current, output),
         "merge editor should open for session-restore fixture");

  auto& merge = WorkspaceShellTestAccess::ActiveMerge(shell);
  Expect(!merge.model.hunks.empty() && merge.model.hunks.front().conflict,
         "merge session fixture should start with a conflict hunk");
  merge.model.hunks.front().choice = MergeChoice::Current;
  WorkspaceShellTestAccess::RefreshMergeTabDerivedState(shell);
  merge.selected_hunk = 0;
  merge.scroll_row = 2;
  merge.horizontal_scroll = 5;
  merge.left_divider_fraction = 0.24f;
  merge.right_divider_fraction = 0.74f;
  merge.result_viewport.SetScrollLine(2);
  WorkspaceShellTestAccess::SaveSessionState(shell);

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "merge session restore should succeed");
  Expect(WorkspaceShellTestAccess::OpenTabs(restored).size() == 1,
         "merge session restore should reopen the merge tab");

  const auto& rebuilt = WorkspaceShellTestAccess::ActiveMerge(restored);
  Expect(rebuilt.base_path == base.lexically_normal(),
         "merge session restore should preserve the base path");
  Expect(rebuilt.incoming_path == incoming.lexically_normal(),
         "merge session restore should preserve the incoming path");
  Expect(rebuilt.current_path == current.lexically_normal(),
         "merge session restore should preserve the current path");
  Expect(rebuilt.output_path == output.lexically_normal(),
         "merge session restore should preserve the output path");
  Expect(rebuilt.selected_hunk == 0,
         "merge session restore should preserve the selected conflict");
  Expect(rebuilt.scroll_row == 2,
         "merge session restore should preserve vertical scroll");
  Expect(rebuilt.horizontal_scroll == 5,
         "merge session restore should preserve horizontal scroll");
  Expect(std::fabs(rebuilt.left_divider_fraction - 0.24f) < 0.0001f,
         "merge session restore should preserve the left divider fraction");
  Expect(std::fabs(rebuilt.right_divider_fraction - 0.74f) < 0.0001f,
         "merge session restore should preserve the right divider fraction");
  Expect(!rebuilt.model.hunks.empty() && rebuilt.model.hunks.front().choice == MergeChoice::Current,
         "merge session restore should preserve the committed conflict choice metadata");
}

void TestWorkspaceShellRestoreSessionDefersInactiveCleanEditorTabs() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::vector<std::filesystem::path> files;
  for (int i = 0; i < 20; ++i) {
    const std::filesystem::path path = root / ("file_" + std::to_string(i) + ".txt");
    WriteFile(path, "line 1\nline 2\nline 3\n");
    files.push_back(path);
  }

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedEnvVar scoped_xdg_state_home("XDG_STATE_HOME", xdg_state_home.string());
  ScopedEnvVar scoped_xdg_config_home("XDG_CONFIG_HOME", xdg_config_home.string());

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, files.front());
  for (std::size_t i = 1; i < files.size(); ++i) {
    Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, files[i]),
           "session defer fixture should open each tab");
  }
  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  WorkspaceShellTestAccess::SaveSessionState(shell);

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "session restore should succeed for deferred-tab fixture");

  const auto& tabs = WorkspaceShellTestAccess::OpenTabs(restored);
  Expect(tabs.size() == files.size(),
         "session restore should preserve the total tab count");
  std::size_t deferred_count = 0;
  for (std::size_t i = 0; i < tabs.size(); ++i) {
    if (tabs[i].deferred_handle.has_value()) {
      ++deferred_count;
      if (i != 0) {
        Expect(!tabs[i].path.empty(),
               "deferred tab entries should preserve displayable paths in tab-strip state");
      }
    }
  }
  Expect(deferred_count >= 1,
         "inactive clean editor tabs should restore as deferred handles");
}

void TestWorkspaceShellDeferredTabHydrationPreservesCursorAndScroll() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path alpha = root / "alpha.txt";
  const std::filesystem::path beta = root / "beta.txt";
  WriteFile(alpha, "a1\na2\na3\na4\na5\na6\na7\na8\na9\na10\n");
  WriteFile(beta, "b1\nb2\nb3\nb4\nb5\nb6\nb7\nb8\nb9\nb10\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_state_home = temp_dir.path() / "xdg-state-home";
  const std::filesystem::path xdg_config_home = temp_dir.path() / "xdg-config-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_state_home);
  std::filesystem::create_directories(xdg_config_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedEnvVar scoped_xdg_state_home("XDG_STATE_HOME", xdg_state_home.string());
  ScopedEnvVar scoped_xdg_config_home("XDG_CONFIG_HOME", xdg_config_home.string());

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenFile(shell, alpha);
  Expect(WorkspaceShellTestAccess::OpenFileInNewTab(shell, beta),
         "deferred hydration fixture should open the secondary tab");
  WorkspaceShellTestAccess::ActiveEditor(shell).MoveCursorTo(6, 1);
  WorkspaceShellTestAccess::ActiveEditor(shell).SetScrollLine(4);
  WorkspaceShellTestAccess::ActiveEditor(shell).SetHorizontalScroll(0);
  WorkspaceShellTestAccess::ActivateTab(shell, 0);
  WorkspaceShellTestAccess::SaveSessionState(shell);

  WorkspaceShell restored;
  WorkspaceShellTestAccess::SetProjectRoot(restored, root);
  Expect(WorkspaceShellTestAccess::RestoreSessionState(restored),
         "deferred hydration restore should succeed");
  const auto& tabs_before = WorkspaceShellTestAccess::OpenTabs(restored);
  Expect(tabs_before.size() == 2, "deferred hydration restore should reopen both tabs");
  Expect(tabs_before[1].deferred_handle.has_value(),
         "inactive secondary tab should restore as deferred");
  Expect(tabs_before[1].deferred_handle->cursor_line == 6 &&
             tabs_before[1].deferred_handle->cursor_column == 1 &&
             tabs_before[1].deferred_handle->scroll_line == 4,
         "deferred handle should persist cursor/scroll metadata");

  WorkspaceShellTestAccess::ActivateTab(restored, 1);
  const auto& tabs_after = WorkspaceShellTestAccess::OpenTabs(restored);
  Expect(!tabs_after[1].deferred_handle.has_value() && tabs_after[1].editor_state.has_value(),
         "activating a deferred tab should hydrate and clear the deferred handle");
  const auto& hydrated = WorkspaceShellTestAccess::ActiveEditor(restored);
  Expect(hydrated.path() == beta.lexically_normal(),
         "hydrated deferred tab should open the same path");
  Expect(hydrated.cursor_line() == 6 && hydrated.cursor_column() == 1 &&
             hydrated.scroll_line() == 4,
         "hydrated deferred tab should restore cursor and scroll metadata");
}

}  // namespace

void RegisterWorkspaceShellSessionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/RestoreSessionPreservesBranchCompareState",
          TestWorkspaceShellRestoreSessionPreservesBranchCompareState);
  AddTest(tests, "WorkspaceShell/ImportsLegacyUserConfigAndArchivesSource",
          TestWorkspaceShellImportsLegacyUserConfigAndArchivesSource);
  AddTest(tests, "WorkspaceShell/ImportsLegacyProjectStateAndArchivesSource",
          TestWorkspaceShellImportsLegacyProjectStateAndArchivesSource);
  AddTest(tests, "WorkspaceShell/ImportsLegacyWorkspaceSessionAndArchivesSource",
          TestWorkspaceShellImportsLegacyWorkspaceSessionAndArchivesSource);
  AddTest(tests, "WorkspaceShell/ImportsLegacyChatConversationsAndArchivesSource",
          TestWorkspaceShellImportsLegacyChatConversationsAndArchivesSource);
  AddTest(tests, "WorkspaceShell/RestoreSessionPreservesRenamedWorkingTreeCompareState",
          TestWorkspaceShellRestoreSessionPreservesRenamedWorkingTreeCompareState);
  AddTest(tests, "WorkspaceShell/ReopenFileReloadsCleanEditorTab",
          TestWorkspaceShellReopenFileReloadsCleanEditorTab);
  AddTest(tests, "WorkspaceShell/RefreshReloadsCleanOpenEditorBuffers",
          TestWorkspaceShellRefreshReloadsCleanOpenEditorBuffers);
  AddTest(tests, "WorkspaceShell/RestoreSessionPreservesDirtyEditorBufferContent",
          TestWorkspaceShellRestoreSessionPreservesDirtyEditorBufferContent);
  AddTest(tests, "WorkspaceShell/RestoreSessionPreservesDirtyUntitledBufferContent",
          TestWorkspaceShellRestoreSessionPreservesDirtyUntitledBufferContent);
  AddTest(tests, "WorkspaceShell/QuitShutdownPersistsDirtyEditorBuffers",
          TestWorkspaceShellQuitShutdownPersistsDirtyEditorBuffers);
  AddTest(tests, "WorkspaceShell/ReopenWorkingTreeComparisonRefreshesExistingTab",
          TestWorkspaceShellReopenWorkingTreeComparisonRefreshesExistingTab);
  AddTest(tests, "WorkspaceShell/CompareSyntaxTokensAreDeferredUntilRender",
          TestWorkspaceShellCompareSyntaxTokensAreDeferredUntilRender);
  AddTest(tests, "WorkspaceShell/MergeSyntaxTokensAreDeferredUntilRender",
          TestWorkspaceShellMergeSyntaxTokensAreDeferredUntilRender);
  AddTest(tests, "WorkspaceShell/ReopenMergeEditorRefreshesCleanTabFromOutput",
          TestWorkspaceShellReopenMergeEditorRefreshesCleanTabFromOutput);
  AddTest(tests, "WorkspaceShell/MergeEditorUsesWorkingTreeConflictMarkers",
          TestWorkspaceShellMergeEditorUsesWorkingTreeConflictMarkers);
  AddTest(tests, "WorkspaceShell/MergeEditorParsesLargeWorkingTreeConflictBlock",
          TestWorkspaceShellMergeEditorParsesLargeWorkingTreeConflictBlock);
  AddTest(tests, "WorkspaceShell/MergeChoicePreservesManualEditsAroundConflicts",
          TestWorkspaceShellMergeChoicePreservesManualEditsAroundConflicts);
  AddTest(tests, "WorkspaceShell/MergeConflictTrackingShiftsAfterInsertion",
          TestWorkspaceShellMergeConflictTrackingShiftsAfterInsertion);
  AddTest(tests, "WorkspaceShell/MergeHoverPreviewDoesNotCommitState",
          TestWorkspaceShellMergeHoverPreviewDoesNotCommitState);
  AddTest(tests, "WorkspaceShell/MergeHoverPrefersIncomingAcceptButton",
          TestWorkspaceShellMergeHoverPrefersIncomingAcceptButton);
  AddTest(tests, "WorkspaceShell/MergeHoverPrefersResultActionButton",
          TestWorkspaceShellMergeHoverPrefersResultActionButton);
  AddTest(tests, "WorkspaceShell/MergeResultDragSelectionTracksPointer",
          TestWorkspaceShellMergeResultDragSelectionTracksPointer);
  AddTest(tests, "WorkspaceShell/MergeDividerDragUpdatesPaneFractions",
          TestWorkspaceShellMergeDividerDragUpdatesPaneFractions);
  AddTest(tests, "WorkspaceShell/MergeWheelScrollsRows",
          TestWorkspaceShellMergeWheelScrollsRows);
  AddTest(tests, "WorkspaceShell/MergeToolbarButtonsNavigateConflicts",
          TestWorkspaceShellMergeToolbarButtonsNavigateConflicts);
  AddTest(tests, "WorkspaceShell/MergeHorizontalNavigationInvalidatesResultPane",
          TestWorkspaceShellMergeHorizontalNavigationInvalidatesResultPane);
  AddTest(tests, "WorkspaceShell/MoveMergeSelectionInvalidatesConflictBand",
          TestWorkspaceShellMoveMergeSelectionInvalidatesConflictBand);
  AddTest(tests, "WorkspaceShell/RestoreSessionPreservesMergeNavigationState",
          TestWorkspaceShellRestoreSessionPreservesMergeNavigationState);
  AddTest(tests, "WorkspaceShell/RestoreSessionDefersInactiveCleanEditorTabs",
          TestWorkspaceShellRestoreSessionDefersInactiveCleanEditorTabs);
  AddTest(tests, "WorkspaceShell/DeferredTabHydrationPreservesCursorAndScroll",
          TestWorkspaceShellDeferredTabHydrationPreservesCursorAndScroll);
  AddTest(tests, "WorkspaceShell/RestoreWorkspaceSessionAcrossProjects",
          TestWorkspaceShellRestoreWorkspaceSessionAcrossProjects);
  AddTest(tests, "WorkspaceShell/ShutdownPreservesDistinctWorkspaceProjectRoots",
          TestWorkspaceShellShutdownPreservesDistinctWorkspaceProjectRoots);
}

}  // namespace microide::tests
