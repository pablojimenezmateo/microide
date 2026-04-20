#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "compare/CompareModel.h"
#include "compare/MergeModel.h"
#include "editor/EditorViewRenderer.h"
#include "editor/TextViewport.h"
#include "plugin/PluginHost.h"
#include "project/DirectoryTree.h"
#include "project/FileFinder.h"
#include "project/GitBlameService.h"
#include "project/FileIndex.h"
#include "project/GitCompareService.h"
#include "project/ProjectSearchService.h"
#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "terminal/TerminalSession.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceActionTypes.h"
#include "workspace/WorkspaceInteractionState.h"
#include "workspace/WorkspaceMenuState.h"
#include "workspace/WorkspaceMenuRegistry.h"
#include "workspace/WorkspacePersistenceFormat.h"
#include "workspace/WorkspacePluginRuntime.h"
#include "workspace/WorkspaceProjectDialogState.h"
#include "workspace/WorkspaceProjectState.h"
#include "workspace/WorkspaceProjectSearchRuntime.h"
#include "workspace/WorkspacePromptState.h"
#include "workspace/WorkspaceSidebarState.h"
#include "workspace/WorkspaceTerminalSelection.h"

namespace microide::workspace {

class WorkspaceActionContext;
class ProjectCatalogCoordinator;
class PersistenceCoordinator;
class CommandPromptCoordinator;
class MenuCoordinator;
class KeyInputCoordinator;
class TextInputCoordinator;
class TabCoordinator;
class PathMutationCoordinator;
class LifecycleCoordinator;
class DirtyPromptCoordinator;
class CompareInteractionCoordinator;
class DiffTabCoordinator;
class SidebarCoordinator;
class ChromeMouseCoordinator;
class EditorMouseCoordinator;
class CompareMouseCoordinator;
class MergeMouseCoordinator;
class TabMouseCoordinator;
class SidebarMouseCoordinator;
class PanelMouseCoordinator;

class WorkspaceShell {
 public:
  using ActionId = workspace::ActionId;
  using ActionSource = workspace::ActionSource;
  using ActionSpec = workspace::ActionSpec;
  using MenuId = workspace::MenuId;
  using TreeContextTargetKind = workspace::TreeContextTargetKind;
  using MenuItemSpec = workspace::MenuItemSpec;
  using MenuSpec = workspace::MenuSpec;
  using SidebarMode = workspace::SidebarMode;
  using GitSidebarEntry = workspace::GitSidebarEntry;
  using GitSidebarLine = workspace::GitSidebarLine;
  using GitSidebarEntryActionLayout = workspace::GitSidebarEntryActionLayout;
  using ProblemsSidebarEntry = workspace::ProblemsSidebarEntry;
  using GitSidebarState = workspace::GitSidebarState;
  using ProblemsSidebarState = workspace::ProblemsSidebarState;
  using PluginSidebarState = workspace::PluginSidebarState;
  using SidebarState = workspace::SidebarState;

  enum class WindowAction {
    None,
    Minimize,
    ToggleMaximize,
    ToggleFullscreen,
  };

  enum class WindowControlButtonId {
    Minimize,
    Maximize,
    Close,
  };

  struct WindowChromeState {
    bool custom_enabled = false;
    bool maximized = false;
    bool fullscreen = false;

    [[nodiscard]] bool Expanded() const { return maximized || fullscreen; }
    [[nodiscard]] bool ResizableFrameEnabled() const {
      return custom_enabled && !Expanded();
    }
  };

  struct WindowPresentationState {
    int logical_width = 0;
    int logical_height = 0;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    WindowChromeState chrome{};
  };

  struct RenderInvalidation {
    bool full = false;
    std::vector<SDL_FRect> rects;

    [[nodiscard]] bool HasAnyRedraw() const { return full || !rects.empty(); }

    [[nodiscard]] std::optional<SDL_FRect> SingleRectIfOnlyOne() const {
      if (rects.size() != 1) {
        return std::nullopt;
      }
      return rects.front();
    }
  };

  struct EventResult {
    bool handled = false;
    RenderInvalidation redraw{};
  };

  WorkspaceShell();

  static std::vector<std::string> DocumentedCommandUsages();
  bool Initialize(const std::filesystem::path& project_root);
  void Shutdown();
  EventResult HandleEvent(const SDL_Event& event);
  EventResult HandleScheduledWake();
  void PrepareRenderFrame(SDL_Renderer* renderer, int width, int height);
  void Render(SDL_Renderer* renderer, int width, int height);
  void RenderPrepared(SDL_Renderer* renderer, int width, int height);
  std::optional<Uint32> NextAnimationDelayMs() const;
  bool ConsumePostRenderFullRedrawRequest();
  std::optional<SDL_FRect> CurrentCaretDirtyRect() const;
  RenderInvalidation ConsumePendingRenderInvalidation();
  void RequestQuit();
  bool ConsumeQuitRequested();
  float UiScale() const { return ui_scale_; }
  render::TextClipPadding PartialRedrawClipPadding() const;
  void SetWindowPresentationState(WindowPresentationState state);
  void SetDialogWindow(SDL_Window* window) { dialog_window_ = window; }
  SDL_HitTestResult WindowHitTest(float x, float y) const;
  bool WindowDragRegionContains(float x, float y) const;
  WindowAction ConsumeWindowAction();

 private:
  using FocusTarget = workspace::FocusTarget;
  using OverlayMode = workspace::OverlayMode;
  using BufferSearchField = workspace::BufferSearchField;
  using EditorSplitOrientation = workspace::EditorSplitOrientation;
  using ProjectSearchEditField = workspace::ProjectSearchEditField;
  using CompareTabState = workspace::CompareTabState;
  using MergeTrackedConflict = workspace::MergeTrackedConflict;
  using MergeHoverState = workspace::MergeHoverState;
  using MergeTabState = workspace::MergeTabState;
  using TabEntry = workspace::TabEntry;
  using TerminalSelectionPosition = workspace::TerminalSelectionPosition;
  using TerminalTabState = workspace::TerminalTabState;
  using EditorPreferences = workspace::EditorPreferences;
  using ProjectSurfaceState = workspace::ProjectSurfaceState;
  using CommandState = workspace::CommandState;
  using BufferSearchState = workspace::BufferSearchState;
  using ProjectSearchState = workspace::ProjectSearchState;
  using ComparePickerState = workspace::ComparePickerState;
  using OverlayWorkflowState = workspace::OverlayWorkflowState;
  using OverlayState = workspace::OverlayState;
  using PanelState = workspace::PanelState;
  using ProjectWorkspaceState = workspace::ProjectWorkspaceState;
  using ProjectCatalogState = workspace::ProjectCatalogState;
  using DragTarget = workspace::DragTarget;
  using TabDragKind = workspace::TabDragKind;
  using InteractionState = workspace::InteractionState;
  using TabDragState = workspace::TabDragState;
  using TreeContextMenuState = workspace::TreeContextMenuState;
  using MenuSurfaceState = workspace::MenuSurfaceState;
  using DirtyPromptState = workspace::DirtyPromptState;
  using PromptSurfaceState = workspace::PromptSurfaceState;
  using PromptState = workspace::PromptState;

