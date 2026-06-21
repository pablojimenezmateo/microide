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
  using GitSidebarActionId = workspace::GitSidebarActionId;
  using GitSidebarLine = workspace::GitSidebarLine;
  using GitSidebarEntryActionLayout = workspace::GitSidebarEntryActionLayout;
  using ProblemsSidebarEntry = workspace::ProblemsSidebarEntry;
  using TestsSidebarEntry = workspace::TestsSidebarEntry;
  using GitSidebarState = workspace::GitSidebarState;
  using ProblemsSidebarState = workspace::ProblemsSidebarState;
  using TestsSidebarState = workspace::TestsSidebarState;
  using PluginSidebarState = workspace::PluginSidebarState;
  using SidebarState = workspace::SidebarState;
  using DirtyPathResolution = workspace::DirtyPathResolution;
  using OverlayMode = workspace::OverlayMode;
  using CompletionSessionState = workspace::CompletionSessionState;
  using CodeActionSessionState = workspace::CodeActionSessionState;
  using PanelContentKind = workspace::PanelContentKind;

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
  using RenderInvalidation = workspace::RenderInvalidation;
  using EventResult = workspace::EventResult;
  struct FrameToken {
    struct VisibleLineRange {
      std::size_t start_line = 0;
      std::size_t end_line = 0;
      const editor::TextViewport* viewport = nullptr;
    };

   public:
    FrameToken() = default;
    FrameToken(std::uint64_t frame_id, VisibleLineRange visible_line_range)
        : frame_id_(frame_id), visible_line_range_(visible_line_range) {}
    bool valid() const { return frame_id_ != 0; }
    std::uint64_t frame_id() const { return frame_id_; }
    const VisibleLineRange& visible_line_range() const { return visible_line_range_; }

   private:
    std::uint64_t frame_id_ = 0;
    VisibleLineRange visible_line_range_{};
  };

  enum class IdleHint {
    Full,
    CaretOnly,
    Idle,
  };

  struct IdleWaitState {
    IdleHint hint = IdleHint::Idle;
    Uint32 caret_remaining_ms = 0;
  };

  WorkspaceShell();
  ~WorkspaceShell();

  static std::vector<std::string> DocumentedCommandUsages();
  void SetStartupOptions(WorkspaceStartupOptions options);
  const WorkspaceStartupOptions& StartupOptions() const { return startup_options_; }
  bool Initialize(const std::filesystem::path& project_root);
  // Apply a cold-start control spec (set breakpoints, reveal files, optionally
  // start debugging). Public: invoked by Application right after Initialize.
  void ApplyControlSpec(const ControlSpec& spec);
  // Force-start the control channel + stdout JSONL mirror when `--control` was
  // passed (startup_options_.control_stdout). No-op otherwise. Invoked by
  // Application right after Initialize, before the cold-start spec.
  void ForceStartControlChannel();
  // Apply the `--set <id> <value>` startup overrides transiently (persist=false).
  // Invoked by Application after Initialize.
  void ApplyStartupSettingOverrides();
  void Shutdown();
  EventResult HandleEvent(const SDL_Event& event);
  EventResult HandleScheduledWake();
  FrameToken PrepareFrameOnce(SDL_Renderer* renderer, int width, int height);
  void RenderClip(const FrameToken& frame_token, SDL_Renderer* renderer, int width, int height,
                  std::optional<SDL_FRect> dirty_rect_hint = std::nullopt);
  void EnsureClipFrameAndOverlayViewModels(const WorkspaceLayout& layout) const;
  void PrepareRenderFrame(SDL_Renderer* renderer, int width, int height);
  void Render(SDL_Renderer* renderer, int width, int height);
  void RenderPrepared(SDL_Renderer* renderer, int width, int height);
  void RootViewRenderFrameBase(SDL_Renderer* renderer, const WorkspaceLayout& layout) const;
  void RootViewRenderActiveWorkspaceSurface(SDL_Renderer* renderer,
                                            const WorkspaceLayout& layout,
                                            const FrameToken& frame_token,
                                            bool draw_editor_caret,
                                            std::optional<SDL_FRect>* active_editor_pane_rect);
  void RootViewRenderOverlaySurface(SDL_Renderer* renderer, const WorkspaceLayout& layout);
  IdleWaitState CurrentIdleWaitState() const;
  std::optional<Uint32> NextAnimationDelayMs() const;
  bool ConsumePostRenderFullRedrawRequest();
  // Background highlight prefetch around the active viewport; no-ops when cached.
  void RequestActiveHighlightPrefetch();
  void OnFramePresented();
  std::optional<SDL_FRect> CurrentCaretDirtyRect() const;
  RenderInvalidation ConsumePendingRenderInvalidation();
  void RequestQuit();
  bool ConsumeQuitRequested();
  float UiScale() const { return ui_scale_; }
  render::TextClipPadding PartialRedrawClipPadding() const;
  void SetWindowPresentationState(WindowPresentationState state);
  // Returns the last value pushed via SetWindowPresentationState. Used by the
  // outer app loop so it can skip re-querying SDL window/render dimensions on
  // every frame when nothing has invalidated the cached state.
  std::optional<WindowPresentationState> CurrentWindowPresentationState() const;
  void SetDialogWindow(SDL_Window* window) { dialog_window_ = window; }
  SDL_HitTestResult WindowHitTest(float x, float y) const;
  bool WindowDragRegionContains(float x, float y) const;
  bool MenuSurfaceCapturingMouse() const;
  WindowAction ConsumeWindowAction();
  // Force the next UpdateMouseCursor to re-apply our cursor to the platform even
  // if the cursor kind is unchanged. SDL's Wayland backend sets resize/draggable
  // cursors from our hit test (and re-asserts a stale one on pointer-enter)
  // without updating SDL's current-cursor bookkeeping, so the displayed cursor
  // can drift out of sync with what we intend. Call this on the events where SDL
  // may have changed the cursor behind our back: pointer-enter, focus-gain, and
  // after a window resize.
  void ForceCursorReassert();

  struct VisiblePopupMenuItem {
    std::size_t index = 0;
    SDL_FRect rect{};
    bool enabled = false;
    bool checked = false;
    bool hovered = false;
    bool separator = false;
  };

  // Geometry-only popup row info — no IsMenuItemEnabled/IsMenuItemChecked work.
  // Use this when only hit-testing or rect lookup is needed (mouse motion,
  // cursor kind probe). Caches matter: the heavy enabled/checked lookups are
  // O(items) each and were dominating per-event handler time at high mouse
  // rates.
  struct PopupRowGeometry {
    std::size_t index = 0;
    SDL_FRect rect{};
    bool separator = false;
  };

  static std::optional<PopupRowGeometry> HitTestPopupRow(
      std::span<const MenuItemSpec> items, const SDL_FRect& popup_rect, float x, float y);
  static std::optional<SDL_FRect> PopupRowRectByIndex(
      std::span<const MenuItemSpec> items, const SDL_FRect& popup_rect, std::size_t index);

  WorkspaceActionContext MakeActionContext();

#ifdef MICROIDE_TESTING
  struct TestAccess;
  friend struct TestAccess;
