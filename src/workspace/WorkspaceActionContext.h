#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/WorkspaceActionRequests.h"
#include "workspace/WorkspaceActionTypes.h"
#include "workspace/WorkspaceSidebarRegistry.h"

namespace microide::workspace {

class WorkspaceShell;

enum class ProjectOpenPickerResult {
  Launched,
  AlreadyOpen,
  Unavailable,
};

class WorkspaceActionContext {
 public:
  explicit WorkspaceActionContext(WorkspaceShell& shell);

  void PrepareForAction(ActionSource source);
  bool RejectAction(ActionSource source, std::string feedback);

  SidebarViewRequest ParseSidebarViewRequest(const std::vector<std::string>& args) const;

  bool HasProjectRoot() const;
  bool HasActiveProject() const;
  std::size_t ProjectCount() const;
  std::size_t ActiveProjectIndex() const;
  std::filesystem::path ProjectRoot() const;
  bool OpenProject(const std::filesystem::path& project_root,
                   bool restore_persistence,
                   bool log_feedback);
  void RequestCloseProject(std::size_t index);
  bool SwitchProject(std::size_t index, bool log_feedback);
  ProjectOpenPickerResult OpenNativeProjectPicker();

  bool SidebarVisible() const;
  bool SidebarTemporary() const;
  std::string_view SidebarViewId() const;
  SidebarMode ActiveSidebarMode() const;
  void ShowSidebarSurface();
  void ToggleSidebar();
  void CloseSidebar();
  bool ShowSidebarView(const SidebarViewInfo& view,
                       const std::filesystem::path& root,
                       const std::string& query);
  bool ToggleSidebarView(const SidebarViewInfo& view,
                         const std::filesystem::path& root,
                         const std::string& query);
  float CurrentWindowWidth() const;
  void SetSidebarWidth(float width);
  void RefreshProjectFiles();
  void ReloadCleanOpenBuffersFromDisk();
  std::filesystem::path TreeMutationBasePath(ActionSource source) const;
  std::filesystem::path ResolveTreeActionPath(ActionSource source) const;
  void OpenCreatePathPrompt(bool directory, const std::filesystem::path& base_path);
  void OpenRenamePathPrompt(const std::filesystem::path& path);
  void OpenDeletePathPrompt(const std::filesystem::path& path);
  bool WriteClipboardText(std::string_view text) const;
  bool WritePrimarySelectionText(std::string_view text) const;

  void OpenTerminal(std::string command);
  void ShowFileFinderWithQuery(std::string query);
  void ShowFileFinder();
  bool OverlayVisible() const;
  void DismissOverlay();
  void ShowProjectSearchSidebar(std::string query);
  bool ActiveTabIsCompare() const;
  bool ActiveTabIsMerge() const;
  void OpenBufferSearch(std::string query);
  void OpenBufferReplace();
  std::filesystem::path ResolveComparePath(const std::filesystem::path& requested_path,
                                           ActionSource source) const;
  void OpenComparePickerForPath(const std::filesystem::path& path,
                                const std::string& commit_spec);
  void OpenHeadComparison(const std::filesystem::path& path);
  void OpenMergeEditor(const std::filesystem::path& base_path,
                       const std::filesystem::path& incoming_path,
                       const std::filesystem::path& current_path,
                       const std::filesystem::path& output_path);

  bool OpenPath(const std::filesystem::path& path, std::string* error_message);
  bool OpenPathInNewTab(const std::filesystem::path& path);
  bool OpenUntitledTab();
  std::optional<std::size_t> FindTabIndexBySpecifier(std::string_view specifier,
                                                     std::string* error_message) const;
  void ActivateTab(std::size_t index);
  bool HasOpenTabs() const;
  std::size_t OpenTabCount() const;
  std::size_t ActiveTabIndex() const;
  void MoveActiveTabTo(std::size_t index);
  void ReopenActiveTab();
  bool SaveTab(std::size_t index);
  void ResetCaretBlink();
  bool OpenVerticalSplitPath(const std::filesystem::path& path, std::string* error_message);
  void SplitActiveEditorVertically();
  void UnsplitActiveEditor();
  void CycleEditorSplit(int delta);
  void ActivateOrderedEditorSplit(std::size_t index);
  std::size_t ActiveEditorSplitCount() const;
  void RequestCloseTab(std::size_t index);
  void RequestCloseTabs(std::vector<std::size_t> indices);
  void CloseAllTabs();

  bool ExecuteLineNavigation(const LineNavigationRequest& request, bool relative);
  void SelectAll();
  void Undo();
  void Redo();
  std::string CopySelectionText() const;
  std::optional<std::string> LastTerminalCommandText() const;
  std::optional<std::string> SelectionTextWithContext() const;
  void CutSelection();
  void PasteClipboard();

  void RefreshAvailableColorschemeNames();
  void ApplyColorscheme(std::string_view name);
  void SetTabSize(std::size_t value);
  void SetIndentWidth(std::size_t value);
  float UiScale() const;
  void ApplyUiScale(float scale);
  void SetSoftTabs(bool enabled);
  bool Focus(FocusRequestTarget target);
  void OpenCommandPrompt(std::string input = {});
  bool PluginRuntimeEnabled() const;
  void ReloadPluginsWithFeedback();
  void RequestQuit();

 private:
  WorkspaceShell& shell_;
};

}  // namespace microide::workspace