  enum class TextInputSurface {
    None,
    Editor,
    Command,
    PromptInput,
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

  enum class CursorKind {
    Default,
    Text,
    Pointer,
    EwResize,
    NsResize,
  };

  struct CompareSurfaceLayout {
    float line_height = 14.0f;
    float gutter_width = 28.0f;
    float divider_width = 18.0f;
    float left_width = 0.0f;
    float right_width = 0.0f;
    float left_x = 0.0f;
    float center_x = 0.0f;
    float right_x = 0.0f;
    float header_y = 0.0f;
    float rows_y = 0.0f;
    int visible_rows = 1;
    std::size_t left_visible_columns = 1;
    std::size_t right_visible_columns = 1;
    std::size_t visible_columns = 1;
    bool show_vertical = false;
    bool show_horizontal = false;
  };

  struct TextInputVisual {
    TextInputSurface surface = TextInputSurface::None;
    SDL_FRect area{};
    float text_x = 0.0f;
    float text_y = 0.0f;
    float cursor_x = 0.0f;
    SDL_Color foreground{};
    SDL_Color background{};
  };

  struct MergeSurfaceLayout {
    float line_height = 14.0f;
    float gutter_width = 28.0f;
    float divider_width = 16.0f;
    float left_width = 0.0f;
    float center_width = 0.0f;
    float right_width = 0.0f;
    float left_x = 0.0f;
    float center_x = 0.0f;
    float right_x = 0.0f;
    float button_y = 0.0f;
    float secondary_button_y = 0.0f;
    float header_y = 0.0f;
    float rows_y = 0.0f;
    int visible_rows = 1;
    std::size_t visible_columns = 1;
    bool show_vertical = false;
    bool show_horizontal = false;
  };

  struct MergeToolbarLayout {
    SDL_FRect prev_rect{};
    SDL_FRect next_rect{};
    SDL_FRect open_rect{};
    SDL_FRect save_rect{};
  };

  struct MergeResultInteractionLayout {
    SDL_FRect rect{};
    editor::EditorViewMetrics metrics{};
    VisibleLineRangeLayout lines{};
    TextGridInteractionLayout text{};
  };

  struct MergeInteractionLayout {
    float content_bottom = 0.0f;
    MergeResultInteractionLayout result{};
    float incoming_accept_button_width = 0.0f;
    float current_accept_button_width = 0.0f;
    std::array<float, 4> result_action_widths{};
  };

  struct ChangedLineSpan {
    std::size_t old_start = 0;
    std::size_t old_end = 0;
    std::size_t new_end = 0;
  };

  struct BottomPanelLogLayout {
    SDL_FRect content_rect{};
    float text_x = 0.0f;
    float text_y = 0.0f;
    float text_width = 0.0f;
    float line_height = 0.0f;
    ScrollSurfaceLayout scroll;
  };

  struct VisibleStripTab {
    std::size_t index = 0;
    SDL_FRect rect{};
    SDL_FRect close_rect{};
    bool active = false;
    std::string display_title;
    std::string tooltip_label;
  };

  struct VisibleMenuBarItem {
    MenuId id = MenuId::None;
    SDL_FRect rect{};
    bool active = false;
  };

  struct VisibleWindowControlButton {
    WindowControlButtonId id = WindowControlButtonId::Minimize;
    SDL_FRect rect{};
    bool hovered = false;
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

  struct EditorBlamePopupLayout {
    std::size_t line_index = 0;
    SDL_FRect rect{};
    SDL_FRect copy_sha_rect{};
  };

  struct EditorHoverTarget {
    enum class Kind {
      Blame,
      Diagnostic,
      Plugin,
    };

    Kind kind = Kind::Blame;
    SDL_FRect anchor_rect{};
    std::size_t blame_line_index = 0;
    std::optional<editor::PublishedDiagnostic> diagnostic;
    std::optional<plugin::PluginHost::HoverResult> plugin_hover;

    bool operator==(const EditorHoverTarget& other) const {
      return kind == other.kind && anchor_rect.x == other.anchor_rect.x &&
             anchor_rect.y == other.anchor_rect.y && anchor_rect.w == other.anchor_rect.w &&
             anchor_rect.h == other.anchor_rect.h &&
             blame_line_index == other.blame_line_index && diagnostic == other.diagnostic &&
             plugin_hover == other.plugin_hover;
    }
  };

  struct EditorHoverPopupLayout {
    EditorHoverTarget::Kind kind = EditorHoverTarget::Kind::Blame;
    SDL_FRect anchor_rect{};
    SDL_FRect rect{};
    std::size_t blame_line_index = 0;
    std::optional<editor::PublishedDiagnostic> diagnostic;
    std::optional<plugin::PluginHost::HoverResult> plugin_hover;
    std::optional<SDL_FRect> primary_action_rect;
  };

  enum class DirtyPathResolution {
    RequirePrompt,
    Save,
    Discard,
  };

  enum class ProjectOpenDialogLaunchResult {
    Launched,
    AlreadyOpen,
    Unavailable,
  };

  struct TextCompositionState {
    TextInputSurface surface = TextInputSurface::None;
    std::string text;
    int start = -1;
    int length = -1;
  };

  struct VisiblePopupMenuItem {
    std::size_t index = 0;
    SDL_FRect rect{};
    bool enabled = false;
    bool checked = false;
    bool hovered = false;
    bool separator = false;
  };

  friend class WorkspaceActionContext;
  friend class ProjectCatalogCoordinator;
  friend class PersistenceCoordinator;
  friend class CommandPromptCoordinator;
  friend class MenuCoordinator;
  friend class KeyInputCoordinator;
  friend class TextInputCoordinator;
  friend class TabCoordinator;
  friend class PathMutationCoordinator;
  friend class LifecycleCoordinator;
  friend class DirtyPromptCoordinator;
  friend class CompareInteractionCoordinator;
  friend class DiffTabCoordinator;
  friend class SidebarCoordinator;
  friend class ChromeMouseCoordinator;
  friend class EditorMouseCoordinator;
  friend class CompareMouseCoordinator;
  friend class MergeMouseCoordinator;
  friend class TabMouseCoordinator;
  friend class SidebarMouseCoordinator;
  friend class PanelMouseCoordinator;

  static constexpr float kProjectSearchQueryTop = 38.0f;
  static constexpr float kProjectSearchReplaceTop = 54.0f;
  static constexpr float kProjectSearchButtonTop = 72.0f;
  static constexpr float kProjectSearchButtonHeight = 18.0f;
  static constexpr float kProjectSearchStatusTop = 94.0f;
  static constexpr float kProjectSearchResultsTop = 112.0f;

