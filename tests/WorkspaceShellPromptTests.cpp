#include "TestSupport.h"

#include "workspace/WorkspaceShell.h"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace microide::workspace {

struct WorkspaceShellTestAccess {
  static void SetProjectRoot(WorkspaceShell& shell, const std::filesystem::path& root) {
    shell.project_root_ = root.lexically_normal();
    shell.directory_tree_.SetRoot(shell.project_root_);
    shell.file_index_.SetRoot(shell.project_root_);
    shell.file_finder_.SetIndex(&shell.file_index_);
    shell.sidebar_visible_ = true;
    shell.sidebar_mode_ = WorkspaceShell::SidebarMode::Tree;
    shell.focus_ = WorkspaceShell::FocusTarget::Sidebar;
  }

  static void OpenSingleEditorTab(WorkspaceShell& shell, const std::filesystem::path& path) {
    editor::TextViewport opened_view;
    if (!opened_view.OpenFile(path)) {
      throw std::runtime_error("failed to open editor fixture: " + path.string());
    }
    shell.ApplyEditorPreferences(opened_view);
    shell.text_viewport_ = opened_view;
    shell.open_tabs_.push_back(WorkspaceShell::TabEntry{
        .kind = WorkspaceShell::TabEntry::Kind::Editor,
        .path = path.lexically_normal(),
        .title = path.filename().string(),
        .editor_state = WorkspaceShell::MakeEditorTabState(opened_view),
        .compare = std::nullopt,
        .merge = std::nullopt,
    });
    shell.active_tab_index_ = 0;
    shell.focus_ = WorkspaceShell::FocusTarget::Editor;
  }

  static editor::TextViewport& ActiveEditor(WorkspaceShell& shell) { return shell.text_viewport_; }

  static void PrepareRenamePrompt(WorkspaceShell& shell,
                                  const std::filesystem::path& path,
                                  std::string input) {
    shell.OpenPromptSurface(WorkspaceShell::PromptSurfaceState::Action::RenamePath,
                            WorkspaceShell::PromptSurfaceState::Kind::TextInput, path,
                            std::move(input));
  }

  static void PrepareDeletePrompt(WorkspaceShell& shell, const std::filesystem::path& path) {
    shell.OpenPromptSurface(WorkspaceShell::PromptSurfaceState::Action::DeletePath,
                            WorkspaceShell::PromptSurfaceState::Kind::Confirm, path);
  }

  static void ConfirmPromptSurface(WorkspaceShell& shell) { shell.ConfirmPromptSurface(); }

  static void ConfirmDirtyPrompt(WorkspaceShell& shell, int selected_action) {
    shell.dirty_prompt_state_.selected_action = selected_action;
    shell.ConfirmDirtyPrompt();
  }

  static bool DirtyPromptVisible(const WorkspaceShell& shell) { return shell.dirty_prompt_visible_; }
  static bool PromptSurfaceVisible(const WorkspaceShell& shell) {
    return shell.prompt_surface_visible_;
  }
  static const std::vector<WorkspaceShell::TabEntry>& OpenTabs(const WorkspaceShell& shell) {
    return shell.open_tabs_;
  }
  static const std::string& StatusMessage(const WorkspaceShell& shell) { return shell.status_message_; }
};

}  // namespace microide::workspace

namespace microide::tests {
namespace {

using microide::workspace::WorkspaceShell;
using microide::workspace::WorkspaceShellTestAccess;

std::optional<std::filesystem::path> FirstRegularFileIn(const std::filesystem::path& directory) {
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
    if (error) {
      return std::nullopt;
    }
    if (entry.is_regular_file()) {
      return entry.path();
    }
  }
  return std::nullopt;
}

void TestWorkspaceShellRenamePromptSavesDirtyTabs() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::filesystem::create_directories(root);
  const std::filesystem::path source = root / "notes.txt";
  WriteFile(source, "base text\n");

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("edited ");

