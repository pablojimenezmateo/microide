#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "compare/CompareModel.h"
#include "editor/EditorViewRenderer.h"
#include "editor/TextViewport.h"
#include "project/DirectoryTree.h"
#include "project/FileFinder.h"
#include "project/FileIndex.h"
#include "project/GitCompareService.h"
#include "project/ProjectSearchService.h"
#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "terminal/TerminalSession.h"

namespace microide::workspace {

class WorkspaceShell {
 public:
  WorkspaceShell() = default;

  bool Initialize(const std::filesystem::path& project_root);
  void Shutdown();
  bool HandleEvent(const SDL_Event& event);
  void Render(SDL_Renderer* renderer, int width, int height);
  std::optional<Uint32> NextAnimationDelayMs() const;
  void RequestQuit();
  bool ConsumeQuitRequested();

 private:
  enum class FocusTarget {
    Sidebar,
    Editor,
    Panel,
    Overlay,
  };

  enum class SidebarMode {
    None,
    Tree,
    Search,
  };

  enum class OverlayMode {
    FileFinder,
    BufferSearch,
    BufferReplace,
    ProjectSearch,
    CommitPicker,
  };

  enum class BufferSearchField {
    Search,
    Replace,
  };

  enum class BottomPanelMode {
    Logs,
    Terminal,
  };

  enum class TextInputSurface {
    None,
    Editor,
    Command,
    FileFinder,
    BufferSearch,
    BufferReplaceSearch,
    BufferReplaceReplace,
    ProjectSearchOverlay,
    CommitPicker,
    SidebarSearchQuery,
    SidebarSearchReplace,
    Terminal,
  };

  enum class EditorSplitOrientation {
    None,
    Vertical,
    Horizontal,
  };

  enum class ProjectSearchEditField {
    Query,
    Replace,
  };

  enum class DragTarget {
    None,
    SidebarDivider,
    BottomPanelDivider,
    SidebarScrollbar,
    BottomPanelScrollbar,
    OverlayScrollbar,
    EditorVerticalScrollbar,
    EditorHorizontalScrollbar,
    EditorSplitDivider,
    CompareScrollbar,
  };

  enum class CursorKind {
    Default,
    Text,
    Pointer,
    EwResize,
    NsResize,
  };

  struct CompareTabState {
    std::filesystem::path path;
    std::string title;
    std::string commit_hash;
    std::string left_label;
    std::string right_label;
    editor::SyntaxState left_initial_syntax_state;
    editor::SyntaxState right_initial_syntax_state;
    compare::CompareModel model;
    std::vector<std::vector<editor::SyntaxTokenKind>> left_tokens_by_row;
    std::vector<std::vector<editor::SyntaxTokenKind>> right_tokens_by_row;
    std::size_t selected_row = 0;
    int scroll_row = 0;
  };

  struct TabEntry {
    enum class Kind {
      Editor,
      Compare,
    };

    struct EditorTabState {
      struct EditorViewState {
        std::size_t leaf_id = 0;
        editor::TextViewport viewport;
      };

      struct EditorSplitNode {
        std::size_t leaf_id = 0;
        EditorSplitOrientation orientation = EditorSplitOrientation::None;
        float size_fraction = 1.0f;
        std::vector<std::unique_ptr<EditorSplitNode>> children;

        bool IsLeaf() const { return children.empty(); }
      };

      std::vector<EditorViewState> views;
      std::size_t active_leaf_id = 0;
      std::size_t next_leaf_id = 1;
      std::unique_ptr<EditorSplitNode> split_root;
    };

    Kind kind = Kind::Editor;
    std::filesystem::path path;
    std::string title;
    std::optional<EditorTabState> editor_state;
    std::optional<CompareTabState> compare;
  };

  struct VisibleTab {
    std::size_t index = 0;
    SDL_FRect rect{};
    SDL_FRect close_rect{};
    bool active = false;
  };

  struct VisibleProjectTab {
    std::size_t index = 0;
    SDL_FRect rect{};
    SDL_FRect close_rect{};
    bool active = false;
  };

  struct VisibleTerminalTab {
    std::size_t index = 0;
    SDL_FRect rect{};
    SDL_FRect close_rect{};
    bool active = false;
  };