  static std::span<const ActionSpec> ActionSpecs();
  static const ActionSpec* FindActionSpec(ActionId id);
  static const ActionSpec* FindActionByCommand(std::string_view command_name);
  static const std::vector<std::string>& CommandNames();
  bool IsActionEnabled(ActionId id) const;
  static std::span<const MenuSpec> MenuSpecs();
  static const MenuSpec* FindMenuSpec(MenuId id);
  std::span<const MenuItemSpec> MenuItems(MenuId id) const;
  static std::span<const MenuItemSpec> TreeContextMenuItems(TreeContextTargetKind target);
  std::vector<VisibleMenuBarItem> ComputeVisibleMenuBarItems(const SDL_FRect& menu_bar) const;
  std::vector<VisibleWindowControlButton> ComputeVisibleWindowControlButtons(
      const SDL_FRect& menu_bar) const;
  std::optional<SDL_FRect> ComputePopupMenuRect(const SDL_FRect& anchor_rect,
                                                std::span<const MenuItemSpec> items,
                                                const SDL_FRect& bounds) const;
  std::optional<SDL_FRect> ComputePopupMenuRect(const SDL_FRect& menu_bar, MenuId id) const;
  std::vector<VisiblePopupMenuItem> ComputeVisiblePopupMenuItems(
      std::span<const MenuItemSpec> items,
      int active_item_index,
      const SDL_FRect& popup_rect) const;
  std::vector<VisiblePopupMenuItem> ComputeVisiblePopupMenuItems(MenuId id,
                                                                 const SDL_FRect& popup_rect) const;
  std::string MenuItemLabel(const MenuItemSpec& item) const;
  std::string MenuItemAccelerator(const MenuItemSpec& item) const;
  bool IsMenuItemEnabled(const MenuItemSpec& item) const;
  bool IsMenuItemChecked(const MenuItemSpec& item) const;
  std::optional<SDL_FRect> ActiveSubmenuRect(const SDL_FRect& menu_bar) const;
  const project::TreeEntry* SelectedTreeEntry() const;
  std::filesystem::path SelectedTreePath() const;
  TreeContextTargetKind SelectedTreeTargetKind() const;
  std::filesystem::path ResolveTreeActionPath(ActionSource source) const;
  std::optional<SDL_FRect> ComputeTreeContextMenuRect() const;
  static char KeycodeToAscii(SDL_Keycode keycode, SDL_Keymod modifiers);
  std::filesystem::path ResolveProjectRootInput(const std::filesystem::path& project_root) const;
  ProjectOpenDialogLaunchResult OpenNativeProjectPicker(std::string* error_message = nullptr);
  static void SDLCALL OnProjectOpenDialogComplete(void* userdata,
                                                  const char* const* filelist,
                                                  int filter);
  void ConsumePendingProjectOpenDialogResult();
  bool SetProjectRoot(const std::filesystem::path& project_root);
  static bool ConfigureProjectState(ProjectWorkspaceState& state,
                                    const std::filesystem::path& project_root);
  void RebindProjectState(ProjectWorkspaceState& state);
  void ResetCurrentProjectStateStorage();
  void ResetTransientInteractionState();
  void ClearDragState();
  bool HasActiveProjectCatalogEntry() const;
  ProjectWorkspaceState* ProjectCatalogEntry(std::size_t index);
  const ProjectWorkspaceState* ProjectCatalogEntry(std::size_t index) const;
  std::filesystem::path ProjectCatalogRoot(std::size_t index) const;
  void ResetProjectCatalogToWelcomeState();
  bool ReloadPluginsForCurrentProject();
  bool ReloadPluginsIfPluginAssetsChanged(bool force_check);
  void InvalidateRuntimeSyntaxStateCaches();
  std::string PluginRuntimeReloadSummary() const;
  void NotifyPluginsAboutOpenBuffers();
  void NotifyPluginBufferOpen(const std::filesystem::path& path);
  void NotifyPluginBufferSave(const std::filesystem::path& path);
  void ResetProjectScopedState(bool show_welcome);
  void SetWelcomePlaceholder();
  bool InitializeCurrentProject(const std::filesystem::path& project_root,
                                bool restore_persistence,
                                bool log_feedback,
                                bool activate_restored_tab = true);
  bool ActivateProjectState(ProjectWorkspaceState& state, bool activate_restored_tab);
  void StoreCurrentProjectState(ProjectWorkspaceState& state);
  void LoadProjectState(ProjectWorkspaceState& state);
  bool OpenProjectTab(const std::filesystem::path& project_root,
                      bool restore_persistence,
                      bool log_feedback);
  bool SwitchProject(std::size_t index, bool log_feedback);
  bool MoveActiveProjectTo(std::size_t index);
  void RequestCloseProject(std::size_t index);
  void CloseProject(std::size_t index);
  void ShowDirtyPromptForProject(std::size_t index);
  void ActivateTab(std::size_t index);
  void CloseTab(std::size_t index);
  void RequestCloseTab(std::size_t index);
  void RequestCloseTabs(std::vector<std::size_t> indices);
  void SyncActiveEditorTab();
  bool SaveTab(std::size_t index);
  bool TabIsDirty(std::size_t index) const;
  std::string TabDisplayTitle(std::size_t index) const;
  std::string TabTooltipLabel(std::size_t index) const;
  std::vector<std::size_t> DirtyEditorTabIndices() const;
  static std::vector<std::size_t> DirtyEditorTabIndices(const ProjectWorkspaceState& state);
  std::vector<std::size_t> DirtyEditorTabIndicesForProject(std::size_t project_index) const;
  void ShowDirtyPromptForTab(std::size_t index);
  void ShowDirtyPromptForTabs(std::vector<std::size_t> target_tabs,
                              std::vector<std::size_t> dirty_tabs);
  void ShowDirtyPromptForQuit();
  void DismissDirtyPrompt(bool restore_focus);
  void ConfirmDirtyPrompt();
  std::array<std::string, 3> DirtyPromptActionLabels() const;
  std::string DirtyPromptTitle() const;
  std::string DirtyPromptMessage() const;
  void OpenPromptSurface(PromptSurfaceState::Action action,
                         PromptSurfaceState::Kind kind,
                         const std::filesystem::path& path,
                         std::string input = {});
  void DismissPromptSurface(bool restore_focus);
  void ConfirmPromptSurface(DirtyPathResolution resolution = DirtyPathResolution::RequirePrompt);
  std::string PromptSurfaceTitle() const;
  std::string PromptSurfaceMessage() const;
  std::array<std::string, 2> PromptSurfaceActionLabels() const;
  std::filesystem::path TreeMutationBasePath(ActionSource source) const;
  bool HasDirtyEditorTabsForPath(const std::filesystem::path& path,
                                 std::string* blocking_label = nullptr) const;
  void CloseOpenTabsForPath(const std::filesystem::path& path);
  std::filesystem::path EditorViewPath(const TabEntry::EditorTabState::EditorViewState& view) const;
  bool RestoreEditorView(TabEntry::EditorTabState::EditorViewState& view);
  bool EnsureEditorTabLoaded(TabEntry& tab);
  bool ActivateCurrentTabAfterStateLoad();
  bool ActiveTabIsEditor() const;
  TabEntry::EditorTabState* ActiveEditorTab();
  const TabEntry::EditorTabState* ActiveEditorTab() const;
  static TabEntry::EditorTabState MakeEditorTabState(const editor::TextViewport& view);
  static std::unique_ptr<TabEntry::EditorTabState::EditorSplitNode> MakeEditorLeafNode(
      std::size_t leaf_id,
      float size_fraction = 1.0f);
  void SyncActiveEditorTabMetadata();
  bool ReplaceActiveEditorView(const editor::TextViewport& viewport);
  TabEntry::EditorTabState::EditorViewState* FindEditorViewState(
      TabEntry::EditorTabState& editor_tab,
      std::size_t leaf_id);
  const TabEntry::EditorTabState::EditorViewState* FindEditorViewState(
      const TabEntry::EditorTabState& editor_tab,
      std::size_t leaf_id) const;
  editor::TextViewport* FindEditorView(TabEntry::EditorTabState& editor_tab, std::size_t leaf_id);
  const editor::TextViewport* FindEditorView(const TabEntry::EditorTabState& editor_tab,
                                             std::size_t leaf_id) const;
  editor::TextViewport* ActiveEditorViewport();
  const editor::TextViewport* ActiveEditorViewport() const;
  editor::TextViewport* ActiveNavigableViewport();
  const editor::TextViewport* ActiveNavigableViewport() const;
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
  ScrollSurfaceLayout ComputeEditorScrollLayout(const SDL_FRect& rect,
                                                const editor::TextViewport& viewport,
                                                const editor::EditorViewMetrics& metrics) const;
  bool ActiveTabIsCompare() const;
  CompareTabState* ActiveCompareTab();
  const CompareTabState* ActiveCompareTab() const;
  CompareSurfaceLayout ComputeCompareSurfaceLayout(const SDL_FRect& rect,
                                                   const CompareTabState& compare_tab) const;
  ScrollSurfaceLayout ComputeCompareScrollLayout(const SDL_FRect& rect,
                                                 const CompareSurfaceLayout& surface,
                                                 const CompareTabState& compare_tab) const;
  TextGridInteractionLayout BuildCompareRightInteractionLayout(const CompareSurfaceLayout& surface,
                                                               CompareTabState& compare_tab) const;
  int CompareMaxScrollRow(const CompareTabState& compare_tab, int visible_rows) const;
  void ClampCompareScrollRow(CompareTabState& compare_tab, int visible_rows) const;
  std::size_t CompareMaxScrollColumn(const CompareTabState& compare_tab,
                                     std::size_t visible_columns) const;
  void ClampCompareHorizontalScroll(CompareTabState& compare_tab,
                                    std::size_t visible_columns) const;
  void RevealActiveCompareSelection();
  bool ActiveTabIsMerge() const;
  MergeTabState* ActiveMergeTab();
  const MergeTabState* ActiveMergeTab() const;
  MergeSurfaceLayout ComputeMergeSurfaceLayout(const SDL_FRect& rect,
                                               const MergeTabState& merge_tab) const;
  ScrollSurfaceLayout ComputeMergeScrollLayout(const SDL_FRect& rect,
                                               const MergeSurfaceLayout& surface,
                                               const MergeTabState& merge_tab) const;
  TextGridInteractionLayout BuildMergeSourceInteractionLayout(const MergeSurfaceLayout& surface,
                                                              const MergeTabState& merge_tab,
                                                              bool incoming) const;
  MergeResultInteractionLayout BuildMergeResultInteractionLayout(
      const SDL_FRect& rect,
      const MergeSurfaceLayout& surface,
      MergeTabState& merge_tab) const;
  MergeInteractionLayout BuildMergeInteractionLayout(const SDL_FRect& rect,
                                                     const MergeSurfaceLayout& surface,
                                                     MergeTabState& merge_tab) const;
  std::optional<std::size_t> FindMergeTrackedConflictAtSourceLine(const MergeTabState& merge_tab,
                                                                  std::size_t line,
                                                                  bool incoming) const;
  std::optional<std::size_t> FindMergeTrackedConflictAtResultLine(const MergeTabState& merge_tab,
                                                                  std::size_t line) const;
  SDL_FRect BuildMergeSourceActionButtonRect(const MergeSurfaceLayout& surface,
                                             const MergeInteractionLayout& interaction,
                                             const MergeTrackedConflict& conflict,
                                             bool incoming) const;
  std::array<SDL_FRect, 4> BuildMergeResultActionButtonRects(
      const MergeSurfaceLayout& surface,
      const MergeInteractionLayout& interaction,
      const MergeTrackedConflict& conflict) const;
  std::optional<MergeHoverState> ClassifyMergeHoverState(const MergeSurfaceLayout& surface,
                                                         const MergeInteractionLayout& interaction,
                                                         const MergeTabState& merge_tab,
                                                         float x,
                                                         float y) const;
  MergeToolbarLayout ComputeMergeToolbarLayout(const SDL_FRect& rect,
                                              const MergeSurfaceLayout& surface) const;
  int MergeMaxScrollRow(const MergeTabState& merge_tab, int visible_rows) const;
  void ClampMergeScrollRow(MergeTabState& merge_tab, int visible_rows) const;
  std::size_t MergeMaxScrollColumn(const MergeTabState& merge_tab,
                                   std::size_t visible_columns) const;
  void ClampMergeHorizontalScroll(MergeTabState& merge_tab, std::size_t visible_columns) const;
  void RevealActiveMergeSelection();
  std::string ActiveTabTitle() const;
  void OpenComparePicker();
  bool OpenComparePickerForPath(const std::filesystem::path& path,
                                std::string_view commit_spec = {});
  SidebarMode SidebarModeForViewId(std::string_view view_id) const;
  SidebarMode ActiveSidebarMode() const;
  std::optional<TabEntry> BuildCompareTabEntry(const std::filesystem::path& path,
                                               const project::GitCommitEntry& commit,
                                               std::size_t selected_row = 0) const;
  std::optional<TabEntry> BuildCompareTabEntry(const std::filesystem::path& path,
                                               const CompareTabState& compare_tab) const;
  void RefreshCompareTabDerivedState(CompareTabState& compare_tab) const;
  std::size_t CompareRowIndexForRightLine(const CompareTabState& compare_tab,
                                          std::size_t line_index) const;
  std::size_t CompareRightLineForRow(const CompareTabState& compare_tab,
                                     std::size_t row_index) const;
  void SyncCompareViewportScroll(CompareTabState& compare_tab) const;
  void SyncCompareSelectionFromViewport(CompareTabState& compare_tab,
                                        bool reveal_selection) const;
  std::optional<TabEntry> BuildMergeTabEntry(const std::filesystem::path& base_path,
                                             const std::filesystem::path& incoming_path,
                                             const std::filesystem::path& current_path,
                                             const std::filesystem::path& output_path) const;
  void RefreshComparePicker();
  void MoveComparePickerSelection(int delta);
  void OpenSelectedCompareCommit();
  void OpenComparison(const project::GitCommitEntry& commit);
  void OpenWorkingFileFromCompare();
  void MoveCompareSelection(int delta);
  void JumpCompareHunk(int delta);
  void ScrollCompareRows(int delta);
  void ScrollCompareColumns(int delta);
  bool OpenMergeEditor(const std::filesystem::path& base_path,
                       const std::filesystem::path& incoming_path,
                       const std::filesystem::path& current_path,
                       const std::filesystem::path& output_path);
  void RefreshMergeTabDerivedState(MergeTabState& merge_tab) const;
  void PopulateCompareSyntaxTokensForWindow(CompareTabState& compare_tab,
                                            std::size_t visible_start_row,
                                            std::size_t visible_end_row);
  std::vector<MergeTrackedConflict> BuildMergeTrackedConflicts(
      const compare::MergeModel& model) const;
  std::vector<MergeTrackedConflict> BuildMergeTrackedConflictsForResult(
      compare::MergeModel& model,
      const std::vector<std::string>& result_lines,
      std::span<const std::vector<std::string>> conflict_line_hints = {},
      std::span<const compare::MergeChoice> choice_hints = {}) const;
  void UpdateMergeMaxVisualColumns(MergeTabState& merge_tab,
                                   std::span<const std::string> result_lines) const;
  void PopulateMergeSyntaxTokensForWindow(MergeTabState& merge_tab,
                                          std::size_t visible_start_row,
                                          std::size_t visible_end_row);
  void UpdateMergeTrackingAfterViewportEdit(MergeTabState& merge_tab,
                                            const std::vector<std::string>& before_lines,
                                            std::optional<editor::SelectionRange> selection_before,
                                            editor::TextPosition cursor_before);
  editor::TextViewport* ActiveEditableViewport();
  const editor::TextViewport* ActiveEditableViewport() const;
  void MoveMergeSelection(int delta);
  void ScrollMergeColumns(int delta);
  void ApplyMergeChoice(compare::MergeChoice choice);
  void OpenMergeResultFile();
  void MoveFileFinderSelection(int delta);
  void RenderCompareSurface(SDL_Renderer* renderer, const SDL_FRect& rect);
  void RenderMergeSurface(SDL_Renderer* renderer, const SDL_FRect& rect);
  void ShowSidebarMode(SidebarMode mode, bool temporary = false);
  void ShowTreeSidebar(const std::filesystem::path& root = {});
  void ShowSearchSidebar(std::string query = {}, bool temporary = false);
  void ShowProblemsSidebar();
  void ShowGitSidebar();
  bool ShowPluginSidebar(std::string_view id, bool temporary = false);
  std::string SidebarModeControlLabel() const;
  SDL_FRect SidebarModeControlRect(const SDL_FRect& sidebar_rect) const;
  void CloseSidebar();
  void ToggleSidebar();
  void RestorePreviousSidebar();
  void RefreshProjectFiles();
  void RefreshGitSidebar();
  bool RefreshProblemsSidebar();
  bool RefreshPluginSidebar();
  SDL_FRect TreeSidebarCollapseButtonRect(const SDL_FRect& sidebar_rect) const;
  SDL_FRect TreeSidebarRefreshButtonRect(const SDL_FRect& sidebar_rect) const;
  SDL_FRect GitSidebarActionRowRect(const SDL_FRect& sidebar_rect) const;
  SDL_FRect GitSidebarRefreshButtonRect(const SDL_FRect& sidebar_rect) const;
  SDL_FRect GitSidebarStageAllButtonRect(const SDL_FRect& sidebar_rect) const;
  SDL_FRect GitSidebarDiscardAllButtonRect(const SDL_FRect& sidebar_rect) const;
  float GitSidebarListTop(const SDL_FRect& sidebar_rect) const;
  float GitSidebarVisibleUnits(const SDL_FRect& sidebar_rect) const;
  ScrollableListLayout ComputeProjectSearchSidebarListLayout(const SDL_FRect& sidebar_rect,
                                                             std::size_t line_count) const;
  ScrollableListLayout ComputeGitSidebarListLayout(const SDL_FRect& sidebar_rect,
                                                   std::size_t line_count) const;
  ScrollableListLayout ComputeTreeSidebarListLayout(const SDL_FRect& sidebar_rect,
                                                    std::size_t line_count) const;
  ScrollableListLayout ComputeProblemsSidebarListLayout(const SDL_FRect& sidebar_rect,
                                                        std::size_t line_count) const;
  ScrollableListLayout ComputePluginSidebarListLayout(const SDL_FRect& sidebar_rect,
                                                      std::size_t line_count) const;
  std::vector<GitSidebarLine> BuildGitSidebarLines() const;
  GitSidebarEntryActionLayout ComputeGitSidebarEntryActionLayout(const SDL_FRect& row_rect,
                                                                 const GitSidebarEntry& entry) const;
  std::optional<std::size_t> SelectedGitSidebarLineIndex() const;
  const GitSidebarEntry* SelectedGitSidebarEntry() const;
  void RevealSelectedTreeSidebarLine();
  void RevealSelectedGitSidebarLine();
  void RevealSelectedProblemsSidebarLine();
  void RevealSelectedPluginSidebarLine();
  void MoveGitSidebarSelection(int delta);
  void MoveProblemsSidebarSelection(int delta);
  void MovePluginSidebarSelection(int delta);
  bool OpenGitSidebarEntry(std::size_t entry_index);
  bool OpenSelectedProblemSidebarItem();
  bool OpenSelectedPluginSidebarItem();
  bool CanStageAllGitSidebarEntries() const;
  bool CanDiscardAllGitSidebarEntries() const;
  bool StageAllGitSidebarEntries();
  void OpenDiscardAllGitSidebarPrompt();
  bool DiscardAllGitSidebarEntries();
  bool StageGitSidebarEntry(std::size_t entry_index);
  bool UnstageGitSidebarEntry(std::size_t entry_index);
  bool DiscardGitSidebarEntry(std::size_t entry_index);
  void ReconcileOpenTabsAfterPathDiscard(const std::filesystem::path& path);
  void ReloadCleanEditorTabsForPath(const std::filesystem::path& path);
  bool EditorBlameFitsPane(const editor::TextViewport& viewport,
                           const SDL_FRect& rect,
                           float minimum_pane_width = 520.0f) const;
  std::optional<editor::EditorBlameOverlay> BuildEditorBlameOverlay(
      editor::TextViewport& viewport,
      const SDL_FRect& rect,
      float minimum_pane_width = 520.0f);
  std::optional<editor::EditorBlameOverlay> BuildCompareBlameOverlay(
      CompareTabState& compare_tab,
      const CompareSurfaceLayout& surface,
      const SDL_FRect& rect);
  const editor::EditorBlameLine* VisibleEditorBlameLine(std::size_t line_index) const;
  const editor::EditorBlameLine* EditorBlameLineAtPosition(float x, float y) const;
  std::optional<EditorHoverTarget> EditorHoverTargetAtPosition(float x, float y) const;
  std::optional<EditorHoverTarget> DiagnosticHoverTargetAtPosition(float x, float y) const;
  std::optional<EditorHoverTarget> DiagnosticHoverTargetForViewport(
      const editor::TextViewport& viewport,
      const TextGridInteractionLayout& interaction,
      std::span<const editor::PublishedDiagnostic> diagnostics,
      float x,
      float y) const;
  std::optional<EditorHoverTarget> PluginHoverTargetAtPosition(float x, float y) const;
  std::optional<EditorHoverTarget> PluginHoverTargetForViewport(
      const editor::TextViewport& viewport,
      const TextGridInteractionLayout& interaction,
      float x,
      float y) const;
  std::optional<EditorHoverTarget> PluginHoverTargetForLine(
      const std::filesystem::path& path,
      std::string_view line,
      std::size_t line_index,
      std::size_t tab_size,
      const TextGridInteractionLayout& interaction,
      float x,
      float y) const;
  std::optional<EditorHoverPopupLayout> ActiveEditorHoverPopupLayout() const;
  SDL_FRect EditorHoverPopupPrimaryActionHitRect(const EditorHoverPopupLayout& popup) const;
  bool EditorHoverPopupPrimaryActionHovered(float x, float y) const;
  std::vector<std::string> WrapEditorHoverPopupText(std::string_view text,
                                                    float max_width,
                                                    std::size_t max_lines) const;
  void UpdateEditorHover(float x, float y);
  std::optional<EditorBlamePopupLayout> ActiveEditorBlamePopupLayout() const;
  SDL_FRect EditorBlamePopupCopyShaHitRect(const EditorBlamePopupLayout& popup) const;
  bool EditorBlamePopupCopyShaHovered(float x, float y) const;
  std::vector<std::string> WrapEditorBlamePopupText(std::string_view text,
                                                    float max_width,
                                                    std::size_t max_lines) const;
  void UpdateEditorBlameHover(float x, float y);
  void InvalidateEditorBlamePath(const std::filesystem::path& path);
  void ClearEditorBlame();
  std::optional<std::size_t> FindOpenCompareTabIndex(const std::filesystem::path& path,
                                                     std::string_view left_ref,
                                                     std::string_view right_ref) const;
  std::optional<std::size_t> FindOpenMergeTabIndex(const std::filesystem::path& path) const;
  std::optional<TabEntry> BuildCompareTabFromBuffers(const std::filesystem::path& path,
                                                     std::string left_content,
                                                     std::string right_content,
                                                     std::string left_label,
                                                     std::string right_label,
                                                     std::size_t selected_row = 0,
                                                     bool persistable = true) const;
  bool OpenWorkingTreeComparison(const std::filesystem::path& path,
                                 const std::string& left_ref,
                                 const std::string& left_label);
  bool OpenBranchHeadComparison(const std::filesystem::path& path,
                                const std::string& left_ref,
                                const std::string& left_label,
                                const std::string& right_ref,
                                const std::string& right_label);
  std::optional<TabEntry> BuildMergeTabFromBuffers(const std::filesystem::path& output_path,
                                                   std::string base_content,
                                                   std::string incoming_content,
                                                   std::string current_content,
                                                   std::string incoming_label,
                                                   std::string result_label,
                                                   std::string current_label,
                                                   std::size_t selected_hunk = 0,
                                                   bool persistable = true) const;
  bool OpenGitConflictMerge(const std::filesystem::path& path);
  void OpenBufferSearch();
  void OpenBufferReplace();
  void OpenProjectSearch();
  FocusTarget PrimarySurfaceFocusTarget() const;
  void ShowOverlay(OverlayMode mode);
  void DismissOverlay(bool focus_editor = false);
  void RefreshBufferSearch();
  void RefreshProjectSearch();
  void StopProjectSearch();
  void ConsumeProjectSearchUpdates();
  void ResetOverlayScroll();
  float OverlayListStartOffset() const;
  int OverlayVisibleRows(const SDL_FRect& overlay) const;
  std::size_t OverlayItemCount() const;
  std::size_t OverlaySelectedIndex() const;
  ScrollableListLayout ComputeOverlayListLayout(const SDL_FRect& overlay) const;
  void SetOverlaySelectedIndex(std::size_t index);
  void ClampOverlayScrollRow(const SDL_FRect& overlay);
  void RevealOverlaySelection(const SDL_FRect& overlay);
  bool ActivateOverlaySelection();
  void BeginProjectSearchEdit(ProjectSearchEditField field);
  void CommitProjectSearchEdit();
  void CancelProjectSearchEdit();
  SDL_FRect ProjectSearchQueryRect(const SDL_FRect& sidebar_rect) const;
  SDL_FRect ProjectSearchReplaceRect(const SDL_FRect& sidebar_rect) const;
  SDL_FRect ProjectSearchModeButtonRect(const SDL_FRect& sidebar_rect) const;
  SDL_FRect ProjectSearchCaseButtonRect(const SDL_FRect& sidebar_rect) const;
  SDL_FRect ProjectSearchHiddenButtonRect(const SDL_FRect& sidebar_rect) const;
  std::string ProjectSearchModeButtonLabel() const;
  std::string ProjectSearchCaseButtonLabel() const;
  std::string ProjectSearchHiddenButtonLabel() const;
  bool ProjectSearchCanReplaceAll() const;
  bool ProjectSearchReplaceCaseSensitive() const;
  void ToggleProjectSearchPatternMode();
  void CycleProjectSearchCaseMode();
  void ToggleProjectSearchHiddenFiles();
  void ReplaceAllProjectSearchMatches();
  std::vector<int> BuildProjectSearchLineMap() const;
  int ProjectSearchLineForResult(std::size_t index) const;
  void MoveBufferSearchSelection(int delta);
  void MoveProjectSearchSelection(int delta);
  void ReplaceCurrentBufferSearchMatch();
  void ReplaceAllBufferSearchMatches();
  std::optional<editor::SelectionRange> ActiveBufferSearchMatch() const;
  void OpenTerminal(std::string command, bool focus_terminal = true, bool log_feedback = true);
  bool ReopenActiveTab();
  std::filesystem::path ConfigStatePath() const;
  std::filesystem::path UserConfigPath() const;
  std::filesystem::path ProjectStateDirectory() const;
  void ApplyEditorPreferences(editor::TextViewport& viewport) const;
  void ApplyEditorPreferencesToAllTabs();
  bool WriteClipboardText(std::string_view text) const;
  std::optional<std::string> ReadClipboardText() const;
  bool WritePrimarySelectionText(std::string_view text) const;
  std::optional<std::string> ReadPrimarySelectionText() const;
  std::optional<std::string> SelectionTextWithContext() const;
  void SyncPrimarySelectionWithActiveEditor();
  void SyncPrimarySelectionWithTerminalSelection();
  void CloseAllTabs();
  void ReloadCleanOpenBuffersFromDisk();
  void AppendTerminalPendingInput(std::string_view input);
  void EraseLastTerminalPendingInputCodepoint();
  void SubmitTerminalPendingInput();
  std::optional<std::string> LastTerminalCommandText() const;
  TextInputSurface CurrentTextInputSurface() const;
  TerminalTabState* ActiveTerminalTab();
  const TerminalTabState* ActiveTerminalTab() const;
  std::optional<std::size_t> FocusedTerminalTabIndex() const;
  void SyncTerminalFocusState();
  bool MoveActiveTerminalTabTo(std::size_t index);
  void CloseTerminalTab(std::size_t index);
  void ConsumeTerminalSessionUpdates();
  void ReapExitedTerminalTabs();
  bool BottomPanelVisible() const;
  BottomPanelLogLayout ComputeBottomPanelLogLayout(const WorkspaceLayout& layout,
                                                   std::size_t line_count) const;
  int BottomPanelVisibleRows(float panel_height) const;
  int BottomPanelScrollRow(std::size_t line_count, int visible_rows) const;
  void SetBottomPanelScrollRow(int scroll_row, std::size_t line_count, int visible_rows);
  void ClearTerminalSelection();
  bool TerminalHasSelection() const;
  std::optional<std::string> TerminalUrlAtPoint(float x, float y) const;
  std::optional<TerminalSelectionPosition> TerminalSelectionPositionForPoint(
      int x,
      int y,
      const std::vector<terminal::TerminalLine>& lines,
      std::size_t first_row = 0) const;
  std::optional<TerminalSelectionPosition> TerminalViewportPositionForPoint(int x, int y) const;
  terminal::TerminalSession::MouseButton TerminalMouseButtonForSdl(Uint8 button) const;
  std::optional<TerminalSelectionBounds> ActiveTerminalSelectionBounds() const;
  std::string SelectedTerminalText() const;
  bool TerminalCellSelected(std::size_t row, std::size_t column) const;
  bool OpenExternalUrl(std::string_view url) const;
  void ResizeTerminalToPanel(const SDL_FRect& panel_rect);
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
  void RenderFrameBase(SDL_Renderer* renderer, const WorkspaceLayout& layout) const;
  void RenderActiveWorkspaceSurface(SDL_Renderer* renderer,
                                    const WorkspaceLayout& layout,
                                    bool draw_editor_caret,
                                    std::optional<SDL_FRect>* active_editor_pane_rect);
  void RenderWindowChrome(SDL_Renderer* renderer, const WorkspaceLayout& layout) const;
  void RenderSidebarSurface(SDL_Renderer* renderer, const WorkspaceLayout& layout);
  void RenderOverlaySurface(SDL_Renderer* renderer, const WorkspaceLayout& layout);
  void RenderBottomPanelSurface(SDL_Renderer* renderer,
                                const WorkspaceLayout& layout,
                                std::size_t terminal_line_count);
  void RenderMenuPopups(SDL_Renderer* renderer, const WorkspaceLayout& layout) const;
  void RenderPromptSurface(
      SDL_Renderer* renderer,
      const WorkspaceLayout& layout,
      const std::optional<TextInputVisual>& active_text_input_visual) const;
  void RenderDirtyPromptSurface(SDL_Renderer* renderer, const WorkspaceLayout& layout) const;
  std::optional<TextInputVisual> BuildActiveTextInputVisual(
      const WorkspaceLayout& layout,
      const std::optional<SDL_FRect>& active_editor_pane_rect) const;
  void UpdateTextInputArea(SDL_Renderer* renderer,
                           SDL_Window* render_window,
                           const std::optional<TextInputVisual>& visual) const;
  void RenderEditorHoverPopup(SDL_Renderer* renderer) const;
  void RenderTextComposition(SDL_Renderer* renderer,
                             const std::optional<TextInputVisual>& visual) const;
  float ProjectTabWidthForIndex(std::size_t index) const;
  void EnsureActiveProjectVisible();
  std::vector<VisibleStripTab> ComputeVisibleProjectTabs(
      const SDL_FRect& project_tab_strip) const;
  float TabWidthForIndex(std::size_t index) const;
  void EnsureActiveTabVisible();
  std::vector<VisibleStripTab> ComputeVisibleTabs(const SDL_FRect& tab_strip) const;
  std::string HoveredTabTooltipLabel(const SDL_FRect& tab_strip) const;
  std::string HoveredGitSidebarTooltipLabel(const SDL_FRect& sidebar_rect) const;
  std::vector<VisibleStripTab> ComputeVisibleTerminalTabs(
      const SDL_FRect& panel_header) const;
  static std::vector<VisibleStripTab> BuildVisibleStripTabs(
      const std::vector<float>& widths,
      float start_x,
      float gap,
      float max_tab_x,
      std::size_t scroll_index,
      float tab_y,
      float tab_height,
      const std::vector<std::size_t>& model_indices,
      std::size_t active_index,
      const std::vector<std::string>& display_titles,
      const std::vector<std::string>& tooltip_labels);
  void ClearTabDrag();
  SDL_FRect BottomPanelTerminalNewTabRect(const SDL_FRect& panel_header) const;
  SDL_FRect ComputeOverlayRect(const SDL_FRect& editor_area) const;
  std::string BreadcrumbLabel() const;
  std::string ProjectLabel() const;
  std::string ProjectLabelForRoot(const std::filesystem::path& root) const;
  std::string ProjectTabDisplayTitle(std::size_t index) const;
  std::string TruncateLabel(std::string_view text, float max_width) const;
  std::optional<SDL_FRect> CurrentWindowRect() const;
  std::optional<WorkspaceLayout> CurrentWorkspaceLayout() const;
  const WindowChromeState& CurrentWindowChromeState() const;
  void RequestFullRedraw();
  void RequestRedrawRect(const SDL_FRect& rect);
  void RequestWindowRedraw();
  void RequestChromeRedraw();
  void RequestSidebarRedraw();
  void RequestSidebarLayoutChangeRedraw(const WorkspaceLayout& previous_layout);
  void RequestBreadcrumbRedraw();
  void RequestTabStripRedraw();
  void RequestEditorSurfaceRedraw();
  void RequestActiveTabRedraw(bool include_tree_sidebar = false);
  void RequestFocusedEditorRedraw();
  void RequestEditorLineRangeRedraw(std::size_t start_line, std::size_t end_line);
  void RequestEditorLineToBottomRedraw(std::size_t start_line);
  void RequestActiveEditableChangeRedraw(const std::vector<std::string>& before_lines,
                                         const std::vector<std::string>& after_lines);
  void RequestActiveEditableBlameNeighborhoodRedraw(std::size_t before_line,
                                                    std::size_t after_line);
  void RequestCompareRowRangeRedraw(std::size_t start_row, std::size_t end_row);
  void RequestCompareRowToBottomRedraw(std::size_t start_row);
  void RequestCompareRightLineRangeRedraw(std::size_t start_line, std::size_t end_line);
  void RequestCompareRightLineToBottomRedraw(std::size_t start_line);
  void RequestMergeResultLineRangeRedraw(std::size_t start_line, std::size_t end_line);
  void RequestMergeResultLineToBottomRedraw(std::size_t start_line);
  void RequestMergeConflictRedraw(std::size_t conflict_index);
  void RequestBottomPanelRedraw();
  void RequestBottomPanelCommandRedraw();
  void RequestBottomPanelLayoutChangeRedraw(const WorkspaceLayout& previous_layout);
  void RequestBottomPanelContentRedraw();
  void RequestCommandModeTransitionRedraw(bool bottom_panel_was_visible);
  void RequestOverlayRedraw();
  void RequestPromptRedraw();
  void QueueEditorHoverRefresh();
  std::optional<SDL_FRect> CurrentChromeRedrawRect() const;
  std::optional<SDL_FRect> CurrentFocusedEditorRedrawRect() const;
  std::optional<SDL_FRect> CurrentEditorLineRangeRect(std::size_t start_line,
                                                      std::size_t end_line) const;
  std::optional<SDL_FRect> CurrentEditorLineToBottomRect(std::size_t start_line) const;
  std::optional<SDL_FRect> CurrentCompareRowRangeRect(std::size_t start_row,
                                                      std::size_t end_row) const;
  std::optional<SDL_FRect> CurrentCompareRowToBottomRect(std::size_t start_row) const;
  std::optional<SDL_FRect> CurrentMergeResultLineRangeRect(std::size_t start_line,
                                                           std::size_t end_line) const;
  std::optional<SDL_FRect> CurrentMergeResultLineToBottomRect(std::size_t start_line) const;
  std::optional<SDL_FRect> CurrentMergeConflictRect(std::size_t conflict_index) const;
  std::optional<SDL_FRect> CurrentBottomPanelCommandRedrawRect() const;
  std::optional<SDL_FRect> CurrentBottomPanelContentRedrawRect() const;
  std::optional<SDL_FRect> CurrentOverlayRedrawRect() const;
  std::optional<SDL_FRect> CurrentPromptRedrawRect() const;
  static std::optional<ChangedLineSpan> ComputeChangedLineSpan(
      const std::vector<std::string>& before_lines,
      const std::vector<std::string>& after_lines);
  std::optional<Uint32> NextCaretBlinkDelayMs() const;
  void ResetCaretBlink();
  bool ShouldBlinkCaret() const;
  bool CaretVisibleNow() const;
  std::optional<SDL_FRect> ActiveEditorCaretRect(const WorkspaceLayout& layout) const;
  std::optional<SDL_FRect> ActiveTerminalCaretRect(const WorkspaceLayout& layout) const;
  SDL_FRect CompareDividerHitRect(const SDL_FRect& editor_surface,
                                  const CompareSurfaceLayout& surface) const;
  std::optional<TextInputVisual> BuildCompareTextInputVisual(const SDL_FRect& editor_surface) const;
  std::optional<TextInputVisual> BuildMergeTextInputVisual(const SDL_FRect& editor_surface) const;
  void RenderCompareScrollbars(SDL_Renderer* renderer, const SDL_FRect& editor_surface);
  void RenderMergeScrollbars(SDL_Renderer* renderer, const SDL_FRect& editor_surface);
  CursorKind CursorKindForPosition(float x, float y) const;
  SDL_Cursor* CursorHandle(CursorKind kind);
  void UpdateMouseCursor(float x, float y);