#endif

 private:
  class Bootstrapper;
  using FocusTarget = workspace::FocusTarget;
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
  using OutputPanelState = workspace::OutputPanelState;
  using ProjectWorkspaceState = workspace::ProjectWorkspaceState;
  using ProjectCatalogState = workspace::ProjectCatalogState;
  using WorkspaceContext = workspace::WorkspaceContext;
  using DragTarget = workspace::DragTarget;
  using TabDragKind = workspace::TabDragKind;
  using InteractionState = workspace::InteractionState;
  using TabDragState = workspace::TabDragState;
  using TreeContextMenuState = workspace::TreeContextMenuState;
  using MenuSurfaceState = workspace::MenuSurfaceState;
  using DirtyPromptState = workspace::DirtyPromptState;
  using PromptSurfaceState = workspace::PromptSurfaceState;
  using PromptState = workspace::PromptState;
  using TextInputSurface = workspace::TextInputSurface;
  using TextCompositionState = workspace::TextCompositionState;
  using TextInputState = workspace::TextInputState;

  enum class CursorKind {
    Default,
    Text,
    Pointer,
    EwResize,
    NsResize,
    NResize,
    EResize,
    SResize,
    WResize,
    NeResize,
    SeResize,
    SwResize,
    NwResize,
  };

 public:
  struct CompareSurfaceLayout {
    float line_height = 14.0f;
    float gutter_width = 28.0f;
    float divider_width = 18.0f;
    float left_width = 0.0f;
    float right_width = 0.0f;
    float left_x = 0.0f;
    float center_x = 0.0f;
    float right_x = 0.0f;
    float review_summary_y = 0.0f;
    float action_hint_y = 0.0f;
    float header_y = 0.0f;
    float rows_y = 0.0f;
    int visible_rows = 1;
    std::size_t left_visible_columns = 1;
    std::size_t right_visible_columns = 1;
    std::size_t visible_columns = 1;
    float min_divider_fraction = 0.5f;
    bool show_vertical = false;
    bool show_horizontal = false;
  };

  struct TextInputVisual {
    TextInputSurface surface = TextInputSurface::None;
    SDL_FRect area{};
    float text_x = 0.0f;
    float text_y = 0.0f;
    float cursor_x = 0.0f;
    float cursor_y = 0.0f;
    SDL_Color foreground{};
    SDL_Color background{};
    std::string displayed_text;
    std::optional<std::pair<std::size_t, std::size_t>> selection_bytes;
  };

  struct SingleLineViewMetrics {
    std::string displayed_text;
    float cursor_x = 0.0f;
    std::optional<std::pair<std::size_t, std::size_t>> selection_bytes;
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
    float min_divider_fraction = 1.0f / 3.0f;
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

 public:
  // Geometry of a scrollable text/list surface (content rect + text origin +
  // scroll surface). Shared by the bottom panel (terminal/output) and the
  // right-side debug pane; computed by ComputeBottomPanelLogLayout /
  // ComputeDebugPaneListLayout respectively.
  struct LogSurfaceLayout {
    SDL_FRect content_rect{};
    float text_x = 0.0f;
    float text_y = 0.0f;
    float text_width = 0.0f;
    float line_height = 0.0f;
    ScrollSurfaceLayout scroll;
  };

  using VisibleStripTab = workspace::VisibleStripTab;
  using TabStripOverflowControls = workspace::TabStripOverflowControls;

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

  struct VisibleStatusItem {
    StatusItemView item;
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

  using BottomPanelTabKind = workspace::BottomPanelTabKind;
  using BottomPanelTabModel = workspace::BottomPanelTabModel;

 private:

  struct EditorBlamePopupLayout {
    std::size_t line_index = 0;
    SDL_FRect rect{};
    SDL_FRect copy_sha_rect{};
  };

  // Debug hover-to-inspect popup payload (Phase 5): the evaluated value + its type
  // for the identifier under the cursor while a session is Stopped. Strings are
  // prebuilt by DebugService (off the render path).
  struct DebugHoverValue {
    std::string value;
    std::string type;
    bool operator==(const DebugHoverValue&) const = default;
  };

  struct EditorHoverTarget {
    enum class Kind {
      Blame,
      Diagnostic,
      Plugin,
      DebugValue,
    };

    Kind kind = Kind::Blame;
    SDL_FRect anchor_rect{};
    std::size_t blame_line_index = 0;
    std::optional<editor::PublishedDiagnostic> diagnostic;
    std::optional<plugin::PluginHost::HoverResult> plugin_hover;
    std::optional<DebugHoverValue> debug_value;

    bool operator==(const EditorHoverTarget& other) const {
      return kind == other.kind && anchor_rect.x == other.anchor_rect.x &&
             anchor_rect.y == other.anchor_rect.y && anchor_rect.w == other.anchor_rect.w &&
             anchor_rect.h == other.anchor_rect.h &&
             blame_line_index == other.blame_line_index && diagnostic == other.diagnostic &&
             plugin_hover == other.plugin_hover && debug_value == other.debug_value;
    }
  };

  struct EditorHoverPopupLayout {
    EditorHoverTarget::Kind kind = EditorHoverTarget::Kind::Blame;
    SDL_FRect anchor_rect{};
    SDL_FRect rect{};
    std::size_t blame_line_index = 0;
    std::optional<editor::PublishedDiagnostic> diagnostic;
    std::optional<plugin::PluginHost::HoverResult> plugin_hover;
    std::optional<DebugHoverValue> debug_value;
    std::optional<SDL_FRect> primary_action_rect;
  };

  // Recorded by the (const) debug-value hover resolver when it classifies a cache
  // MISS, and consumed by the non-const UpdateEditorHover to kick off the async
  // `evaluate` request. Keeps the const resolver pure-read (no DAP I/O); the
  // instance-scoped, cleaner analog of the thread_local cache the plugin hover uses.
  struct PendingHoverEval {
    int frame_id = 0;
    std::string expression;
    bool valid = false;
  };
  mutable PendingHoverEval pending_hover_eval_;

  enum class ProjectOpenDialogLaunchResult {
    Launched,
    AlreadyOpen,
    Unavailable,
  };

  static constexpr float kProjectSearchButtonTop = 34.0f;
  static constexpr float kProjectSearchQueryTop = 58.0f;
  static constexpr float kProjectSearchReplaceTop = 82.0f;
  static constexpr float kProjectSearchButtonHeight = 18.0f;
  static constexpr float kProjectSearchStatusTop = 106.0f;
  static constexpr float kProjectSearchResultsTop = 124.0f;

  static std::span<const ActionSpec> ActionSpecs();
  static const ActionSpec* FindActionSpec(ActionId id);
  static const ActionSpec* FindActionByCommand(std::string_view command_name);
  static const std::vector<std::string>& CommandNames();
  static std::span<const MenuSpec> MenuSpecs();
  static const MenuSpec* FindMenuSpec(MenuId id);
  std::span<const MenuItemSpec> MenuItems(MenuId id) const;
  static std::span<const MenuItemSpec> TreeContextMenuItems(TreeContextTargetKind target);
  std::vector<VisibleMenuBarItem> ComputeVisibleMenuBarItems(const SDL_FRect& menu_bar) const;
  std::vector<MenuId> ComputeOverflowMenuBarItems(const SDL_FRect& menu_bar) const;
  // True when a top-level menu should appear in the menu bar / hamburger overflow.
  // Gates MenuId::Debug on the `debug.enabled` setting.
  bool IsMenuBarMenuVisible(MenuId id) const;
  std::optional<SDL_FRect> MenuOverflowChevronRect(const SDL_FRect& menu_bar) const;
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
  bool ExecuteCustomMenuItem(MenuId id, std::size_t item_index);
  bool ExecuteCommandName(std::string_view command_name,
                          const std::vector<std::string>& args,
                          ActionSource source,
                          std::string* error_message = nullptr);
  std::optional<std::string> GetSettingValue(std::string_view id) const;
  const std::vector<ResolvedKeybinding>& ResolvedKeybindings() const;
  std::vector<std::string> OrderedSidebarViewIds() const;
  void NormalizeSidebarViewSelection();
  void RebuildPhase3Registries(bool reconcile_language_servers = true);
  void RebuildPhase4Registries();
  std::vector<VisibleStatusItem> ComputeVisibleStatusItems(const SDL_FRect& breadcrumb) const;
  std::string HoveredStatusTooltip(const SDL_FRect& breadcrumb) const;
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
  ProjectCatalogService MakeProjectCatalogService();
  PersistenceCoordinator MakePersistenceCoordinator();
  MenuCoordinator MakeMenuCoordinator();
  CommandPromptCoordinator MakeCommandPromptCoordinator();
  PromptSurfaceService MakePromptSurfaceService();
  DirtyPromptCoordinator MakeDirtyPromptCoordinator(EditorTabService& editor_tabs,
                                                    PromptSurfaceService& prompt_surfaces);
  CompareInteractionCoordinator MakeCompareInteractionCoordinator();
  CompareMergeService MakeCompareMergeService();
  PathMutationCoordinator MakePathMutationCoordinator(EditorTabService& editor_tabs,
                                                      PromptSurfaceService& prompt_surfaces);
  DiffTabCoordinator MakeDiffTabCoordinator();
  LifecycleCoordinator MakeLifecycleCoordinator();
  SidebarCoordinator MakeSidebarCoordinator();
  SidebarService MakeSidebarService();
  DebugPaneService MakeDebugPaneService();
  KeyInputCoordinator MakeKeyInputCoordinator();
  TextInputCoordinator MakeTextInputCoordinator();
  EditorTabService MakeEditorTabService();
  TabCoordinator MakeTabCoordinator();
  ChromeMouseCoordinator MakeChromeMouseCoordinator();
  EditorMouseCoordinator MakeEditorMouseCoordinator();
  CompareMouseCoordinator MakeCompareMouseCoordinator();
  MergeMouseCoordinator MakeMergeMouseCoordinator();
  TabMouseCoordinator MakeTabMouseCoordinator();
  bool HandleTabMouseMotion(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleTabMouseButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleTabMouseButtonUp(const SDL_Event& event);
  bool HandleTabMouseWheel(const SDL_Event& event, const WorkspaceLayout& layout, int vertical_ticks);
  SidebarMouseCoordinator MakeSidebarMouseCoordinator();
  PanelMouseCoordinator MakePanelMouseCoordinator();
  DebugPaneMouseCoordinator MakeDebugPaneMouseCoordinator();
  TerminalPanelService MakeTerminalPanelService();
  void ResetLifecycleStartupState();
  void RegisterLifecycleWakeEvents();
  void DestroyLifecycleCursors();
  void ConsumeHighlightPrefetchResults();  // installs worker prefetch results (main thread)
  void RebindProjectState(ProjectWorkspaceState& state);
  void ResetCurrentProjectStateStorage();
  void ResetTransientInteractionState();
  void ClearDragState();
  bool HasActiveProjectCatalogEntry() const;
  ProjectWorkspaceState* ProjectCatalogEntry(std::size_t index);
  const ProjectWorkspaceState* ProjectCatalogEntry(std::size_t index) const;
  std::filesystem::path ProjectCatalogRoot(std::size_t index) const;
  void ResetProjectCatalogToWelcomeState();
  bool ReloadPluginsForCurrentProject(PluginReloadRequest request = {});
  void RefreshPluginSurfacesForReactivation();
  bool ReloadPluginsIfPluginAssetsChanged(bool force_check);
  bool ReloadProjectIfFilesChanged(bool force_check);
  void ApplyProjectChangeBatch(const project::ProjectChangeBatch& batch);
  void MarkCompareTabsStaleForPath(const std::filesystem::path& path);
  void InvalidateMergeTabsForPath(const std::filesystem::path& path);
  void ActivateEditorBannerAction(EditorBannerAction action, const std::filesystem::path& path);
  void ClearDiagnosticsForPath(const std::filesystem::path& path);
  void RequestGitSidebarRefresh(GitSidebarRefreshScope scope = GitSidebarRefreshScope::Full);
  void MaybeRequestTreeGitBadgesAfterFirstPaint();
  void RequestAutomaticGitSidebarRefresh();
  bool ConsumePendingGitSidebarRefreshSnapshot(GitSidebarState::RefreshSnapshot* snapshot);
  void InvalidateRuntimeSyntaxStateCaches(std::span<const std::string_view> changed_languages);
  std::string PluginRuntimeReloadSummary() const;
  void NotifyPluginsAboutOpenBuffers(bool replay_plugin_buffer_opens = true,
                                     bool open_lsp_documents = true);
  bool PrepareEditorViewportForSave(const std::filesystem::path& path,
                                    editor::TextViewport& viewport,
                                    std::string* error_message = nullptr);
  bool OpenVirtualDocumentInNewTab(std::string_view uri);
  void ReloadVirtualDocumentTabs(std::string_view uri);
  bool IsReadOnlyVirtualDocument(const std::filesystem::path& path) const;
  void NotifyPluginBufferOpen(const std::filesystem::path& path);
  void NotifyPluginBufferSave(const std::filesystem::path& path);
  void NotifyLspBufferClose(const std::filesystem::path& path);
  void ResetProjectScopedState(bool show_welcome);
  bool StartFileIndexWatcherForCurrentProject();
  void StopFileIndexWatcher();
  void SetWelcomePlaceholder();
  bool InitializeCurrentProject(const std::filesystem::path& project_root,
                                bool restore_persistence,
                                bool log_feedback,
                                bool activate_restored_tab = true);
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
  std::string PromptSurfaceDetail() const;
  std::vector<std::string> PromptSurfaceActionLabels() const;
  std::filesystem::path TreeMutationBasePath(ActionSource source) const;
  bool HasDirtyEditorTabsForPath(const std::filesystem::path& path,
                                 std::string* blocking_label = nullptr) const;
  void CloseOpenTabsForPath(const std::filesystem::path& path);
  std::filesystem::path EditorViewPath(const TabEntry::EditorTabState& editor_state) const;
  bool ActivateCurrentTabAfterStateLoad();
  bool ActiveTabIsEditor() const;
  TabEntry::EditorTabState* ActiveEditorTab();
  const TabEntry::EditorTabState* ActiveEditorTab() const;
  static TabEntry::EditorTabState MakeEditorTabState(const editor::TextViewport& view);
  void SyncActiveEditorTabMetadata();
  bool ReplaceActiveEditorView(const editor::TextViewport& viewport);
  editor::TextViewport* ActiveEditorViewport();
  const editor::TextViewport* ActiveEditorViewport() const;
  editor::TextViewport* ActiveNavigableViewport();
  const editor::TextViewport* ActiveNavigableViewport() const;
  std::filesystem::path ActiveTabPath() const;
  std::vector<EditorPaneLayout> ComputeEditorPaneLayouts(const SDL_FRect& editor_surface) const;
  std::vector<EditorSplitDividerLayout> ComputeEditorSplitDividerLayouts(
      const SDL_FRect& editor_surface) const;
  SDL_FRect EditorSurfaceBelowBanner(const SDL_FRect& editor_surface) const;
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
  std::optional<SDL_FRect> ComputeMergeSecondaryToolbarButtonRect(const SDL_FRect& rect,
                                                                  const MergeSurfaceLayout& surface,
                                                                  std::string_view label) const;
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
  void OpenOutgoingBaseRefPicker();
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
  // Launch-config picker (Phase 9): open the fuzzy picker over the project's launch
  // configs, re-filter on query change, and start a session for the chosen config.
  void OpenLaunchConfigPicker();
  void RefreshLaunchConfigPicker();
  void ConfirmLaunchConfigSelection();
  void OpenCommandPalette();
  void RefreshCommandPalette();
  void ConfirmCommandPaletteSelection();
  // Build the welcome-surface model + geometry for the current placeholder editor, so
  // the click handler and cursor resolver hit-test the same regions the renderer drew.
  // Returns false when the welcome home surface is not currently showing.
  bool ProbeWelcomeSurface(editor::WelcomeViewModel* model,
                           editor::WelcomeLayout* layout) const;
  void OpenComparison(const project::GitCommitEntry& commit);
  void OpenWorkingFileFromCompare();
  void MoveCompareSelection(int delta);
  void JumpCompareHunk(int delta);
  void MarkActiveBranchFileReviewed();
  void MarkActiveBranchHunkReviewed();
  void ClearActiveBranchReviewState();
  void EditActiveBranchReviewNote(const std::string& note_text);
  void PersistBranchReviewState();
  void StageCompareHunk();
  void StageCompareSelectedLines();
  void UnstageCompareHunk();
  void UnstageCompareSelectedLines();
  void OpenDiscardCompareHunkPrompt();
  void OpenDiscardCompareSelectedLinesPrompt();
  void RefreshOpenCompareTabsForPath(const std::filesystem::path& path);
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
  void ResetMergeHunk();
  void JumpNextUnresolvedMergeConflict();
  void ToggleMergeBasePane();
  void ToggleMergeRawMarkers();
  void CopyMergeIncomingSnippet();
  void CopyMergeCurrentSnippet();
  void MarkMergeResolved();
  void OpenMergeResultFile();
  void MoveFileFinderSelection(int delta);
  void RenderCompareSurface(SDL_Renderer* renderer,
                            const SDL_FRect& rect,
                            CompareTabState& compare_tab,
                            const std::filesystem::path& project_root,
                            bool draw_compare_caret,
                            const editor::DiagnosticsStore& diagnostics_store);
  void RenderMergeSurface(SDL_Renderer* renderer,
                          const SDL_FRect& rect,
                          const std::filesystem::path& project_root,
                          bool draw_caret,
                          const editor::DiagnosticsStore& diagnostics_store);
  void ShowSidebarMode(SidebarMode mode, bool temporary = false);
  void ShowTreeSidebar(const std::filesystem::path& root = {});
  void ShowSearchSidebar(std::string query = {}, bool temporary = false);
  void ShowProblemsSidebar();
  void ShowGitSidebar();
  void ShowTestsSidebar();
  bool ShowPluginSidebar(std::string_view id, bool temporary = false);
  // Right-side debug pane convenience wrappers (forward to MakeDebugPaneService()).
  void ShowDebugPaneMode(DebugPaneMode mode);
  void ToggleDebugPane();
  void CloseDebugPane();
  void OpenDebugPaneOnStop();
  SidebarModeRowLayout SidebarModeRow(const SDL_FRect& sidebar_rect) const;
  std::string HoveredSidebarModeTooltipLabel(const SDL_FRect& sidebar_rect) const;
  void CloseSidebar();
  void ToggleSidebar();
  void RestorePreviousSidebar();
  void RefreshProjectFiles();
  void RefreshGitSidebar();
  void ConsumeGitSidebarRefresh();
  bool RefreshProblemsSidebar();
  bool RefreshTestsSidebar();
  bool RefreshTestsSidebarState();
  bool RefreshPluginSidebar();
  SDL_FRect TreeSidebarCollapseButtonRect(const SDL_FRect& sidebar_rect) const;
  SDL_FRect TreeSidebarRefreshButtonRect(const SDL_FRect& sidebar_rect) const;
  SDL_FRect GitSidebarActionRowRect(const SDL_FRect& sidebar_rect) const;
  SDL_FRect GitSidebarCommitButtonRect(const SDL_FRect& sidebar_rect) const;
  bool CanOpenGitCommitButton() const;
  SDL_FRect GitSidebarRefreshButtonRect(const SDL_FRect& sidebar_rect) const;
  SDL_FRect GitSidebarStageAllButtonRect(const SDL_FRect& sidebar_rect) const;
  SDL_FRect GitSidebarDiscardAllButtonRect(const SDL_FRect& sidebar_rect) const;
  std::optional<SDL_FRect> GitSidebarOutgoingBaseButtonRect(const SDL_FRect& sidebar_rect) const;
  std::vector<std::string> GitSidebarSummaryLines() const;
  float GitSidebarSummaryHeight() const;
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
  ScrollableListLayout ComputeTestsSidebarListLayout(const SDL_FRect& sidebar_rect,
                                                     std::size_t line_count) const;
  ScrollableListLayout ComputePluginSidebarListLayout(const SDL_FRect& sidebar_rect,
                                                      std::size_t line_count) const;
  std::vector<GitSidebarLine> BuildGitSidebarLines() const;
  bool ToggleGitSidebarDirectoryCollapsed(const std::string& tree_node_key);
  GitSidebarEntryActionLayout ComputeGitSidebarEntryActionLayout(const SDL_FRect& row_rect,
                                                                 const GitSidebarEntry& entry) const;
  std::optional<std::size_t> SelectedGitSidebarLineIndex() const;
  const GitSidebarEntry* SelectedGitSidebarEntry() const;
  void RevealSelectedTreeSidebarLine();
  void RevealSelectedGitSidebarLine();
  void RevealSelectedProblemsSidebarLine();
  void RevealSelectedTestsSidebarLine();
  void RevealSelectedPluginSidebarLine();
  void MoveGitSidebarSelection(int delta);
  void MoveProblemsSidebarSelection(int delta);
  void MoveTestsSidebarSelection(int delta);
  void MovePluginSidebarSelection(int delta);
  bool OpenGitSidebarEntry(std::size_t entry_index);
  bool OpenSelectedProblemSidebarItem();
  bool OpenSelectedTestSidebarItem();
  bool OpenSelectedPluginSidebarItem();
  bool RunSelectedTestSidebarItem();
  bool CanStageAllGitSidebarEntries() const;
  bool CanDiscardAllGitSidebarEntries() const;
  bool StageAllGitSidebarEntries();
  void OpenDiscardAllGitSidebarPrompt();
  void InitializeCommitWorkflowService();
  bool OpenCommitWorkflow();
  void CloseCommitWorkflow();
  bool CommitWorkflowOpen() const;
  bool RequestCommitWorkflowCommit();
  float GitSidebarCommitWorkflowHeight() const;
  bool DiscardAllGitSidebarEntries();
  bool StageGitSidebarEntry(std::size_t entry_index);
  bool UnstageGitSidebarEntry(std::size_t entry_index);
  bool DiscardGitSidebarEntry(std::size_t entry_index);
  void OpenDiscardGitSidebarEntryPrompt(std::size_t entry_index);
  bool DispatchGitSidebarAction(GitSidebarActionId action, std::size_t entry_index);
  void ReconcileOpenTabsAfterPathDiscard(const std::filesystem::path& path);
  void SetGitOutgoingBaseChoice(OutgoingBaseChoice choice);
  void ReloadCleanEditorTabsForPath(const std::filesystem::path& path);
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
  // Debug hover-to-inspect (Phase 5). Resolves the identifier under (x,y) in the
  // focused frame's source file and serves a cached `evaluate` value, recording a
  // PendingHoverEval on a cache miss so UpdateEditorHover can kick off the request.
  std::optional<EditorHoverTarget> DebugValueHoverTargetAtPosition(float x, float y) const;
  std::optional<EditorHoverTarget> DebugValueHoverTargetForViewport(
      const editor::TextViewport& viewport,
      const TextGridInteractionLayout& interaction,
      float x,
      float y) const;
  std::optional<EditorHoverPopupLayout> ActiveEditorHoverPopupLayout() const;
  SDL_FRect EditorHoverPopupPrimaryActionHitRect(const EditorHoverPopupLayout& popup) const;
  bool EditorHoverPopupPrimaryActionHovered(float x, float y) const;
  std::vector<std::string> WrapEditorHoverPopupText(std::string_view text,
                                                    float max_width,
                                                    std::size_t max_lines) const;
  // Shared "secondary title line(s) + gap + primary content line(s)" hover-card
  // shape used by the Plugin (title/content) and DebugValue (type/value) popups.
  // Compute the positioned card rect, and draw the two stacked text blocks.
  SDL_FRect ComputeTwoBlockHoverCardRect(std::string_view title,
                                         std::string_view content,
                                         std::size_t max_title_lines,
                                         std::size_t max_content_lines,
                                         const SDL_FRect& anchor_rect,
                                         const SDL_FRect& editor_surface) const;
  void DrawTwoBlockHoverCard(SDL_Renderer* renderer,
                             const SDL_FRect& card_rect,
                             std::string_view title,
                             std::string_view content,
                             std::size_t max_title_lines,
                             std::size_t max_content_lines) const;
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
  void FinalizeGitMergeTab(MergeTabState& merge_tab, const std::filesystem::path& path);
  void InvalidateStaleMergeTabs();
  bool OpenGitConflictMerge(const std::filesystem::path& path);
  void OpenBufferSearch();
  void OpenBufferReplace();
  void OpenBufferSearchSurface(OverlayMode mode);
  std::string SingleLineEditorSelectionSeed() const;
  void OpenProjectSearch();
  FocusTarget PrimarySurfaceFocusTarget() const;
  void ShowOverlay(OverlayMode mode);
  void DismissOverlay(bool focus_editor = false);
  void RevealBufferSearchMatch(const editor::SelectionRange& match);
  void ResetBufferSearchFoldRevealState(bool preserve_expanded_folds);
  void RefreshBufferSearch();
  void OpenBufferSearchFromProjectSearchResult();
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
  bool DiscoverTestsForActiveBuffer(std::string* error_message = nullptr);
  bool RunTests(const std::vector<std::string>& test_ids, std::string* error_message = nullptr);
  bool RunAllDiscoveredTests(std::string* error_message = nullptr);
  const std::vector<WorkspaceOutputChannels::ChannelInfo>& OutputChannels() const;
  const std::vector<std::string>* OutputChannelEntries(std::string_view id) const;
  void ConsumeLspCallbacks();
  bool ConsumePluginAsyncProcessCallbacks();
  void OpenTerminal(std::string command, bool focus_terminal = true, bool log_feedback = true);
  bool ReopenActiveTab();
  std::filesystem::path ConfigStatePath() const;
  std::filesystem::path UserConfigPath() const;
  std::filesystem::path ProjectStateDirectory() const;
  void ApplyEditorPreferences(editor::TextViewport& viewport) const;
  void ApplyDetectedIndentOnOpen(editor::TextViewport& viewport) const;
  void ApplyEditorPreferencesToAllTabs();
  // Refresh the active editor tab's `editor::FoldingModel` from the active
  // viewport, the resolved language contract, and the current `editor.fold.enabled`
  // setting. Returns the (now-fresh) model pointer or nullptr when no editor
  // tab is active. Callers (render path, fold-action handlers) consume the
  // model directly.
  editor::FoldingModel* EnsureActiveFoldingModelFresh();
  bool WriteClipboardText(std::string_view text) const;
  std::optional<std::string> ReadClipboardText() const;
  bool WritePrimarySelectionText(std::string_view text) const;
  std::optional<std::string> ReadPrimarySelectionText() const;
  std::optional<std::string> SelectionTextWithContext();
  void SyncPrimarySelectionWithActiveEditor();
  void SyncPrimarySelectionWithTerminalSelection();
  std::size_t CountOpenBufferViews(const std::filesystem::path& path) const;
  // Thin forwarders to lsp_service_ (see src/workspace/LspService.*).
  bool HasActiveCompletionProvider() const;
  bool HasActiveCodeActionProvider() const;
  bool HasActiveDefinitionProvider() const;
  bool HasActiveReferencesProvider() const;
  LspManager& CurrentLspManager();
  const LspManager& CurrentLspManager() const;
  LspManager& EnsureProjectLspManager(ProjectWorkspaceState& state);
  // Thin forwarders to debug_service_ (see src/workspace/DebugService.*).
  DapManager& CurrentDapManager();
  const DapManager& CurrentDapManager() const;
  DapManager& EnsureProjectDapManager(ProjectWorkspaceState& state);
  void ConsumeDapCallbacks();
  // Control channel (live remote control). ConsumeControlCallbacks drains the
  // socket server on the control wake event; ExecuteControlCommand runs one
  // command line through the in-app command chokepoint and reports the outcome;
  // ApplyControlSpec applies a cold-start spec; MaybeStartControlChannel starts
  // the listener when the `control.enabled` setting is on.
  void ConsumeControlCallbacks();
  ControlChannelService::CommandOutcome ExecuteControlCommand(const std::string& command_line);
  void MaybeStartControlChannel();
  bool StartDebugging(const LaunchConfig& config, const std::string& cwd = {});
  // Builds a minimal launch config from a registered adapter and starts it.
  // Returns an error string (empty on success). Launch-config selection /
  // persistence is a later phase; Phase 1 launches the first registered adapter.
  std::string StartDebuggingWithDefaultConfig();
  // Start a session for the launch config named `name` (case-insensitive). An
  // empty name defers to StartDebuggingWithDefaultConfig(). Returns an error
  // string (empty on success). Drives the `debug-launch` command / spec field.
  std::string StartNamedDebugConfig(const std::string& name);
  // Ad-hoc launch by program path (the `debug-run` command). Synthesizes a
  // transient LaunchConfig from `program` (resolved against the project root) +
  // `args`, using `type` (or the sole registered adapter when empty), then starts
  // it. Returns an error string (empty on success). No pre-defined config needed.
  std::string StartAdHocDebug(const std::string& program, const std::vector<std::string>& args,
                              const std::string& type);
  void StopDebugging();
  void ResendBreakpointsForFile(const std::filesystem::path& path);
  bool IsDebugSessionActive() const;
  bool IsDebugSessionStopped() const;
  // Canonical read of the master `debug.enabled` toggle (default off). The one
  // place that interprets the stored setting string as a bool for shell code.
  bool DebugEnabled() const;
  // Turn the debugger on transiently (never persisted) when it is off. Shared by
  // the cold-start spec and the control channel so a headless driver never needs
  // a separate `set-setting debug.enabled true` prelude before breakpoint-/debug-
  // commands. Returns true when it flipped the setting on.
  bool EnsureDebuggerEnabledTransiently();
  // Execution control (Phase 3); forward to debug_service_ / the active session.
  void DebugContinue();
  void DebugStepOver();
  void DebugStepIn();
  void DebugStepOut();
  void DebugPause();
  // Reverse execution; forward to debug_service_ (no-op unless the adapter
  // advertises `supportsStepBack`). DebugSupportsReverse reports the capability,
  // gating the reverse commands + their toolbar buttons.
  void DebugReverseContinue();
  void DebugStepBack();
  bool DebugSupportsReverse() const;
  // Restart the active session (Phase 7); DAP `restart` or terminate+relaunch.
  void DebugRestart();
  // Focus a thread in the Call Stack thread selector (Phase 7 multi-thread).
  void DebugFocusThread(int thread_id);
  // Make a session active (by id, for the Call Stack session-row click) or switch
  // by 1-based index / cycle (index < 0) for the command (Phase 8 multi-session).
  void DebugFocusSession(int session_id);
  void DebugSwitchSession(int index);
  // Breakpoint-modifier context menu (Phase 6). Read the breakpoint gutter
  // menu's target path+line; open a seeded prompt for a condition/hit/log field
  // (mapped from the ActionId) or remove the breakpoint and live-resend.
  void OpenBreakpointContextMenu(const std::filesystem::path& path, std::size_t line,
                                 const SDL_FRect& anchor_rect);
  void EditBreakpointModifierFromMenu(ActionId id);
  // Prompt-less gutter-menu actions: toggle enabled / clear the condition.
  void BreakpointQuickActionFromMenu(ActionId id);
  void RemoveBreakpointFromMenu();
  // Commit a SetBreakpoint* prompt: write the field (empty clears) + live-resend.
  void CommitBreakpointModifierPrompt();
  // Watch panel (Phase 6): open the add/edit-expression prompt (nullopt = add),
  // and commit it into the watch model (edit with empty text removes).
  void OpenWatchExpressionPrompt(std::optional<std::size_t> index);
  void CommitWatchExpressionPrompt();
  // Debug-console REPL (Phase 9): open the single-line evaluate prompt, and commit
  // it (evaluate in the active session, then re-open for the next expression).
  void OpenDebugReplPrompt();
  void CommitDebugReplPrompt();
  void AppendDebugConsoleOutput(int session_id, const std::string& label,
                                const dap_protocol::DapOutputEvent& output);
  void ShowDebugConsole(int session_id, const std::string& label);
  // Surface the active debug session's console output in the bottom panel on
  // demand (Debug → Show Output / Ctrl+Shift+5). No-op without an active session.
  void ShowDebugOutput();
  // Remove a pruned session's console channel + close its tab (Phase 10).
  void RemoveDebugConsole(int session_id);
  // Stop every live debug session and close the debug pane (Phase 10).
  void StopAllDebugSessions();
  LspClient::ReadinessSnapshot ActiveLspReadinessSnapshot(bool ensure_started = true);
  void ActiveLspStatusStrings(bool ensure_started, std::string& text, std::string& tooltip);
  std::string ActiveLspStatusText(bool ensure_started = true);
  void BeginTrackedLspRequest();
  void FinishTrackedLspRequest();
  LspClient* LspClientForViewport(const editor::TextViewport& viewport,
                                  std::string* language_id = nullptr);
  void EnsureLspDocumentOpen(const editor::TextViewport& viewport,
                             LspClient& client,
                             std::string_view language_id);
  void PublishLspDiagnostics(ProjectWorkspaceState& state,
                             std::string uri,
                             std::vector<LspClient::Diagnostic> diagnostics);
  void SyncLspForActiveEditableLastChange();
  void SyncLspForActiveEditableChange(const std::vector<std::string>& before_lines,
                                      const std::vector<std::string>& after_lines);
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
  bool MoveActiveOutputTabTo(std::size_t index);
  void CloseTerminalTab(std::size_t index);
  void ConsumeTerminalSessionUpdates();
  void ReapExitedTerminalTabs();
  bool BottomPanelVisible() const;
  bool BottomPanelShowsTerminal() const;
  bool BottomPanelShowsOutput() const;
  LogSurfaceLayout ComputeBottomPanelLogLayout(const WorkspaceLayout& layout,
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
  void OpenExternalUrlPrompt(std::string url);
  bool RevealPathInFileExplorer(const std::filesystem::path& directory) const;
  void ResizeTerminalToPanel(const SDL_FRect& panel_rect);
  bool OpenUntitledTab();
  bool OpenFileInNewTab(const std::filesystem::path& path);
  bool MoveActiveTabTo(std::size_t index);
  std::optional<std::size_t> FindTabIndexBySpecifier(std::string_view specifier,
                                                     std::string* error_message = nullptr) const;
  void OpenFile(const std::filesystem::path& path);
  void OpenFileAtLocation(const std::filesystem::path& path,
                          std::size_t line,
                          std::size_t column);
  bool HandleMouseButtonDown(const SDL_Event& event);
  bool HandleMouseButtonUp(const SDL_Event& event);
  bool HandleMouseMotion(const SDL_Event& event);
  bool HandleMouseWheel(const SDL_Event& event);
  ActionAvailability MakeActionAvailability() const;
  WorkspaceRootView MakeRootView();
  void DrawFilledRect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) const;
  void DrawRect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) const;
  void RenderFrameBase(SDL_Renderer* renderer,
                       const WorkspaceLayout& layout,
                       const FrameSurfaceViewModel& frame_vm) const;
  void RenderActiveWorkspaceSurface(SDL_Renderer* renderer,
                                    const WorkspaceLayout& layout,
                                    const FrameToken& frame_token,
                                    bool draw_editor_caret,
                                    std::optional<SDL_FRect>* active_editor_pane_rect,
                                    const FrameSurfaceViewModel& frame_vm,
                                    const OverlaySurfaceViewModel& overlay_vm);
  void RenderWindowChrome(SDL_Renderer* renderer, const WorkspaceLayout& layout) const;
  void RenderChromeTooltips(SDL_Renderer* renderer, const WorkspaceLayout& layout) const;
  void RenderSidebarSurface(SDL_Renderer* renderer, const WorkspaceLayout& layout);
  void RenderOverlaySurface(SDL_Renderer* renderer,
                            const WorkspaceLayout& layout,
                            const OverlaySurfaceViewModel& overlay_vm);
  void RenderFindWidget(SDL_Renderer* renderer, const WorkspaceLayout& layout,
                        const OverlaySurfaceViewModel& overlay_vm);
  // Floating, icon-only debug control bar (Continue/Pause, Step Over/In/Out,
  // Restart, Stop) anchored top-right of the editor area while a session is
  // active. Mirrors the find-widget pattern. Defined in
  // WorkspaceShellDebugToolbar.cpp (not a render-named TU, so it may read state
  // for the hit-test alongside the render).
  void RenderDebugToolbar(SDL_Renderer* renderer, const WorkspaceLayout& layout,
                          bool session_stopped, std::optional<float> avoid_below_y) const;
  // y below which the toolbar must sit (find widget's bottom when the buffer
  // search/replace widget is visible), so the two floating bars stack instead of
  // overlapping. std::nullopt anchors the toolbar at the editor top.
  std::optional<float> DebugToolbarAvoidBelowY(const WorkspaceLayout& layout) const;
  // Left-click hit-test for the debug toolbar. Returns true (consuming the click)
  // when the bar is showing and the point lands on it; dispatches the button's
  // action. No-op when debugging is disabled or no session is active.
  bool HandleDebugToolbarButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  // Whether the debug toolbar is currently shown (debugger enabled + active
  // session). Used to gate rendering and hit-testing.
  bool DebugToolbarVisible() const;
  void RenderBottomPanelSurface(SDL_Renderer* renderer,
                                const WorkspaceLayout& layout,
                                std::size_t terminal_line_count);
  // Right-side debug pane (Call Stack / Variables / Watch / Breakpoints). Defined in
  // DebugPaneRender.cpp; the four structured debug surfaces moved here from the
  // bottom panel.
  void RenderDebugPaneSurface(SDL_Renderer* renderer, const WorkspaceLayout& layout);
  DebugPaneModeRowLayout DebugPaneModeRow(const SDL_FRect& pane_rect) const;
  // List geometry below the mode-row, anchored to layout.right_pane. Reuses the
  // LogSurfaceLayout shape (content rect + text origin + scroll surface).
  LogSurfaceLayout ComputeDebugPaneListLayout(const WorkspaceLayout& layout,
                                                  std::size_t line_count) const;
  // Row count + clamped scroll for the currently active debug-pane surface.
  std::size_t DebugPaneActiveRowCount() const;
  // True when a click on `row` of the active surface does something (frame/thread/
  // session focus, variable/watch expand or edit, breakpoint navigate, filter
  // toggle). Drives the hover-cursor pointer/default decision for the pane.
  bool DebugPaneRowIsActionable(std::size_t row) const;
  int DebugPaneScrollRow(std::size_t line_count, int visible_rows) const;
  void SetDebugPaneScrollRow(int scroll_row, std::size_t line_count, int visible_rows);
  void RenderMenuPopups(SDL_Renderer* renderer, const WorkspaceLayout& layout) const;
  void RenderStatusBar(SDL_Renderer* renderer, const WorkspaceLayout& layout) const;
  void RenderNotifications(SDL_Renderer* renderer, const WorkspaceLayout& layout) const;
  void RefreshStatusBar();
  void ApplyLiveSettings();
  // Write a setting through the canonical chokepoint. When persist is false the
  // value is applied live (UpsertSetting + ApplyLiveSettings) but never saved to
  // disk, and the id is recorded so the persistence coordinator strips it on
  // shutdown — the transient path used by `--set`, cold-start spec settings, and
  // debug auto-enable so headless drives never clobber the user's saved config.
  bool SetSettingValue(std::string_view id, std::string value, bool persist = true);
  bool ResetSettingValue(std::string_view id);
  void RefreshSettingsOverlayCatalog();
  void OpenSettingsOverlay();
  void OpenHelpAboutOverlay();
  void CloseSettingsOverlay();
  void RenderSettingsOverlay(SDL_Renderer* renderer, const WorkspaceLayout& layout) const;
  bool HandleSettingsOverlayButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  void StepSetting(std::string_view id, bool forward);
  void TogglePluginEnabled(std::string_view plugin_id);
  void EnsureSettingsSelectionVisible();
  bool HandleStatusBarButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  void RenderPromptSurface(
      SDL_Renderer* renderer,
      const WorkspaceLayout& layout,
      const std::optional<TextInputVisual>& active_text_input_visual) const;
  void RenderDirtyPromptSurface(SDL_Renderer* renderer, const WorkspaceLayout& layout) const;
  void RenderActiveTextInputCaret(SDL_Renderer* renderer,
                                  const std::optional<TextInputVisual>& visual) const;
  void RenderSingleLineTextSelection(SDL_Renderer* renderer,
                                     const std::optional<TextInputVisual>& visual) const;
  std::optional<TextInputVisual> BuildActiveTextInputVisual(
      const WorkspaceLayout& layout,
      const std::optional<SDL_FRect>& active_editor_pane_rect) const;
  SingleLineViewMetrics ComputeSingleLineViewMetrics(const editor::SingleLineEditor& state,
                                                     std::string_view prefix,
                                                     float available_width) const;
  // Maps a pointer x-offset (relative to the input's text_x) to a byte position within
  // state.text(). Uses the same view-start computation as the renderer so the caret lands
  // under the glyph the user clicked even when the text is horizontally scrolled.
  std::size_t HitTestSingleLineByteOffset(const editor::SingleLineEditor& state,
                                          std::string_view prefix,
                                          float available_width,
                                          float relative_x) const;
  // Returns the active single-line input's hit-test rect (text rect) and surface tag, or
  // nullopt when no single-line input is currently editable. Used by mouse handlers to
  // decide whether a click landed inside an input.
 public:
  struct SingleLineInputHit {
    TextInputSurface surface = TextInputSurface::None;
    SDL_FRect text_rect{};
    float available_width = 0.0f;
    std::string prefix;
    const editor::SingleLineEditor* state = nullptr;
    editor::SingleLineEditor* mutable_state = nullptr;
  };

 private:
  std::optional<SingleLineInputHit> FindSingleLineInputHit(const WorkspaceLayout& layout,
                                                            float x,
                                                            float y);
  // Handles a single-line input mouse press by placing the caret, extending the selection
  // when shift is held, selecting the word at the caret for double-click, and selecting
  // all for triple-click. Begins drag-select tracking when the press was a plain click.
  bool HandleSingleLineInputMouseDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleSingleLineInputDrag(const SDL_Event& event, const WorkspaceLayout& layout);
  float MeasureSingleLineTextTail(std::string_view text, float available_width) const;
  void DrawSingleLineTextTail(SDL_Renderer* renderer,
                              float x,
                              float y,
                              float available_width,
                              SDL_Color foreground,
                              SDL_Color background,
                              std::string_view text) const;
  void UpdateTextInputArea(SDL_Renderer* renderer,
                           SDL_Window* render_window,
                           const std::optional<TextInputVisual>& visual) const;
  void RenderEditorHoverPopup(SDL_Renderer* renderer) const;
  void RenderTextComposition(SDL_Renderer* renderer,
                             const std::optional<TextInputVisual>& visual) const;
  std::string HoveredProjectTabTooltipLabel(const SDL_FRect& project_tab_strip) const;
  std::optional<SDL_FRect> HoveredProjectTabTooltipRect(const WorkspaceLayout& layout) const;
  std::string HoveredTabTooltipLabel(const SDL_FRect& tab_strip) const;
  std::optional<SDL_FRect> HoveredTabTooltipRect(const WorkspaceLayout& layout) const;
  std::string HoveredGitSidebarTooltipLabel(const SDL_FRect& sidebar_rect) const;
  std::optional<SDL_FRect> HoveredGitSidebarTooltipRect(const WorkspaceLayout& layout) const;
  // Tooltip text for the cryptic project-search option toggles in the sidebar
  // (pattern mode / case mode / hidden-file toggle). Returns "" when none is hovered.
  std::string HoveredSidebarSearchTooltipLabel(const SDL_FRect& sidebar_rect) const;
  std::optional<SDL_FRect> HoveredSidebarSearchTooltipRect(const WorkspaceLayout& layout) const;
  std::optional<SDL_FRect> HoveredStatusTooltipRect(const WorkspaceLayout& layout) const;
  // Hovered interactive-element rects used both by the cursor (pointer feedback) and by
  // the motion handler (hover-state redraw invalidation). Centralizing the geometry here
  // keeps render, cursor, and invalidation from each maintaining their own hit-test copy.
  std::optional<SDL_FRect> HoveredGitSidebarActionButtonRect(const WorkspaceLayout& layout,
                                                             float x, float y) const;
  std::optional<SDL_FRect> HoveredStatusBarSegmentRect(const WorkspaceLayout& layout, float x,
                                                       float y) const;
  // Screen region for the surface that currently owns keyboard focus (nullopt for the
  // modal Overlay focus, or when the surface is hidden). Used to draw the focus ring and
  // to invalidate the old+new surface when focus moves (see HandleEvent).
  std::optional<SDL_FRect> FocusSurfaceRect(const WorkspaceLayout& layout,
                                            FocusTarget focus) const;
  void DrawSurfaceFocusRing(SDL_Renderer* renderer, const SDL_FRect& region) const;
  void EnsureOutputChannelTabOpen(std::string_view channel_id);
  void CloseOutputChannelTab(std::string_view channel_id);
  SDL_FRect ComputeOverlayRect(const SDL_FRect& editor_area) const;
  SDL_FRect FindWidgetAnchorRect(const SDL_FRect& fallback) const;  // editor_surface anchor
  SDL_FRect ComputeCaretAnchoredOverlayRect(const SDL_FRect& editor_area,
                                            const SDL_FRect& caret_anchor) const;
  std::string BreadcrumbLabel() const;
  std::string ProjectLabel() const;
  std::string ProjectLabelForRoot(const std::filesystem::path& root) const;
  std::string ProjectTabDisplayTitle(std::size_t index) const;
  std::string ProjectTabTooltipLabel(std::size_t index) const;
  std::string TruncateLabel(std::string_view text, float max_width) const;
  std::optional<SDL_FRect> CurrentWindowRect() const;
  std::optional<WorkspaceLayout> CurrentWorkspaceLayout() const;
  const WindowChromeState& CurrentWindowChromeState() const;
  void MarkLayoutDirty();
  void RequestFullRedraw();
  // Post a transient notification toast and schedule a full redraw so it paints.
  // The single host-owned entry point for toasts (plugins + built-in callers).
  void Notify(NotificationService::Tone tone, std::string message);
  void RequestRedrawRect(const SDL_FRect& rect);
  void RequestWindowRedraw();
  void RequestChromeRedraw();
  void RequestSidebarRedraw();
  void RequestSidebarLayoutChangeRedraw(const WorkspaceLayout& previous_layout);
  void RequestBreadcrumbRedraw();
  void RequestTabStripRedraw();
  void RequestActiveEditableLastChangeRedraw();
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
  void RequestDebugPaneRedraw();
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
  // True when ShouldBlinkCaret() AND the caret hasn't been idle past
  // kCaretBlinkIdleStopMs since the last ResetCaretBlink(). After that
  // window the caret freezes visible and no further blink wakes fire.
  bool CaretBlinkAnimating() const;
  bool CaretVisibleNow() const;
  std::optional<SDL_FRect> ActiveEditorCaretRect(const WorkspaceLayout& layout) const;
  std::optional<SDL_FRect> ActiveTerminalCaretRect(const WorkspaceLayout& layout) const;
  SDL_FRect CompareDividerHitRect(const SDL_FRect& editor_surface,
                                  const CompareSurfaceLayout& surface) const;
  std::optional<TextInputVisual> BuildCompareTextInputVisual(const SDL_FRect& editor_surface) const;
  std::optional<TextInputVisual> BuildMergeTextInputVisual(const SDL_FRect& editor_surface) const;
  void RenderCompareScrollbars(SDL_Renderer* renderer,
                               const SDL_FRect& editor_surface,
                               CompareTabState& compare_tab);
  void RenderMergeScrollbars(SDL_Renderer* renderer, const SDL_FRect& editor_surface);
  CursorKind CursorKindForPosition(float x, float y) const;
  SDL_Cursor* CursorHandle(CursorKind kind);
  void ClearMouseHoverState();
  void InvalidateCursorKindFingerprint();
  void UpdateMouseCursor(float x, float y, bool update_editor_hover = true,
                         bool workspace_layout_recomputed = false);

  render::Theme theme_ = render::MakeDefaultTheme();
  render::TextRenderer text_renderer_;
  editor::EditorViewRenderer editor_view_renderer_;
  WorkspaceContext context_;
  // Single owner of settings resolution + mutation. Binds non-owning pointers to
  // the user / active-project layers in context_ and keeps an O(1) resolved
  // index. Bound in the constructor and re-bound on every project switch
  // (RebindProjectState / ResetCurrentProjectStateStorage).
  SettingsStore settings_store_;
  WindowPresentationState window_presentation_;
  struct SidebarModeMenuEntry {
    std::string label;
    std::string id;
  };
  mutable std::vector<SidebarModeMenuEntry> sidebar_mode_menu_entries_;
  mutable std::vector<MenuItemSpec> sidebar_mode_menu_items_;
  struct DynamicMenuEntryStorage {
    std::string label;
    std::string accelerator;
    std::string command_name;
  };
  static constexpr std::size_t kMenuSlotCount =
      static_cast<std::size_t>(MenuId::ProjectTabContext) + 1;
  mutable std::array<std::vector<DynamicMenuEntryStorage>, kMenuSlotCount> dynamic_menu_entries_;
  mutable std::array<std::vector<MenuItemSpec>, kMenuSlotCount> dynamic_menu_items_;
  mutable std::vector<ResolvedKeybinding> resolved_keybindings_cache_;
  mutable std::vector<std::string> resolved_keybindings_disabled_ids_snapshot_;
  mutable std::size_t resolved_keybindings_reload_generation_ = std::numeric_limits<std::size_t>::max();
  WorkspaceProjectSearchRuntime project_search_runtime_;
  WorkspaceProjectFileMonitor project_file_monitor_;
  project::ProjectChangeCoalescer project_change_coalescer_;
  project::GitRepositoryMetadataTracker git_metadata_tracker_;
  std::uint64_t last_applied_project_change_generation_ = 0;
  project::ProjectBackgroundExecutor project_background_executor_;
  GitRepositoryService git_repository_service_{project_background_executor_};
  PatchApplyService patch_apply_service_{project_background_executor_, git_repository_service_};
  CommitWorkflowService commit_workflow_service_{project_background_executor_, git_repository_service_};
  std::mutex save_tab_mutex_;
  std::unique_ptr<platform::FileIndexWatcher> file_index_watcher_;
  std::atomic<std::uint64_t> file_index_watcher_generation_{0};
  std::atomic<bool> file_index_has_pending_changes_{false};
  std::atomic<bool> file_index_initial_build_in_flight_{false};
  std::atomic<bool> project_file_event_pending_{false};
  WorkspacePluginRuntime plugin_runtime_;
  std::size_t reload_plugins_invocation_count_ = 0;
  std::size_t reload_clean_open_buffers_from_disk_invocation_count_ = 0;
  std::size_t render_compare_surface_invocation_count_ = 0;
  std::optional<WorkspaceLayout> prepared_frame_layout_;
  mutable TabStripService tab_strip_service_;
  bool layout_dirty_ = true;
  std::size_t prepare_frame_layout_compute_count_ = 0;
  bool prepared_frame_draw_editor_caret_ = false;
  std::uint64_t prepared_frame_id_ = 0;
  // `BuildFrameSurface` / `BuildOverlaySurface` are shared across `RenderClip` iterations (partial
  // redraw) and between `RenderFrameBase` + `RenderActiveWorkspaceSurface` within one clip.
  mutable bool clip_frame_overlay_view_models_valid_ = false;
  mutable std::uint64_t clip_frame_overlay_view_models_frame_id_ = 0;
  mutable WorkspaceLayout clip_frame_overlay_view_models_layout_{};
  mutable std::optional<FrameSurfaceViewModel> clip_cached_frame_vm_;
  mutable std::optional<OverlaySurfaceViewModel> clip_cached_overlay_vm_;
  mutable std::optional<SidebarSurfaceViewModel> prepare_cached_sidebar_vm_;
  mutable std::optional<BottomPanelSurfaceViewModel> prepare_cached_bottom_panel_vm_;
  mutable std::optional<DebugPaneSurfaceViewModel> prepare_cached_debug_pane_vm_;
  mutable std::optional<TextInputSurfaceViewModel> prepare_cached_text_input_vm_;
  // Last bottom-panel rect for which ResizeTerminalToPanel was invoked. When
  // the panel rect is unchanged between frames there's nothing to recompute,
  // so we skip the call entirely. Reset when no terminal is active.
  std::optional<SDL_FRect> last_terminal_panel_rect_;
  WorkspaceOutputChannels output_channels_;
  FormatterRegistry formatter_registry_;
  SaveParticipantRegistry save_participant_registry_;
  CompletionRegistry completion_registry_;
  CodeActionRegistry code_action_registry_;
  ToolRegistry tool_registry_;
  ToolDownloader tool_downloader_;
  TestController test_controller_;
  ScmRegistry scm_registry_;
  AnnotationRegistry annotation_registry_;
  VirtualDocumentRegistry virtual_document_registry_;
  ReviewCommentsRegistry review_comments_registry_;
  PersistenceService persistence_service_;
  RecentsService recents_service_;
  LayoutModeService layout_mode_service_;
  WorkspaceTabStripChrome tab_strip_chrome_;
  StatusBarService status_bar_service_;
  StatusBarModelService status_bar_model_service_;
  NotificationService notification_service_;
  SettingsOverlayService settings_overlay_service_;
  // Settings/Help overlay max scroll row, resolved by the const render pass (it
  // owns the list geometry) and consumed by the wheel handler to clamp scrolling.
  mutable int settings_overlay_max_scroll_row_ = 0;
  AssistService assist_service_;
  LspService lsp_service_;
  DebugService debug_service_;
  ControlChannelService control_channel_service_;
  WorkspaceLanguageContract language_contract_;
  struct LiveSettingsEditorSnapshot {
    std::filesystem::path project_root;
    std::size_t tab_size = 0;
    std::size_t indent_width = 0;
    bool soft_tabs = false;
    bool soft_wrap = false;
    std::optional<std::string> auto_close_enabled;
    std::optional<std::string> surround_enabled;
    std::optional<std::string> smart_indent_enabled;
    std::optional<std::string> save_trim_trailing_whitespace;
    std::optional<std::string> save_ensure_final_newline;
  };
  std::optional<LiveSettingsEditorSnapshot> last_live_settings_editor_snapshot_;
  Uint32 git_blame_event_type_ = 0;
  Uint32 git_sidebar_event_type_ = 0;
  Uint32 terminal_event_type_ = 0;
  Uint32 project_file_event_type_ = 0;
  Uint32 project_open_dialog_event_type_ = 0;
  Uint32 lsp_event_type_ = 0;
  Uint32 dap_event_type_ = 0;
  Uint32 control_event_type_ = 0;
  Uint32 plugin_async_process_event_type_ = 0;
  Uint32 highlight_prefetch_event_type_ = 0;
  Uint64 next_automatic_git_sidebar_refresh_ms_ = 0;
  bool pending_tree_git_badge_refresh_after_paint_ = false;
  project::GitBlameService git_blame_service_;
  editor::HighlightPrefetchService highlight_prefetch_service_;
  EditorBlameOverlayService editor_blame_overlay_service_;
  std::optional<EditorHoverTarget> active_editor_hover_target_;
  bool editor_hover_refresh_pending_ = false;
  std::function<std::optional<std::string>()> clipboard_text_reader_;
  std::function<bool(std::string_view)> clipboard_text_writer_;
  std::function<std::optional<std::string>()> primary_selection_text_reader_;
  std::function<bool(std::string_view)> primary_selection_text_writer_;
  std::function<bool(std::string_view)> external_url_opener_;
  std::function<bool(const std::filesystem::path&)> file_manager_opener_;
  SDL_Window* dialog_window_ = nullptr;
  ProjectDialogState project_dialog_state_;
  bool quit_requested_ = false;
  WorkspaceStartupOptions startup_options_;
  std::vector<std::string> available_colorscheme_names_;
  float ui_scale_ = 1.0f;
  float presentation_scale_x_ = 1.0f;
  float presentation_scale_y_ = 1.0f;
  WindowAction pending_window_action_ = WindowAction::None;
  Uint64 caret_blink_epoch_ms_ = 0;
  RenderInvalidation pending_render_invalidation_;
  int post_render_full_redraws_remaining_ = 0;
  CursorKind cursor_kind_ = CursorKind::Default;
  // When set, the next UpdateMouseCursor bypasses both the fingerprint and the
  // unchanged-kind short-circuits and forces the platform cursor to be re-shown.
  // See ForceCursorReassert.
  bool force_cursor_reassert_ = false;
  // Cached fingerprint of state CursorKindForPosition depends on, so the per-frame
  // UpdateMouseCursor call can short-circuit when nothing relevant changed.
  struct CursorKindFingerprint {
    float x = 0.0f;
    float y = 0.0f;
    int drag_target = 0;
    bool valid = false;
    bool dirty_prompt = false;
    bool prompt_surface = false;
    bool menu_bar_open = false;
    bool overflow_popup_open = false;
    bool tree_context_menu_open = false;
    int active_menu_id = 0;
    int hovered_popup_row_index = -1;
    int hovered_submenu_row_index = -1;
    int active_submenu_id = 0;
    bool overlay_visible = false;
    bool settings_overlay_visible = false;
    bool bottom_panel_visible = false;
    bool debug_toolbar_visible = false;
    bool chrome_custom_enabled = false;
    bool chrome_maximized = false;
    bool chrome_fullscreen = false;
    int window_width = 0;
    int window_height = 0;
    std::uint32_t active_tab_index = 0;
    std::uint32_t cursor_hit_generation = 0;
    std::uint32_t editor_hover_target_generation = 0;
  };
  CursorKindFingerprint cursor_kind_fingerprint_{};
  std::uint32_t cursor_hit_generation_ = 0;
  std::uint32_t editor_hover_target_generation_ = 0;
  SDL_Cursor* text_cursor_ = nullptr;
  SDL_Cursor* pointer_cursor_ = nullptr;
  SDL_Cursor* ew_resize_cursor_ = nullptr;
  SDL_Cursor* ns_resize_cursor_ = nullptr;
  SDL_Cursor* n_resize_cursor_ = nullptr;
  SDL_Cursor* e_resize_cursor_ = nullptr;
  SDL_Cursor* s_resize_cursor_ = nullptr;
  SDL_Cursor* w_resize_cursor_ = nullptr;
  SDL_Cursor* ne_resize_cursor_ = nullptr;
  SDL_Cursor* se_resize_cursor_ = nullptr;
  SDL_Cursor* sw_resize_cursor_ = nullptr;
  SDL_Cursor* nw_resize_cursor_ = nullptr;
  float last_mouse_x_ = 0.0f;
  float last_mouse_y_ = 0.0f;
  bool last_mouse_position_valid_ = false;
  struct TerminalForegroundRunScratch {
    std::size_t start_column = 0;
    std::size_t end_column_exclusive = 0;
    SDL_Color foreground{};
    std::size_t blob_offset = 0;
    std::size_t blob_length = 0;
    bool has_visible_text = false;
  };
  mutable std::vector<TerminalForegroundRunScratch> terminal_foreground_runs_scratch_;
  mutable std::string terminal_foreground_runs_blob_;