  struct EditorPaneLayout {
    std::size_t leaf_id = 0;
    SDL_FRect rect{};
    bool active = false;
  };

  struct EditorSplitDividerLayout {
    std::vector<std::size_t> node_path;
    std::size_t divider_index = 0;
    SDL_FRect rect{};
  };

  struct EditorSplitSlot {
    TabEntry::EditorTabState::EditorSplitNode* parent = nullptr;
    std::size_t index = 0;
    std::unique_ptr<TabEntry::EditorTabState::EditorSplitNode>* slot = nullptr;
  };

  struct DirtyPromptState {
    enum class Kind {
      CloseTab,
      CloseProject,
      Quit,
    };

    Kind kind = Kind::CloseTab;
    std::size_t tab_index = 0;
    std::size_t project_index = 0;
    std::vector<std::size_t> dirty_tabs;
    std::size_t dirty_count = 0;
    int selected_action = 0;
  };

  struct TerminalSelectionPosition {
    std::size_t row = 0;
    std::size_t column = 0;
  };

  struct TerminalTabState {
    terminal::TerminalSession session;
    int scroll_row = 0;
    bool follow_tail = true;
    bool mouse_selecting = false;
    std::optional<TerminalSelectionPosition> selection_anchor;
    std::optional<TerminalSelectionPosition> selection_head;
  };

  struct TextCompositionState {
    TextInputSurface surface = TextInputSurface::None;
    std::string text;
    int start = -1;
    int length = -1;
  };

  struct EditorPreferences {
    std::size_t tab_size = 4;
    std::size_t indent_width = 4;
    bool soft_tabs = false;
  };

  struct ProjectWorkspaceState {
    std::filesystem::path root;
    project::DirectoryTree directory_tree;
    project::FileIndex file_index;
    project::FileFinder file_finder;
    editor::TextViewport text_viewport;
    std::vector<TabEntry> open_tabs;
    std::size_t active_tab_index = 0;
    int tab_scroll_index = 0;
    bool sidebar_visible = true;
    SidebarMode sidebar_mode = SidebarMode::Tree;
    SidebarMode sidebar_prev_mode = SidebarMode::None;
    bool sidebar_temporary = false;
    bool bottom_panel_visible = false;
    BottomPanelMode bottom_panel_mode = BottomPanelMode::Logs;
    bool overlay_visible = false;
    OverlayMode overlay_mode = OverlayMode::FileFinder;
    BufferSearchField buffer_search_field = BufferSearchField::Search;
    bool command_mode = false;
    FocusTarget focus = FocusTarget::Sidebar;
    float sidebar_width = 288.0f;
    float bottom_panel_height = 184.0f;
    int sidebar_scroll_row = 0;
    int bottom_panel_scroll_row = 0;
    int overlay_scroll_row = 0;
    bool bottom_panel_follow_tail = true;
    std::vector<std::unique_ptr<TerminalTabState>> terminal_tabs;
    std::size_t active_terminal_tab_index = 0;
    std::string buffer_search_query;
    std::string buffer_replace_text;
    std::vector<editor::SelectionRange> buffer_search_matches;
    std::size_t buffer_search_selected_index = 0;
    std::string project_search_query;
    std::string project_search_edit_buffer;
    bool project_search_editing = false;
    ProjectSearchEditField project_search_edit_field = ProjectSearchEditField::Query;
    std::string project_replace_text;
    std::vector<project::ProjectSearchResult> project_search_results;
    std::size_t project_search_selected_index = 0;
    bool project_search_running = false;
    std::string project_search_error;
    std::filesystem::path compare_picker_path;
    std::string compare_picker_query;
    std::vector<project::GitCommitEntry> compare_picker_commits;
    std::vector<project::GitCommitEntry> compare_picker_matches;
    std::size_t compare_picker_selected_index = 0;
    std::string command_input;
    std::vector<std::string> command_history;
    std::optional<std::size_t> command_history_index;
    std::string command_history_pending_input;
    std::string command_completion_feedback;
    std::vector<std::string> log_messages;
    std::string active_colorscheme_name = "default";
    EditorPreferences editor_preferences;
  };