  render::Theme theme_ = render::MakeDefaultTheme();
  render::TextRenderer text_renderer_;
  editor::EditorViewRenderer editor_view_renderer_;
  ProjectCatalogState project_catalog_;
  // Keep the active workspace, including its project-scoped surface state, in the same container
  // used by project-catalog entries so activation, persistence, and reset paths do not copy it.
  ProjectWorkspaceState current_project_state_;
  std::filesystem::path& project_root_ = current_project_state_.root;
  project::DirectoryTree& directory_tree_ = current_project_state_.directory_tree;
  project::FileIndex& file_index_ = current_project_state_.file_index;
  project::FileFinder& file_finder_ = current_project_state_.file_finder;
  editor::TextViewport& text_viewport_ = current_project_state_.text_viewport;
  std::vector<TabEntry>& open_tabs_ = current_project_state_.open_tabs;
  std::size_t& active_tab_index_ = current_project_state_.active_tab_index;
  int& tab_scroll_index_ = current_project_state_.tab_scroll_index;
  ProjectSurfaceState& surface_ = current_project_state_.surface;
  SidebarState& sidebar_state_ = current_project_state_.sidebar;
  OverlayState& overlay_state_ = current_project_state_.overlay;
  PanelState& panel_state_ = current_project_state_.panel;
  InteractionState interaction_state_;
  MenuSurfaceState menu_state_;
  std::vector<std::unique_ptr<TerminalTabState>>& terminal_tabs_ = current_project_state_.terminal_tabs;
  std::size_t& active_terminal_tab_index_ = current_project_state_.active_terminal_tab_index;
  WindowPresentationState window_presentation_;
  OverlayWorkflowState& overlay_workflow_ = overlay_state_.workflow;
  GitSidebarState& git_sidebar_ = sidebar_state_.git;
  ProblemsSidebarState& problems_sidebar_ = sidebar_state_.problems;
  PluginSidebarState& plugin_sidebar_ = sidebar_state_.plugin;
  editor::DiagnosticsStore& diagnostics_store_ = current_project_state_.diagnostics_store;
  struct SidebarModeMenuEntry {
    std::string label;
    std::string id;
  };
  mutable std::vector<SidebarModeMenuEntry> sidebar_mode_menu_entries_;
  mutable std::vector<MenuItemSpec> sidebar_mode_menu_items_;
  WorkspaceProjectSearchRuntime project_search_runtime_;
  WorkspacePluginRuntime plugin_runtime_;
  Uint32 git_blame_event_type_ = 0;
  Uint32 terminal_event_type_ = 0;
  Uint32 project_open_dialog_event_type_ = 0;
  project::GitBlameService git_blame_service_;
  std::optional<editor::EditorBlameOverlay> visible_editor_blame_overlay_;
  std::optional<EditorHoverTarget> active_editor_hover_target_;
  bool editor_hover_refresh_pending_ = false;
  std::function<std::optional<std::string>()> clipboard_text_reader_;
  std::function<bool(std::string_view)> clipboard_text_writer_;
  std::function<std::optional<std::string>()> primary_selection_text_reader_;
  std::function<bool(std::string_view)> primary_selection_text_writer_;
  std::function<bool(std::string_view)> external_url_opener_;
  SDL_Window* dialog_window_ = nullptr;
  ProjectDialogState project_dialog_state_;
  PromptState prompts_;
  bool quit_requested_ = false;
  CommandState& command_ = panel_state_.command;
  std::vector<std::string> available_colorscheme_names_;
  std::string& active_colorscheme_name_ = current_project_state_.active_colorscheme_name;
  std::optional<SDL_Color>& project_base_color_ = current_project_state_.project_base_color;
  EditorPreferences& editor_preferences_ = current_project_state_.editor_preferences;
  float ui_scale_ = 1.0f;
  float presentation_scale_x_ = 1.0f;
  float presentation_scale_y_ = 1.0f;
  WindowAction pending_window_action_ = WindowAction::None;
  TextInputSurface active_text_input_surface_ = TextInputSurface::None;
  TextCompositionState text_composition_;
  Uint64 caret_blink_epoch_ms_ = 0;
  RenderInvalidation pending_render_invalidation_;
  int post_render_full_redraws_remaining_ = 0;
  TabDragState tab_drag_state_;
  CursorKind cursor_kind_ = CursorKind::Default;
  SDL_Cursor* text_cursor_ = nullptr;
  SDL_Cursor* pointer_cursor_ = nullptr;
  SDL_Cursor* ew_resize_cursor_ = nullptr;
  SDL_Cursor* ns_resize_cursor_ = nullptr;
  float last_mouse_x_ = 0.0f;
  float last_mouse_y_ = 0.0f;
  bool last_mouse_position_valid_ = false;

#ifdef MICROIDE_TESTING
  friend struct WorkspaceShellTestAccess;
#endif
};

}  // namespace microide::workspace