  WorkspaceShellTestAccess::PrepareRenamePrompt(shell, source, "renamed.txt");
  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);

  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "rename prompt should remain open while waiting on dirty confirmation");
  Expect(WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "rename should show a dirty confirmation instead of blocking");

  WorkspaceShellTestAccess::ConfirmDirtyPrompt(shell, 0);

  const std::filesystem::path renamed = root / "renamed.txt";
  Expect(!WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "rename save flow should close the dirty confirmation");
  Expect(!WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "rename save flow should close the rename prompt");
  Expect(!std::filesystem::exists(source), "rename save flow should remove the source path");
  Expect(std::filesystem::is_regular_file(renamed),
         "rename save flow should create the destination path");
  Expect(ReadFile(renamed) == "edited base text\n",
         "rename save flow should persist the dirty editor content before renaming");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).size() == 1,
         "rename save flow should keep the affected editor tab open");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).front().path == renamed.lexically_normal(),
         "rename save flow should retarget the editor tab path");
  Expect(WorkspaceShellTestAccess::ActiveEditor(shell).path() == renamed.lexically_normal(),
         "rename save flow should retarget the live editor viewport");
  Expect(!WorkspaceShellTestAccess::ActiveEditor(shell).dirty(),
         "rename save flow should clear the dirty flag after saving");
}

#if defined(__linux__) || defined(__APPLE__)
void TestWorkspaceShellDeletePromptDiscardsDirtyTabs() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  std::filesystem::create_directories(root);
  const std::filesystem::path source = root / "trash-me.txt";
  WriteFile(source, "original text\n");

  const std::filesystem::path home = temp_dir.path() / "home";
  const std::filesystem::path xdg_data_home = temp_dir.path() / "xdg-data-home";
  std::filesystem::create_directories(home);
  std::filesystem::create_directories(xdg_data_home);
  ScopedEnvVar scoped_home("HOME", home.string());
  ScopedEnvVar scoped_xdg_data_home("XDG_DATA_HOME", xdg_data_home.string());

  WorkspaceShell shell;
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
  WorkspaceShellTestAccess::ActiveEditor(shell).InsertText("discarded ");

  WorkspaceShellTestAccess::PrepareDeletePrompt(shell, source);
  WorkspaceShellTestAccess::ConfirmPromptSurface(shell);

  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "delete prompt should remain open while waiting on dirty confirmation");
  Expect(WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "delete should show a dirty confirmation instead of blocking");

  WorkspaceShellTestAccess::ConfirmDirtyPrompt(shell, 1);

  Expect(!WorkspaceShellTestAccess::DirtyPromptVisible(shell),
         "delete discard flow should close the dirty confirmation");
  Expect(!WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "delete discard flow should close the delete prompt");
  Expect(!std::filesystem::exists(source),
         "delete discard flow should remove the project path");
  Expect(WorkspaceShellTestAccess::OpenTabs(shell).empty(),
         "delete discard flow should close affected tabs");
  Expect(WorkspaceShellTestAccess::StatusMessage(shell).find("Moved to trash: trash-me.txt") !=
             std::string::npos,
         "delete discard flow should report the trash move");

#if defined(__linux__)
  const std::filesystem::path trash_files = xdg_data_home / "Trash" / "files";
#else
  const std::filesystem::path trash_files = home / ".Trash";
#endif
  const auto trashed_file = FirstRegularFileIn(trash_files);
  Expect(trashed_file.has_value(), "delete discard flow should create a trash entry");
  Expect(ReadFile(*trashed_file) == "original text\n",
         "delete discard flow should discard unsaved editor changes before trashing");
}
#endif

}  // namespace

void RegisterWorkspaceShellPromptTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShell/RenamePromptSavesDirtyTabs",
          TestWorkspaceShellRenamePromptSavesDirtyTabs);
#if defined(__linux__) || defined(__APPLE__)
  AddTest(tests, "WorkspaceShell/DeletePromptDiscardsDirtyTabs",
          TestWorkspaceShellDeletePromptDiscardsDirtyTabs);
#endif
}

}  // namespace microide::tests