  static char KeycodeToAscii(SDL_Keycode keycode, SDL_Keymod modifiers);
  std::filesystem::path ResolveProjectRootInput(const std::filesystem::path& project_root) const;
  bool SetProjectRoot(const std::filesystem::path& project_root);
  static bool ConfigureProjectState(ProjectWorkspaceState& state,
                                    const std::filesystem::path& project_root);
  void RebindProjectState(ProjectWorkspaceState& state);
  void ResetProjectScopedState(bool show_welcome);
  void SetWelcomePlaceholder();
  bool InitializeCurrentProject(const std::filesystem::path& project_root,
                                bool restore_persistence,
                                bool log_feedback);
  void StoreCurrentProjectState(ProjectWorkspaceState& state);
  void LoadProjectState(ProjectWorkspaceState& state);
  bool OpenProjectTab(const std::filesystem::path& project_root,
                      bool restore_persistence,
                      bool log_feedback);
  bool SwitchProject(std::size_t index, bool log_feedback);
  void RequestCloseProject(std::size_t index);
  void CloseProject(std::size_t index);
  void ShowDirtyPromptForProject(std::size_t index);
  void ActivateTab(std::size_t index);
  void CloseTab(std::size_t index);
  void RequestCloseTab(std::size_t index);
  void SyncActiveEditorTab();
  bool SaveTab(std::size_t index);
  bool TabIsDirty(std::size_t index) const;
  std::string TabDisplayTitle(std::size_t index) const;
  std::vector<std::size_t> DirtyEditorTabIndices() const;
  static std::vector<std::size_t> DirtyEditorTabIndices(const ProjectWorkspaceState& state);
  std::vector<std::size_t> DirtyEditorTabIndicesForProject(std::size_t project_index) const;
  void ShowDirtyPromptForTab(std::size_t index);
  void ShowDirtyPromptForQuit();
  void DismissDirtyPrompt(bool restore_focus);
  void ConfirmDirtyPrompt();
  std::array<std::string, 3> DirtyPromptActionLabels() const;
  std::string DirtyPromptTitle() const;
  std::string DirtyPromptMessage() const;
  bool ActiveTabIsEditor() const;
  TabEntry::EditorTabState* ActiveEditorTab();
  const TabEntry::EditorTabState* ActiveEditorTab() const;
  static TabEntry::EditorTabState MakeEditorTabState(const editor::TextViewport& view);
  static std::unique_ptr<TabEntry::EditorTabState::EditorSplitNode> MakeEditorLeafNode(
      std::size_t leaf_id,
      float size_fraction = 1.0f);
  void SyncActiveEditorTabMetadata();
  bool ReplaceActiveEditorView(const editor::TextViewport& viewport);
  editor::TextViewport* FindEditorView(TabEntry::EditorTabState& editor_tab, std::size_t leaf_id);
  const editor::TextViewport* FindEditorView(const TabEntry::EditorTabState& editor_tab,
                                             std::size_t leaf_id) const;
  EditorSplitSlot FindEditorLeafSlot(TabEntry::EditorTabState& editor_tab, std::size_t leaf_id);
  TabEntry::EditorTabState::EditorSplitNode* FindEditorSplitNode(
      TabEntry::EditorTabState::EditorSplitNode* node,
      const std::vector<std::size_t>& path);
  const TabEntry::EditorTabState::EditorSplitNode* FindEditorSplitNode(
      const TabEntry::EditorTabState::EditorSplitNode* node,
      const std::vector<std::size_t>& path) const;
  void NormalizeEditorSplitNode(TabEntry::EditorTabState::EditorSplitNode& node);
  void NormalizeEditorSplitTree(TabEntry::EditorTabState& editor_tab);
  void CollectEditorLeafOrder(const TabEntry::EditorTabState::EditorSplitNode* node,
                              std::vector<std::size_t>& order) const;
  std::vector<std::size_t> EditorLeafOrder(const TabEntry::EditorTabState& editor_tab) const;
  void SetActiveEditorSplit(std::size_t index);
  bool ActivateOrderedEditorSplit(std::size_t order_index);
  bool SplitActiveEditor(EditorSplitOrientation orientation);
  bool UnsplitActiveEditor();
  bool CycleEditorSplit(int delta);
  void CollectEditorPaneLayouts(const TabEntry::EditorTabState& editor_tab,
                                const TabEntry::EditorTabState::EditorSplitNode* node,
                                const SDL_FRect& rect,
                                std::vector<EditorPaneLayout>& panes,
                                std::vector<EditorSplitDividerLayout>* dividers,
                                std::vector<std::size_t>* path) const;
  std::optional<SDL_FRect> ComputeEditorSplitNodeRect(const SDL_FRect& editor_surface,
                                                      const std::vector<std::size_t>& path) const;
  std::vector<EditorPaneLayout> ComputeEditorPaneLayouts(const SDL_FRect& editor_surface) const;
  std::vector<EditorSplitDividerLayout> ComputeEditorSplitDividerLayouts(
      const SDL_FRect& editor_surface) const;
  bool ActiveTabIsCompare() const;
  CompareTabState* ActiveCompareTab();
  const CompareTabState* ActiveCompareTab() const;
  int CompareVisibleRows(const SDL_FRect& rect) const;
  int CompareMaxScrollRow(const CompareTabState& compare_tab, int visible_rows) const;
  void ClampCompareScrollRow(CompareTabState& compare_tab, int visible_rows) const;
  void RevealActiveCompareSelection();
  std::string ActiveTabTitle() const;
  void OpenComparePicker();
  bool OpenComparePickerForPath(const std::filesystem::path& path,
                                std::string_view commit_spec = {});
  std::optional<TabEntry> BuildCompareTabEntry(const std::filesystem::path& path,
                                               const project::GitCommitEntry& commit,
                                               std::size_t selected_row = 0) const;
  void RefreshComparePicker();
  void MoveComparePickerSelection(int delta);
  void OpenSelectedCompareCommit();
  void OpenComparison(const project::GitCommitEntry& commit);
  void OpenWorkingFileFromCompare();
  void MoveCompareSelection(int delta);
  void JumpCompareHunk(int delta);
  void MoveFileFinderSelection(int delta);
  void RenderCompareSurface(SDL_Renderer* renderer, const SDL_FRect& rect);
  void ShowSidebarMode(SidebarMode mode, bool temporary = false);
  void ShowTreeSidebar(const std::filesystem::path& root = {});
  void ShowSearchSidebar(std::string query = {}, bool temporary = false);
  void CloseSidebar();
  void ToggleSidebar();
  void RestorePreviousSidebar();
  void RefreshProjectFiles();
  void OpenBufferSearch();
  void OpenBufferReplace();
  void OpenProjectSearch();
  void RefreshBufferSearch();
  void RefreshProjectSearch();
  void StopProjectSearch();
  void ConsumeProjectSearchUpdates();
  void ResetOverlayScroll();
  float OverlayListStartOffset() const;
  int OverlayVisibleRows(const SDL_FRect& overlay) const;
  std::size_t OverlayItemCount() const;
  std::size_t OverlaySelectedIndex() const;
  void SetOverlaySelectedIndex(std::size_t index);
  void ClampOverlayScrollRow(const SDL_FRect& overlay);
  void RevealOverlaySelection(const SDL_FRect& overlay);
  bool ActivateOverlaySelection();
  void BeginProjectSearchEdit(ProjectSearchEditField field);
  void CommitProjectSearchEdit();
  void CancelProjectSearchEdit();
  void ReplaceAllProjectSearchMatches();
  std::vector<int> BuildProjectSearchLineMap() const;
  int ProjectSearchLineForResult(std::size_t index) const;
  void MoveBufferSearchSelection(int delta);
  void MoveProjectSearchSelection(int delta);
  void ReplaceCurrentBufferSearchMatch();
  void ReplaceAllBufferSearchMatches();
  std::optional<editor::SelectionRange> ActiveBufferSearchMatch() const;
  bool ExecuteCommand(const std::string& command_line);
  void OpenTerminal(std::string command);
  bool ReopenActiveTab();
  std::filesystem::path ConfigStatePath() const;
  void RefreshAvailableColorschemeNames();
  bool ApplyColorscheme(std::string_view name, bool persist, bool log_feedback);
  bool RestoreConfigState();
  void SaveConfigState() const;
  std::filesystem::path SessionStatePath() const;
  bool RestoreSessionState();
  void SaveSessionState();
  std::filesystem::path WorkspaceSessionStatePath() const;
  bool RestoreWorkspaceSession();
  void SaveWorkspaceSession();
  void ApplyEditorPreferences(editor::TextViewport& viewport) const;
  void ApplyEditorPreferencesToAllTabs();
  void ResetCommandSessionState();
  void ClearCommandCompletionFeedback();
  void PushCommandHistory(std::string command_line);
  void StepCommandHistory(int delta);
  void CompleteCommandInput();
  std::string CommandPromptStatusText() const;
  bool HandleTextInput(const SDL_TextInputEvent& event);
  bool HandleTextEditing(const SDL_TextEditingEvent& event);
  bool HandleTerminalKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  TextInputSurface CurrentTextInputSurface() const;
  void SyncTextInputSurface(SDL_Window* window);
  bool CompositionConsumesKey(SDL_Keycode key, SDL_Keymod modifiers) const;
  TerminalTabState* ActiveTerminalTab();
  const TerminalTabState* ActiveTerminalTab() const;
  void CloseTerminalTab(std::size_t index);
  void ReapExitedTerminalTabs();
  void SetBottomPanelVisible(bool visible);
  bool BottomPanelShowsTerminal() const;
  int BottomPanelVisibleRows(float panel_height) const;
  int BottomPanelScrollRow(std::size_t line_count, int visible_rows) const;
  void SetBottomPanelScrollRow(int scroll_row, std::size_t line_count, int visible_rows);
  void ClearTerminalSelection();
  bool TerminalHasSelection() const;
  std::optional<TerminalSelectionPosition> TerminalSelectionPositionForPoint(
      int x,
      int y,
      const std::vector<terminal::TerminalLine>& lines) const;
  std::optional<TerminalSelectionPosition> TerminalViewportPositionForPoint(int x, int y) const;
  terminal::TerminalSession::MouseButton TerminalMouseButtonForSdl(Uint8 button) const;
  std::string SelectedTerminalText(const std::vector<terminal::TerminalLine>& lines) const;
  bool TerminalCellSelected(std::size_t row, std::size_t column) const;
  std::string BottomPanelHeaderLabel() const;
  void ResizeTerminalToPanel(const SDL_FRect& panel_rect);
  void LogMessage(std::string message);
  bool OpenUntitledTab();
  bool OpenFileInNewTab(const std::filesystem::path& path);
  bool MoveActiveTabTo(std::size_t index);
  std::optional<std::size_t> FindTabIndexBySpecifier(std::string_view specifier,
                                                     std::string* error_message = nullptr) const;
  void OpenFile(const std::filesystem::path& path);
  bool HandleMouseButtonDown(const SDL_Event& event);
  bool HandleMouseButtonUp(const SDL_Event& event);
  bool HandleMouseMotion(const SDL_Event& event);
  bool HandleMouseWheel(const SDL_Event& event);
  void DrawFilledRect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) const;
  void DrawRect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) const;
  float ProjectTabWidthForIndex(std::size_t index) const;
  void EnsureActiveProjectVisible();
  std::vector<VisibleProjectTab> ComputeVisibleProjectTabs(
      const SDL_FRect& project_tab_strip) const;
  float TabWidthForIndex(std::size_t index) const;
  void EnsureActiveTabVisible();
  std::vector<VisibleTab> ComputeVisibleTabs(const SDL_FRect& tab_strip) const;
  std::vector<VisibleTerminalTab> ComputeVisibleTerminalTabs(
      const SDL_FRect& panel_header) const;
  SDL_FRect BottomPanelTerminalNewTabRect(const SDL_FRect& panel_header) const;
  SDL_FRect ComputeOverlayRect(const SDL_FRect& editor_area) const;
  std::string BreadcrumbLabel() const;
  std::string ProjectLabel() const;
  std::string ProjectLabelForRoot(const std::filesystem::path& root) const;
  std::string ProjectTabDisplayTitle(std::size_t index) const;
  std::string TruncateLabel(std::string_view text, float max_width) const;
  void ResetCaretBlink();
  bool ShouldBlinkCaret() const;
  bool CaretVisibleNow() const;
  CursorKind CursorKindForPosition(float x, float y) const;
  SDL_Cursor* CursorHandle(CursorKind kind);
  void UpdateMouseCursor(float x, float y);

  render::Theme theme_ = render::MakeDefaultTheme();
  render::TextRenderer text_renderer_;
  editor::EditorViewRenderer editor_view_renderer_;
  std::vector<std::unique_ptr<ProjectWorkspaceState>> projects_;
  std::size_t active_project_index_ = 0;
  int project_tab_scroll_index_ = 0;
  std::filesystem::path project_root_;
  project::DirectoryTree directory_tree_;
  project::FileIndex file_index_;
  project::FileFinder file_finder_;
  editor::TextViewport text_viewport_;
  std::vector<TabEntry> open_tabs_;
  std::size_t active_tab_index_ = 0;
  int tab_scroll_index_ = 0;
  bool sidebar_visible_ = true;
  SidebarMode sidebar_mode_ = SidebarMode::Tree;
  SidebarMode sidebar_prev_mode_ = SidebarMode::None;
  bool sidebar_temporary_ = false;
  bool bottom_panel_visible_ = false;
  BottomPanelMode bottom_panel_mode_ = BottomPanelMode::Logs;
  bool overlay_visible_ = false;
  OverlayMode overlay_mode_ = OverlayMode::FileFinder;
  BufferSearchField buffer_search_field_ = BufferSearchField::Search;
  bool command_mode_ = false;
  bool mouse_selecting_ = false;
  DragTarget drag_target_ = DragTarget::None;
  float drag_scrollbar_offset_ = 0.0f;
  std::vector<std::size_t> drag_editor_split_path_;
  std::size_t drag_editor_split_divider_index_ = 0;
  FocusTarget focus_ = FocusTarget::Sidebar;
  float sidebar_width_ = 288.0f;
  float bottom_panel_height_ = 184.0f;
  int sidebar_scroll_row_ = 0;
  int bottom_panel_scroll_row_ = 0;
  int overlay_scroll_row_ = 0;
  bool bottom_panel_follow_tail_ = true;
  std::vector<std::unique_ptr<TerminalTabState>> terminal_tabs_;
  std::size_t active_terminal_tab_index_ = 0;
  int last_window_width_ = 0;
  int last_window_height_ = 0;
  std::string buffer_search_query_;
  std::string buffer_replace_text_;
  std::vector<editor::SelectionRange> buffer_search_matches_;
  std::size_t buffer_search_selected_index_ = 0;
  std::string project_search_query_;
  std::string project_search_edit_buffer_;
  bool project_search_editing_ = false;
  ProjectSearchEditField project_search_edit_field_ = ProjectSearchEditField::Query;
  std::string project_replace_text_;
  std::vector<project::ProjectSearchResult> project_search_results_;
  std::size_t project_search_selected_index_ = 0;
  bool project_search_running_ = false;
  std::string project_search_error_;
  std::uint64_t project_search_run_id_ = 0;
  Uint32 project_search_event_type_ = 0;
  Uint32 terminal_event_type_ = 0;
  project::ProjectSearchService project_search_service_;
  std::filesystem::path compare_picker_path_;
  std::string compare_picker_query_;
  std::vector<project::GitCommitEntry> compare_picker_commits_;
  std::vector<project::GitCommitEntry> compare_picker_matches_;
  std::size_t compare_picker_selected_index_ = 0;
  bool dirty_prompt_visible_ = false;
  FocusTarget dirty_prompt_previous_focus_ = FocusTarget::Editor;
  DirtyPromptState dirty_prompt_state_;
  bool quit_requested_ = false;
  std::string command_input_;
  std::vector<std::string> command_history_;
  std::optional<std::size_t> command_history_index_;
  std::string command_history_pending_input_;
  std::string command_completion_feedback_;
  std::vector<std::string> log_messages_;
  std::vector<std::string> available_colorscheme_names_;
  std::string active_colorscheme_name_ = "default";
  EditorPreferences editor_preferences_;
  TextInputSurface active_text_input_surface_ = TextInputSurface::None;
  TextCompositionState text_composition_;
  Uint64 caret_blink_epoch_ms_ = 0;
  CursorKind cursor_kind_ = CursorKind::Default;
  SDL_Cursor* text_cursor_ = nullptr;
  SDL_Cursor* pointer_cursor_ = nullptr;
  SDL_Cursor* ew_resize_cursor_ = nullptr;
  SDL_Cursor* ns_resize_cursor_ = nullptr;
  float last_mouse_x_ = 0.0f;
  float last_mouse_y_ = 0.0f;
  bool last_mouse_position_valid_ = false;
};

}  // namespace microide::workspace
