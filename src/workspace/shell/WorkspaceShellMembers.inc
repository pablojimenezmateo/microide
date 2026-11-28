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
    SDL_FPoint window_per_unit{1.0f, 1.0f};
    SDL_FPoint requested_window_per_unit{1.0f, 1.0f};
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
  void ApplyControlSpec(const ControlSpec& spec);
  void ForceStartControlChannel();
  void ApplyStartupSettingOverrides();
  void Shutdown();
  void SetRenderBackendInfo(std::string driver_name, bool is_gpu) {
    context_.render_driver_name = std::move(driver_name);
    context_.render_is_gpu = is_gpu;
  }
  void SetSandboxSupport(const platform::SandboxSupport& support) {
    context_.sandbox_support = support;
  }
  EventResult HandleEvent(const SDL_Event& event);
  EventResult HandleScheduledWake();
  EventResult HandleScheduledWakeSources();
  FrameToken PrepareFrameOnce(SDL_Renderer* renderer, int width, int height);
  void RenderClip(const FrameToken& frame_token, SDL_Renderer* renderer, int width, int height,
                  std::optional<SDL_FRect> dirty_rect_hint = std::nullopt);
  void EnsureClipFrameAndOverlayViewModels(const WorkspaceLayout& layout) const;
  void PrepareRenderFrame(SDL_Renderer* renderer, int width, int height);
  void Render(SDL_Renderer* renderer, int width, int height);
  void RenderPrepared(SDL_Renderer* renderer, int width, int height);
  IdleWaitState CurrentIdleWaitState();
  std::optional<Uint32> NextAnimationDelayMs() const;
  std::optional<RenderInvalidation> ConsumePostRenderRedrawRequest();
  void RequestActiveHighlightPrefetch();
  void OnFramePresented();
  std::optional<SDL_FRect> CurrentCaretDirtyRect() const;
  RenderInvalidation ConsumePendingRenderInvalidation();
  void RequestQuit(bool forced = false);
  bool ConsumeQuitRequested();
  float UiScale() const { return ui_scale_; }
  render::TextClipPadding PartialRedrawClipPadding() const;
  void SetWindowPresentationState(WindowPresentationState state);
  std::optional<WindowPresentationState> CurrentWindowPresentationState() const;
  void SetDialogWindow(SDL_Window* window) { dialog_window_ = window; }
  SDL_HitTestResult WindowHitTest(float x, float y) const;
  bool WindowDragRegionContains(float x, float y) const;
  bool MenuSurfaceCapturingMouse() const;
  WindowAction ConsumeWindowAction();
  void ForceCursorReassert();

  void SeedPointerPosition(float render_x, float render_y);

  void InvalidateCursorKindFingerprint();

  struct VisiblePopupMenuItem {
    std::size_t index = 0;
    SDL_FRect rect{};
    bool enabled = false;
    bool checked = false;
    bool hovered = false;
    bool separator = false;
  };

  WorkspaceActionContext& MakeActionContext();
  bool ActionContextRunCompareCommand(ActionId id, std::string* error);
  SaveAllOutcome ActionContextSaveAllDirtyTabs();

  struct TestAccess;
  friend struct TestAccess;

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
  using TerminalSelectionPoint = workspace::TerminalSelectionPoint;
  using TerminalPaneState = workspace::TerminalPaneState;
  using EditorPreferences = workspace::EditorPreferences;
  using ProjectSurfaceState = workspace::ProjectSurfaceState;
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

  using CursorKind = workspace::CursorKind;

 public:
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

  using SingleLineViewMetrics = workspace::SingleLineViewMetrics;

 public:
  struct LogSurfaceLayout {
    SDL_FRect content_rect{};
    float text_x = 0.0f;
    float text_y = 0.0f;
    float text_width = 0.0f;
    float line_height = 0.0f;
    ScrollSurfaceLayout scroll;
  };

  std::optional<SDL_FRect> TerminalCaretRectIn(
      const LogSurfaceLayout& panel_layout,
      const terminal::TerminalCursorSnapshot& cursor) const;

  using VisibleStripTab = workspace::VisibleStripTab;
  using TabStripOverflowControls = workspace::TabStripOverflowControls;

  using VisibleMenuBarItem = workspace::VisibleMenuBarItem;

  struct VisibleWindowControlButton {
    WindowControlButtonId id = WindowControlButtonId::Minimize;
    SDL_FRect rect{};
    bool hovered = false;
  };

  using VisibleMenuBarItems = workspace::VisibleMenuBarItems;
  using MenuBarOverflowIds = workspace::MenuBarOverflowIds;
  using VisibleWindowControlButtons =
      util::InlineVector<VisibleWindowControlButton, kWindowControlButtonCount>;

  struct VisibleStatusItem {
    StatusItemView item;
    SDL_FRect rect{};
    bool hovered = false;
  };

  struct EditorPaneLayout {
    std::size_t group_index = 0;
    SDL_FRect rect{};
    bool active = false;
  };

  using EditorPaneLayouts = util::InlineVector<EditorPaneLayout, kMaxEditorGroups>;

  using BottomPanelTabKind = workspace::BottomPanelTabKind;
  using BottomPanelTabModel = workspace::BottomPanelTabModel;

 private:

  struct EditorBlamePopupLayout {
    std::size_t line_index = 0;
    SDL_FRect rect{};
    SDL_FRect copy_sha_rect{};
  };

  struct SignatureHelpPopup {
    std::string signature;
    std::string documentation;
    std::filesystem::path path;
    std::size_t cursor_line = 0;
    std::size_t cursor_column = 0;
  };
  void LayOutSignatureHelpCard(const SignatureHelpPopup& popup, const SDL_FRect& rect, CardPaint& card) const;

  struct PendingHoverEval {
    int frame_id = 0;
    std::string expression;
    bool valid = false;
  };
  mutable PendingHoverEval pending_hover_eval_;

  struct PluginHoverCache {
    enum class State { Empty, Kickoff, Pending, Resolved, Failed };
    std::filesystem::path path;
    std::size_t line = 0;
    std::size_t column = 0;
    std::uint64_t generation = 0;
    State state = State::Empty;
    Uint64 failed_at_ms = 0;
    plugin::PluginHost::HoverResult result;
    bool Matches(const std::filesystem::path& other_path, std::size_t other_line,
                 std::size_t other_column) const {
      return state != State::Empty && line == other_line && column == other_column &&
             path == other_path;
    }
  };
  mutable PluginHoverCache plugin_hover_cache_;
  void KickOffPluginHover(const std::filesystem::path& path, std::size_t line, std::size_t column);
  void KickOffLspHover(const std::filesystem::path& path, std::size_t line, std::size_t column);

  static const ActionSpec* FindActionByCommand(std::string_view command_name);
  static std::span<const MenuSpec> MenuSpecs();
  static const MenuSpec* FindMenuSpec(MenuId id);
  std::span<const MenuItemSpec> MenuItems(MenuId id) const;
  VisibleMenuBarItems ComputeVisibleMenuBarItems(const SDL_FRect& menu_bar) const;
  MenuBarOverflowIds ComputeOverflowMenuBarItems(const SDL_FRect& menu_bar) const;
  std::optional<SDL_FRect> MenuOverflowChevronRect(const SDL_FRect& menu_bar) const;
  VisibleWindowControlButtons ComputeVisibleWindowControlButtons(const SDL_FRect& menu_bar) const;
  std::optional<SDL_FRect> ComputePopupMenuRect(const SDL_FRect& anchor_rect,
                                                std::span<const MenuItemSpec> items,
                                                const SDL_FRect& bounds) const;
  std::optional<SDL_FRect> ComputePopupMenuRect(const SDL_FRect& menu_bar, MenuId id) const;
  std::vector<VisiblePopupMenuItem> ComputeVisiblePopupMenuItems(
      std::span<const MenuItemSpec> items,
      int active_item_index,
      const SDL_FRect& popup_rect) const;
  void ComputeVisiblePopupMenuItemsInto(std::span<const MenuItemSpec> items,
                                        int active_item_index,
                                        const SDL_FRect& popup_rect,
                                        std::vector<VisiblePopupMenuItem>& out) const;
  std::vector<VisiblePopupMenuItem> ComputeVisiblePopupMenuItems(MenuId id,
                                                                 const SDL_FRect& popup_rect) const;
  std::string_view MenuItemLabel(const MenuItemSpec& item, bool with_reason = true) const;
  std::string_view MenuItemAccelerator(const MenuItemSpec& item) const;
  bool IsMenuItemEnabled(const MenuItemSpec& item) const;
  bool IsMenuItemEnabled(const MenuItemSpec& item, const ActionAvailability& availability) const;
  bool IsMenuItemChecked(const MenuItemSpec& item) const;
  bool ExecuteCustomMenuItem(MenuId id, std::size_t item_index);
  bool ExecuteCommandName(std::string_view command_name,
                          const std::vector<std::string>& args,
                          ActionSource source,
                          std::string* error_message = nullptr);
  void NotifyPluginCommandOutcome(const std::string& command,
                                  bool ran,
                                  const std::string& error,
                                  const std::string& feedback);
  std::optional<std::string> GetSettingValue(std::string_view id) const;
  const std::vector<ResolvedKeybinding>& ResolvedKeybindings() const;
  std::vector<std::string> OrderedSidebarViewIds() const;
  void NormalizeSidebarViewSelection();
  void RebuildPhase3Registries(bool reconcile_language_servers = true);
  void RebuildPhase4Registries();
  void RebuildPresentationRegistries();
  std::vector<VisibleStatusItem> ComputeVisibleStatusItems(const SDL_FRect& breadcrumb) const;
  std::optional<SDL_FRect> ActiveSubmenuRect(const SDL_FRect& menu_bar) const;
  const project::TreeEntry* SelectedTreeEntry() const;
  std::filesystem::path SelectedTreePath() const;
  TreeContextTargetKind SelectedTreeTargetKind() const;
  std::filesystem::path ResolveTreeActionPath(ActionSource source) const;
  std::optional<SDL_FRect> ComputeTreeContextMenuRect() const;
  std::filesystem::path ResolveProjectRootInput(const std::filesystem::path& project_root) const;
  ProjectOpenPickerResult OpenNativeProjectPicker(std::string* error_message = nullptr);
  static void SDLCALL OnProjectOpenDialogComplete(void* userdata,
                                                  const char* const* filelist,
                                                  int filter);
  void ConsumePendingProjectOpenDialogResult();
  ProjectOpenPickerResult OpenNativeFilePicker(std::string* error_message = nullptr);
  static void SDLCALL OnOpenFileDialogComplete(void* userdata,
                                               const char* const* filelist,
                                               int filter);
  void ConsumePendingOpenFileDialogResult();
  void OpenNativeFontFilePicker(std::string setting_id);
  static void SDLCALL OnFontFileDialogComplete(void* userdata, const char* const* filelist,
                                               int filter);
  void ConsumePendingFontFileDialogResult();
  bool SetProjectRoot(const std::filesystem::path& project_root);
  ProjectCatalogService& MakeProjectCatalogService();
  PersistenceCoordinator& MakePersistenceCoordinator();
  MenuCoordinator& MakeMenuCoordinator();
  CommandLineCoordinator& MakeCommandLineCoordinator();
  PromptSurfaceService& MakePromptSurfaceService();
  DirtyPromptCoordinator MakeDirtyPromptCoordinator(EditorTabService& editor_tabs,
                                                    PromptSurfaceService& prompt_surfaces);
  CompareInteractionCoordinator MakeCompareInteractionCoordinator();
  CompareMergeService& MakeCompareMergeService();
  CompareReviewService& MakeCompareReviewService();
  PathMutationCoordinator MakePathMutationCoordinator(EditorTabService& editor_tabs,
                                                      PromptSurfaceService& prompt_surfaces);
  DiffTabCoordinator MakeDiffTabCoordinator();
  LifecycleCoordinator& MakeLifecycleCoordinator();
  SidebarCoordinator MakeSidebarCoordinator();
  SidebarService& MakeSidebarService();
  DebugPaneService& MakeDebugPaneService();
  KeyInputCoordinator& MakeKeyInputCoordinator();
  TextInputCoordinator& MakeTextInputCoordinator();
  EditorTabService& MakeEditorTabService();
  TabCoordinator MakeTabCoordinator();
  ChromeMouseCoordinator& MakeChromeMouseCoordinator();
  EditorMouseCoordinator& MakeEditorMouseCoordinator();
  CompareMouseCoordinator& MakeCompareMouseCoordinator();
  MergeMouseCoordinator& MakeMergeMouseCoordinator();
  TabMouseCoordinator& MakeTabMouseCoordinator();
  bool HandleTabMouseMotion(const SDL_Event& event, const WorkspaceLayout& layout);
  SidebarMouseCoordinator& MakeSidebarMouseCoordinator();
  PanelMouseCoordinator& MakePanelMouseCoordinator();
  DebugPaneMouseCoordinator& MakeDebugPaneMouseCoordinator();
  TerminalPanelService& MakeTerminalPanelService();
  RemoteHostService& MakeRemoteHostService();
  void ResetLifecycleStartupState();
  void RegisterLifecycleWakeEvents();
  void ConsumeHighlightPrefetchResults();
  void RebindProjectState(ProjectWorkspaceState& state);
  void ResetCurrentProjectStateStorage();
  void ResetTransientInteractionState();
  void ClearDragState();
  bool HasActiveProjectCatalogEntry() const;
  ProjectWorkspaceState* ProjectCatalogEntry(std::size_t index);
  const std::filesystem::path& ProjectCatalogRoot(std::size_t index) const;
  void ResetProjectCatalogToWelcomeState();
  bool ReloadPluginsForCurrentProject(PluginReloadRequest request = {});
  void ConsumeReloadResult(PluginReloadRequest request, bool clean_reload);
  void RefreshPluginSurfacesForReactivation();
  bool ReloadPluginsIfPluginAssetsChanged(bool force_check);
  bool ReloadProjectIfFilesChanged(bool force_check);
  void ApplyProjectChangeBatch(const project::ProjectChangeBatch& batch);
  void RefreshProjectFileViews();
  void ApplyExternalFileChange(const std::filesystem::path& normalized_path,
                               std::set<std::filesystem::path>& dirty_external_paths,
                               std::set<std::filesystem::path>& refresh_compare_paths);
  void BeginExternalChangeConfirm(const std::filesystem::path& normalized_path);
  void ApplyExternalChangeConfirm(const std::filesystem::path& normalized_path, bool read_ok,
                                  const std::optional<std::uint64_t>& digest,
                                  const util::FileSignature& read_signature);
  std::set<std::string> pending_external_change_confirms_;
  void MarkCompareTabsStaleForPath(const std::filesystem::path& path);
  void InvalidateMergeTabsForPath(const std::filesystem::path& path);
  void ActivateEditorBannerAction(EditorBannerAction action, const std::filesystem::path& path);
  void ClearDiagnosticsForPath(const std::filesystem::path& path);
  void RequestGitSidebarRefresh(GitSidebarRefreshScope scope = GitSidebarRefreshScope::Full);
  void MaybeRequestTreeGitBadgesAfterFirstPaint();
  void RequestAutomaticGitSidebarRefresh();
  void InvalidateRuntimeSyntaxStateCaches(std::span<const std::string_view> changed_languages);
  std::string PluginRuntimeReloadSummary() const;
  void NotifyPluginsAboutOpenBuffers(bool replay_plugin_buffer_opens = true,
                                     bool open_lsp_documents = true);
  SavePreparation PrepareEditorViewportForSave(const std::filesystem::path& path,
                                               editor::TextViewport& viewport,
                                               std::string* error_message,
                                               SaveMode mode);
  void ApplyDeferredSaveFormat(const SaveFormatterService::Completion& completion);
  void ApplyAsyncFileRead(project::FileReadService::Completion completion,
                          editor::TextViewport loaded);
  void ShareLoadedBufferWithWaitingTabs(const std::filesystem::path& path,
                                        const editor::TextViewport& loaded);
  void ReportSaveFormatterFailure(const SaveFormatterService::Completion& completion,
                                  std::string* error_message);
  bool OpenVirtualDocumentInNewTab(std::string_view uri);
  void ReloadVirtualDocumentTabs(std::string_view uri);
  bool IsReadOnlyVirtualDocument(const std::filesystem::path& path) const;
  void NotifyPluginBufferOpen(const std::filesystem::path& path);
  void NotifyLspBufferOpen(const std::filesystem::path& path);
  void NotifyPluginBufferSave(const std::filesystem::path& path);
  void NotifyLspBufferClose(const std::filesystem::path& path);
  void ResetProjectScopedState(bool show_welcome);
  bool StartFileIndexWatcherForCurrentProject();
  void StopFileIndexWatcher();
  void AppendForcedProjectRescanChanges(project::ProjectChangeBatch& batch);
  void RequestFileIndexRefresh();
  void ApplyForcedFileIndexRefresh(const std::filesystem::path& root,
                                   std::vector<project::ProjectFile> files,
                                   project::ProjectFileScanStatus status,
                                   std::uint64_t index_version_at_dispatch);
  void SetWelcomePlaceholder();
  bool InitializeCurrentProject(const project::ProjectId& id, const std::filesystem::path& project_root,
                                bool restore_persistence,
                                bool log_feedback,
                                bool activate_restored_tab, bool root_missing, project::ProjectLocality locality);
  bool OpenProjectTab(const std::filesystem::path& project_root,
                      bool restore_persistence,
                      bool log_feedback, const project::ProjectLocality& locality = {});
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
  bool SaveTab(std::size_t index, SaveMode mode);
  bool SaveGroupTab(std::size_t group_index, std::size_t index,
                    SaveMode mode);
  void MaybeAutosaveDirtyTabs(bool on_focus_change);
  bool TabIsDirty(std::size_t index) const;
  const TabEntry* TabForLabels(std::size_t group_index, std::size_t index) const;
  std::string TabDisplayTitle(std::size_t index) const;
  std::string TabDisplayTitle(std::size_t group_index, std::size_t index) const;
  std::string TabTooltipLabel(std::size_t index) const;
  std::string TabTooltipLabel(std::size_t group_index, std::size_t index) const;
  std::vector<std::size_t> DirtyEditorTabIndices() const;
  std::vector<std::size_t> DirtyEditorTabIndicesForProject(std::size_t project_index) const;
  bool HasDirtyEditorTabForProject(std::size_t project_index) const;
  std::vector<GroupTabRef> DirtyEditorGroupTabs() const;
  std::vector<GroupTabRef> DirtyEditorGroupTabsForProject(std::size_t project_index) const;
  void ShowDirtyPromptForQuit();
  void DismissDirtyPrompt(bool restore_focus);
  void ConfirmDirtyPrompt();
  std::array<std::string_view, 3> DirtyPromptActionLabels() const;
  std::string_view DirtyPromptTitle() const;
  std::string DirtyPromptMessage() const;
  void OpenPromptSurface(PromptSurfaceState::Action action,
                         PromptSurfaceState::Kind kind,
                         const std::filesystem::path& path,
                         std::string input = {});
  void DismissPromptSurface(bool restore_focus);
  void ConfirmPromptSurface(DirtyPathResolution resolution = DirtyPathResolution::RequirePrompt);
  std::string_view PromptSurfaceTitle() const;
  std::string PromptSurfaceMessage() const;
  std::string_view PromptSurfaceDetail() const;
  std::array<std::string_view, 2> PromptSurfaceActionLabels() const;
  std::filesystem::path TreeMutationBasePath(ActionSource source) const;
  bool HasDirtyEditorTabsForPath(const std::filesystem::path& path,
                                 std::string* blocking_label = nullptr) const;
  void CloseOpenTabsForPath(const std::filesystem::path& path);
  bool ActivateCurrentTabAfterStateLoad();
  bool ActiveTabIsEditor() const;
  TabEntry::EditorTabState* ActiveEditorTab();
  const TabEntry::EditorTabState* ActiveEditorTab() const;
  editor::TextViewport* ViewportForPane(const EditorPaneLayout& pane);
  const editor::TextViewport* ViewportForPane(const EditorPaneLayout& pane) const;
  void SyncActiveEditorTabMetadata();
  bool ReplaceActiveEditorView(const editor::TextViewport& viewport);
  editor::TextViewport* ActiveEditorViewport();
  const editor::TextViewport* ActiveEditorViewport() const;
  editor::TextViewport* ActiveNavigableViewport();
  const editor::TextViewport* ActiveNavigableViewport() const;
  std::filesystem::path ActiveTabPath() const;
  EditorPaneLayouts ComputeEditorPaneLayouts(const WorkspaceLayout& layout) const;
  EditorPaneLayouts EditorPaneLayoutsFromGroupRects(
      const EditorGroupRectsLayout& group_rects) const;
  EditorGroupRectsLayout ComputeEditorGroupRectsForState(const WorkspaceLayout& layout) const;
  SDL_FRect ActiveTabSurface(const WorkspaceLayout& layout, const void* shown = nullptr) const;
  float EditorGroupTabStripWidth(std::size_t group_index) const;
  std::size_t FocusedEditorGroupIndex() const;
  ScrollSurfaceLayout ComputeEditorScrollLayout(const SDL_FRect& rect,
                                                const editor::TextViewport& viewport,
                                                const editor::EditorViewMetrics& metrics) const;
  bool ActiveTabIsCompare() const;
  CompareTabState* ActiveCompareTab();
  const CompareTabState* ActiveCompareTab() const;
  CompareSurfaceLayout ComputeCompareSurfaceLayout(const SDL_FRect& rect, const CompareTabState& compare_tab) const;
  TextGridInteractionLayout BuildCompareRightInteractionLayout(const CompareSurfaceLayout& surface,
                                                               CompareTabState& compare_tab) const;
  void RevealActiveCompareSelection();
  bool ActiveTabIsMerge() const;
  MergeTabState* ActiveMergeTab();
  const MergeTabState* ActiveMergeTab() const;
  MergeSurfaceLayout ComputeMergeSurfaceLayout(const SDL_FRect& rect, const MergeTabState& merge_tab) const;
  TextGridInteractionLayout BuildMergeSourceInteractionLayout(const MergeSurfaceLayout& surface,
                                                              const MergeTabState& merge_tab,
                                                              bool incoming) const;
  MergeResultInteractionLayout BuildMergeResultInteractionLayout(
      const SDL_FRect& rect,
      const MergeSurfaceLayout& surface,
      const MergeTabState& merge_tab) const;
  MergeInteractionLayout BuildMergeInteractionLayout(const SDL_FRect& rect,
                                                     const MergeSurfaceLayout& surface,
                                                     MergeTabState& merge_tab) const;
  SDL_FRect BuildMergeSourceActionButtonRect(const MergeSurfaceLayout& surface,
                                             const MergeInteractionLayout& interaction,
                                             const MergeTrackedConflict& conflict, bool incoming) const;
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
  void RevealActiveMergeSelection();
  bool OpenComparePickerForPath(const std::filesystem::path& path,
                                std::string_view commit_spec = {});
  void RequestComparePickerFileHistory(const std::filesystem::path& path);
  void RequestComparePickerRefs(bool include_commits);
  void ApplyComparePickerFileHistory(std::uint64_t generation,
                                     const project::GitFileHistoryResult& history);
  void ApplyComparePickerRefs(std::uint64_t generation,
                              const std::vector<project::GitBranchReference>& branches,
                              const std::vector<project::GitCommitEntry>& commits);
  bool ComparePickerRequestCurrent(std::uint64_t generation) const;
  SidebarMode ActiveSidebarMode() const;
  std::optional<TabEntry> BuildCompareTabEntry(const std::filesystem::path& path, const CompareTabState& compare_tab,
                                               const CompareRevisionSides* sides = nullptr) const;
  void RefreshCompareTabDerivedState(CompareTabState& compare_tab) const;
  void SyncCompareViewportScroll(CompareTabState& compare_tab) const;
  void SyncCompareSelectionFromViewport(CompareTabState& compare_tab,
                                        bool reveal_selection) const;
  std::optional<TabEntry> BuildMergeTabEntry(const std::filesystem::path& base_path,
                                             const std::filesystem::path& incoming_path,
                                             const std::filesystem::path& current_path,
                                             const std::filesystem::path& output_path) const;
  void OpenCommandPalette(std::string seed = {});
  void RefreshCommandPalette();
  void ConfirmCommandPaletteSelection();
  bool CommandPaletteQueryIsCommandLine() const;
  void CompleteCommandPaletteQuery();
  bool ProbeWelcomeSurface(const editor::WelcomeViewModel** model,
                           const editor::WelcomeLayout** layout) const;
  void JumpCompareHunk(int delta);
  void StageCompareHunk();
  void StageCompareSelectedLines();
  void UnstageCompareHunk();
  void UnstageCompareSelectedLines();
  void OpenDiscardCompareHunkPrompt();
  void OpenDiscardCompareSelectedLinesPrompt();
  void RefreshOpenCompareTabsForPath(const std::filesystem::path& path);
  bool OpenMergeEditor(const std::filesystem::path& base_path,
                       const std::filesystem::path& incoming_path,
                       const std::filesystem::path& current_path,
                       const std::filesystem::path& output_path);
  void RefreshMergeTabDerivedState(MergeTabState& merge_tab) const;
  void PopulateCompareSyntaxTokensForWindow(CompareTabState& compare_tab, const SDL_FRect& rect);
  void UpdateMergeMaxVisualColumns(MergeTabState& merge_tab,
                                   std::span<const std::string> result_lines) const;
  void PopulateMergeSyntaxTokensForWindow(MergeTabState& merge_tab, const SDL_FRect& rect);
  void UpdateMergeTrackingAfterViewportEdit(MergeTabState& merge_tab);
  editor::TextViewport* ActiveEditableViewport();
  const editor::TextViewport* ActiveEditableViewport() const;
  std::vector<LspClient::Diagnostic> CollectLspContextDiagnostics(const editor::TextViewport& viewport, const editor::SelectionRange& range, lsp_encoding::PositionEncoding encoding) const;
  void PublishPluginGhostText(std::string_view owner,
                              const plugin::PluginHost::GhostTextRequest& request);
  void ClearPluginGhostText(std::string_view owner);
  bool AcceptGhostText();
  void DismissGhostText();
  void InvalidateGhostTextIfStale();
  void RefreshPluginEditorEventInterest();
  void SamplePluginEditorEvents();
  bool DispatchDuePluginEditorEvents();
  void MoveMergeSelection(int delta);
  void ApplyMergeChoice(compare::MergeChoice choice);
  void ResetMergeHunk();
  void ToggleMergeBasePane();
  void MarkMergeResolved();
  void MoveFileFinderSelection(int delta);
  void RenderCompareSurface(SDL_Renderer* renderer,
                            const SDL_FRect& rect,
                            CompareTabState& compare_tab,
                            bool draw_compare_caret,
                            const CompareRenderProjectInputs& project_inputs);
  struct CompareRowsPass;
  struct CompareSidePaint;
  void PaintCompareLeftSide(const CompareRowsPass& rows, const CompareSidePaint& side);
  void PaintCompareRightSide(const CompareRowsPass& rows, const CompareSidePaint& side);
  void RenderMergeSurface(SDL_Renderer* renderer,
                          const SDL_FRect& rect,
                          MergeTabState& merge_tab,
                          bool focused,
                          const DiffSurfacePaint& paint);
  void ShowTreeSidebar(const std::filesystem::path& root = {});
  void ShowSearchSidebar(std::string query = {}, bool temporary = false);
  void ShowProblemsSidebar();
  void ShowGitSidebar();
  void ShowTestsSidebar();
  SidebarModeRowLayout SidebarModeRow(const SDL_FRect& sidebar_rect) const;
  void RefreshProjectFiles();
  void RefreshGitSidebar();
  void ConsumeGitSidebarRefresh();
  bool RefreshProblemsSidebar();
  void ActivatePluginSurfacePreview(std::string_view owner, std::string_view surface_id,
                                    editor::SurfacePreviewSlot slot);
  void SyncPluginSurfacePreviewClosed();
  void RenderPluginSurfaceInto(SDL_Renderer* renderer, const SDL_FRect& rect,
                               const editor::SurfaceContent* content, float scroll_y);
  struct FramePrepPass;
  void PrepareFrameState(FramePrepPass& pass);
  void ResolveFrameLayout(FramePrepPass& pass);
  void PrepareChromeFrame(FramePrepPass& pass);
  void PrepareFrameCompletions(FramePrepPass& pass);
  void PrepareEditorPaneMetrics(FramePrepPass& pass);
  void PrepareEditorPaneUnits(FramePrepPass& pass);
  struct EditorPanesPass;
  void PrepareCompareFrontPaneUnits(FramePrepPass& pass, EditorPanesPass& panes, std::size_t gi,
                                    std::vector<DerivedDamage::Unit>& units);
  void PrepareMergeFrontPaneUnits(FramePrepPass& pass, EditorPanesPass& panes, std::size_t gi,
                                  std::vector<DerivedDamage::Unit>& units);
  void FillEditorPanePaint(FramePrepPass& pass, EditorPanesPass& panes, std::size_t gi, const EditorPaneLayout& pane,
                           EditorGroup& group, editor::TextViewport& viewport);
  void PrepareTextPaneUnits(FramePrepPass& pass, EditorPanesPass& panes, const EditorPaneLayout& pane,
                            editor::TextViewport* viewport, std::vector<DerivedDamage::Unit>& units);
  void PrepareWorkbenchChromeUnits(FramePrepPass& pass);
  void ResolvePointerOccluder(FramePrepPass& pass);
  void PreparePanelSurfaces(FramePrepPass& pass);
  void PrepareSidebarSurfaces(FramePrepPass& pass);
  void PrepareHoverFrame(FramePrepPass& pass);
  void PrepareWindowChromeUnits(FramePrepPass& pass);
  void PrepareTextInputOverlays(FramePrepPass& pass);
  void PrepareGitPanelSurface(FramePrepPass& pass);
  void PrepareTreeSidebarSurface(FramePrepPass& pass);
  void PrepareSidebarListSurface(FramePrepPass& pass);
  void PrepareSearchSidebarSurface(FramePrepPass& pass);
  void PrepareMenuBarSurface(FramePrepPass& pass);
  void PrepareSettingsCardSurface(FramePrepPass& pass);
  void PrepareMenuPopupSurfaces(FramePrepPass& pass);
  void PrepareBottomPanelHeaderSurface(FramePrepPass& pass);
  void PrepareOutputBodySurface(FramePrepPass& pass);
  void PrepareTerminalChromeSurface(FramePrepPass& pass);
  void PrepareTerminalGridSurface(FramePrepPass& pass);
  void PrepareDebugPaneSurface(FramePrepPass& pass);
  void PrepareTabStripUnits(FramePrepPass& pass);
  void PrepareOverlayUnits(const WorkspaceLayout& layout, bool overlay_visible);
  void PaintEditorPane(SDL_Renderer* renderer, std::size_t group_index, bool draw_caret);
  void DrawEditorInsets(SDL_Renderer* renderer, const SDL_FRect& pane_rect,
                        const editor::EditorViewMetrics& metrics, std::size_t scroll_line,
                        const editor::EditorViewModel& view_model);
  bool RefreshTestsSidebarState();
  SDL_FRect TreeSidebarCollapseButtonRect(const SDL_FRect& sidebar_rect) const;
  SDL_FRect TreeSidebarRefreshButtonRect(const SDL_FRect& sidebar_rect) const;
  bool CanOpenGitCommitButton() const;
  std::optional<SDL_FRect> GitSidebarOutgoingBaseButtonRect(const SDL_FRect& sidebar_rect) const;
  std::vector<std::string> GitSidebarSummaryLines() const;
  float GitSidebarSummaryHeight() const;
  float GitSidebarListTop(const SDL_FRect& sidebar_rect) const;
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
  const std::vector<GitSidebarLine>& BuildGitSidebarLines() const;
  bool ToggleGitSidebarDirectoryCollapsed(const std::string& tree_node_key);
  std::optional<std::size_t> SelectedGitSidebarLineIndex() const;
  bool OpenSelectedPluginSidebarItem();
  bool ToggleSelectedPluginSidebarItem();
  bool CanStageAllGitSidebarEntries() const;
  bool CanDiscardAllGitSidebarEntries() const;
  void InitializeCommitWorkflowService();
  void InitializeGitOperationService();
  std::string DispatchGitOperationAction(ActionId id, const std::vector<std::string>& args);
  bool OpenCommitWorkflow();
  void CloseCommitWorkflow();
  bool RequestCommitWorkflowCommit();
  float GitSidebarCommitWorkflowHeight(const SDL_FRect& sidebar_rect) const;
  void PrepareCommitBodyViewportForFrame(const SDL_FRect& sidebar_rect);
  bool StageGitSidebarEntry(std::size_t entry_index);
  bool DispatchGitSidebarAction(GitSidebarActionId action, std::size_t entry_index);
  void SetGitOutgoingBaseChoice(OutgoingBaseChoice choice);
  bool ReloadCleanEditorTabsForPath(
      const std::filesystem::path& path,
      EditorReloadEchoGuard echo_guard = EditorReloadEchoGuard::Check);
  std::optional<EditorHoverTarget> EditorHoverTargetAtPosition(float x, float y) const;
  std::optional<EditorHoverTarget> DiagnosticHoverTargetAtPosition(float x, float y) const;
  std::optional<EditorHoverTarget> DiagnosticHoverTargetForViewport(
      const editor::TextViewport& viewport,
      const TextGridInteractionLayout& interaction,
      std::span<const editor::PublishedDiagnostic> diagnostics,
      float x,
      float y) const;
  std::optional<editor::CodeLensDecoration> CodeLensAtPosition(float x, float y) const;
  std::optional<editor::CodeLensDecoration> AboveLensAtPosition(float x, float y) const;
  std::optional<EditorHoverTarget> PluginHoverTargetAtPosition(float x, float y) const;
  std::optional<EditorHoverTarget> PluginHoverTargetForViewport(
      const editor::TextViewport& viewport,
      const TextGridInteractionLayout& interaction,
      float x,
      float y) const;
  std::optional<EditorHoverTarget> PluginHoverTargetForLine(
      const std::filesystem::path& path,
      std::string_view line,
      std::size_t grid_row, std::size_t document_line,
      std::size_t tab_size,
      const TextGridInteractionLayout& interaction,
      float x,
      float y) const;
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
  SDL_FRect ComputeTwoBlockHoverCardRect(std::string_view title,
                                         std::string_view content,
                                         std::size_t max_title_lines,
                                         std::size_t max_content_lines,
                                         const SDL_FRect& anchor_rect,
                                         const SDL_FRect& editor_surface) const;
  void LayOutTwoBlockHoverCard(CardPaint& card,
                               const SDL_FRect& area,
                               std::string_view title,
                               std::string_view content,
                               std::size_t max_title_lines,
                               std::size_t max_content_lines) const;
  void LayOutEditorHoverCard(const EditorHoverPopupLayout& popup, CardPaint& card) const;
  void UpdateEditorHover(float x, float y);
  void ShowSignatureHelpPopup(std::string signature, std::string documentation);
  void MaybeExpireSignatureHelp();
  std::optional<EditorBlamePopupLayout> ActiveEditorBlamePopupLayout() const;
  bool PointerOver(const SDL_FRect& rect) const;
  std::optional<SDL_FRect> HoveredInteractiveRect(const WorkspaceLayout& layout,
                                                  float x, float y) const;
  std::optional<std::optional<SDL_FRect>> HoverOverOverlayAndWidgets(const WorkspaceLayout& layout, float x,
                                                                     float y) const;
  std::optional<std::optional<SDL_FRect>> HoverOverChrome(const WorkspaceLayout& layout, float x, float y) const;
  std::optional<std::optional<SDL_FRect>> HoverOverSidebar(const WorkspaceLayout& layout, float x, float y) const;
  std::optional<SDL_FRect> HoverOverPanels(const WorkspaceLayout& layout, float x, float y) const;
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
                                                     bool persistable = true,
                                                     bool left_exists = true,
                                                     bool right_exists = true) const;
  bool OpenWorkingTreeComparison(const std::filesystem::path& path,
                                 const std::string& left_ref,
                                 const std::string& left_label);
  bool OpenBranchHeadComparison(const std::filesystem::path& path, const std::string& left_ref,
                                const std::string& left_label, const std::string& right_ref,
                                const std::string& right_label,
                                const project::GitRevisionBlobCache* prefetched = nullptr,
                                const std::vector<std::filesystem::path>* review_files = nullptr);
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
  void ShowOverlay(OverlayMode mode);
  void DismissOverlay(bool focus_editor = false);
  void RevealBufferSearchMatch(const editor::SelectionRange& match);
  void ResetBufferSearchFoldRevealState(bool preserve_expanded_folds);
  void RefreshBufferSearch();
  void RefreshBufferSearch(bool reveal);
  void RefreshBufferSearchIfStale();
  bool BufferSearchMatchesAreFresh() const;
  void ToggleBufferSearchOption(BufferFindToggle toggle);
  void OpenBufferSearchFromProjectSearchResult();
  void RefreshProjectSearch(ProjectSearchRerun rerun = ProjectSearchRerun::Fresh);
  void StopProjectSearch();
  void ConsumeProjectSearchUpdates();
  void ResetOverlayScroll();
  float OverlayListStartOffset() const;
  std::size_t OverlayItemCount() const;
  std::size_t OverlaySelectedIndex() const;
  ScrollableListLayout ComputeOverlayListLayout(const SDL_FRect& overlay) const;
  void SetOverlaySelectedIndex(std::size_t index);
  void ClampOverlayScrollRow(const SDL_FRect& overlay);
  void RevealOverlaySelection(const SDL_FRect& overlay);
  bool ActivateOverlaySelection();
  void BeginProjectSearchEdit(ProjectSearchEditField field);
  void CommitProjectSearchEdit();
  void FlushProjectSearchEditField();
  void CancelProjectSearchEdit();
  bool ProjectSearchScopeExpanded() const;
  void ToggleProjectSearchScopeExpanded();
  std::string_view ProjectSearchCaseButtonLabel() const;
  bool ProjectSearchCanReplaceAll() const;
  bool ProjectSearchReplaceCaseSensitive() const;
  void ToggleProjectSearchPatternMode();
  void CycleProjectSearchCaseMode();
  void ToggleProjectSearchHiddenFiles();
  void RequestProjectSearchReplaceAll();
  void ReplaceAllProjectSearchMatches();
  void ApplyProjectReplaceOutcome(ProjectReplaceOutcome outcome);
  const std::vector<int>& BuildProjectSearchLineMap() const;
  int ProjectSearchLineForResult(std::size_t index) const;
  void MoveBufferSearchSelection(int delta);
  void MoveProjectSearchSelection(int delta);
  void ReplaceCurrentBufferSearchMatch();
  void ReplaceAllBufferSearchMatches();
  std::optional<editor::SelectionRange> ActiveBufferSearchMatch() const;
  bool DiscoverTestsForActiveBuffer(std::string* error_message = nullptr);
  bool RunTests(const std::vector<std::string>& test_ids, std::string* error_message = nullptr);
  bool RunAllDiscoveredTests(std::string* error_message = nullptr);
  const std::vector<std::string>* OutputChannelEntries(std::string_view id) const;
  void ConsumeLspCallbacks();
  std::filesystem::path ConfigStatePath() const;
  std::filesystem::path UserConfigPath() const;
  std::filesystem::path ProjectStateDirectory() const;
  void ApplyEditorPreferences(editor::TextViewport& viewport, bool include_contract = true) const;
  void ApplyEditorPreferences(editor::TextViewport& viewport, bool include_contract,
                              const EditorPreferenceSettings& settings) const;
  void ApplyDetectedIndentOnOpen(editor::TextViewport& viewport) const;
  void ApplyEditorPreferencesToAllTabs(bool refresh_language_contracts = true);
  void ApplyLanguageModeToActiveEditor(const std::string& language);
  editor::FoldingModel* EnsureActiveFoldingModelFresh();
  editor::FoldingModel* EnsureFoldingModelFreshForTab(TabEntry::EditorTabState* editor_tab,
                                                      editor::TextViewport* active_viewport);
  void RefreshEditorFoldingModels();
  bool WriteClipboardText(std::string_view text) const;
  bool WritePrimarySelectionText(std::string_view text) const;
  std::optional<std::string> ReadPrimarySelectionText() const;
  std::optional<std::string> SelectionTextWithContext();
  void SyncPrimarySelectionWithActiveEditor();
  void SyncPrimarySelectionWithTerminalSelection();
  std::size_t CountOpenBufferViews(const std::filesystem::path& path) const;
  std::unordered_map<std::string, std::size_t> OpenBufferViewCounts() const;
  bool PluginServesLspMenuAction(ActionId id) const;
  LspManager& CurrentLspManager();
  DapManager& CurrentDapManager();
  DapManager& EnsureProjectDapManager(ProjectWorkspaceState& state);
  void ConsumeDapCallbacks();
  void ConsumeControlCallbacks();
  ControlChannelService::CommandOutcome ExecuteControlCommand(const std::string& command_line);
  void MaybeStartControlChannel();
  bool StartDebugging(const LaunchConfig& config, const std::string& cwd = {});
  std::string StartDebuggingWithDefaultConfig();
  std::string StartNamedDebugConfig(const std::string& name);
  std::string StartAdHocDebug(const std::string& program, const std::vector<std::string>& args,
                              const std::string& type);
  void StopDebugging();
  bool IsDebugSessionStopped() const;
  bool DebugEnabled() const;
  bool EnsureDebuggerEnabledTransiently();
  void OpenBreakpointContextMenu(const std::filesystem::path& path, std::size_t line,
                                 const SDL_FRect& anchor_rect);
  void EditBreakpointModifierFromMenu(ActionId id);
  void BreakpointQuickActionFromMenu(ActionId id);
  void RemoveBreakpointFromMenu();
  void CommitBreakpointModifierPrompt();
  void OpenWatchExpressionPrompt(std::optional<std::size_t> index);
  void CommitWatchExpressionPrompt();
  void OpenDebugReplPrompt();
  void CommitDebugReplPrompt();
  void AppendDebugConsoleOutput(int session_id, const std::string& label,
                                const dap_protocol::DapOutputEvent& output);
  void ShowDebugConsole(int session_id, const std::string& label);
  void ShowDebugOutput();
  void RemoveDebugConsole(int session_id);
  const LspClient::ReadinessSnapshot& MenuLspReadiness() const;
  void ActiveLspStatusStrings(bool ensure_started, std::string& text, std::string& tooltip, StatusTone& tone);
  void BeginTrackedLspRequest();
  void FinishTrackedLspRequest();
  LspClient* LspClientForViewport(const editor::TextViewport& viewport,
                                  std::string* language_id = nullptr);
  void EnsureLspDocumentOpen(const editor::TextViewport& viewport,
                             LspClient& client,
                             std::string_view language_id);
  using DocumentSymbolsReady = std::function<void(
      const std::filesystem::path&, const std::string& error, std::vector<plugin::PluginHost::DocumentSymbolNode>)>;
  void QueryDocumentSymbols(const editor::TextViewport& viewport, DocumentSymbolsReady deliver);
  void QueryLspDocumentSymbolsInto(const editor::TextViewport& viewport, std::filesystem::path request_path,
                                   std::string plugin_error, DocumentSymbolsReady apply);
  void PublishLspDiagnostics(ProjectWorkspaceState& state, std::string uri,
                             lsp_encoding::PositionEncoding encoding,
                             std::vector<LspClient::Diagnostic> diagnostics);
  void SyncLspForActiveEditableLastChange();
  void CloseAllTabs();
  void ReloadCleanOpenBuffersFromDisk();
  void AppendTerminalPendingInput(std::string_view input);
  void EraseLastTerminalPendingInputCodepoint();
  void SubmitTerminalPendingInput();
  TextInputSurface CurrentTextInputSurface() const;
  TerminalPaneState* ActiveTerminalPane();
  const TerminalPaneState* ActiveTerminalPane() const;
  std::optional<std::size_t> FocusedTerminalTabIndex() const;
  void SyncTerminalFocusState();
  bool MoveActiveOutputTabTo(std::size_t index);
  void ConsumeTerminalSessionUpdates();
  bool BottomPanelVisible() const;
  bool ProjectTabStripVisible() const;
  bool BottomPanelShowsTerminal() const;
  bool BottomPanelShowsOutput() const;
  const render::TextRenderer& PanelTextRenderer() const;
  void ApplyTerminalFontPreferences();
  SDL_FRect ActiveTerminalBodyRect(const WorkspaceLayout& layout) const;
  LogSurfaceLayout ComputeTerminalPaneLogLayout(const SDL_FRect& body, float panel_height, const TerminalPaneState& pane, std::size_t line_count) const;
  LogSurfaceLayout ComputeBottomPanelLogLayout(const WorkspaceLayout& layout,
                                                   std::size_t line_count) const;
  int BottomPanelVisibleRows(float panel_height) const;
  int BottomPanelScrollRow(std::size_t line_count, int visible_rows) const;
  void SetBottomPanelScrollRow(int scroll_row, std::size_t line_count, int visible_rows);
  void RebaseActiveTerminalRowSpace();
  void PrepareTerminalPanesForFrame(TerminalTabState& tab, const TerminalPaneRectsLayout& rects, float panel_height);
  void ClearTerminalSelection();
  std::optional<TerminalLinkHit> TerminalLinkAtPoint(float x, float y) const;
  std::optional<TerminalSelectionPoint> TerminalSelectionPointAt(
      int x,
      int y,
      const std::vector<terminal::TerminalLine>& lines,
      std::size_t first_row = 0) const;
  std::optional<TerminalSelectionPoint> TerminalViewportPositionForPoint(int x, int y) const;
  bool OpenExternalUrl(std::string_view url);
  bool RevealPathInFileExplorer(const std::filesystem::path& directory);
  void ResizeTerminalToPanel(const WorkspaceLayout& layout);
  bool OpenFileInNewTab(const std::filesystem::path& path);
  bool SplitEditorGroup(EditorSplitOrientation orientation);
  bool FocusOtherEditorGroup();
  void FocusEditorGroup(std::size_t group_index);
  bool CloseEditorGroup();
  std::size_t EditorGroupCount() const;
  bool MoveActiveTabTo(std::size_t index);
  std::optional<std::size_t> FindTabIndexBySpecifier(std::string_view specifier,
                                                     std::string* error_message = nullptr) const;
  void OpenFile(const std::filesystem::path& path);
  void OpenFileAtLocation(const std::filesystem::path& path,
                          std::size_t line,
                          std::size_t column);
  bool HandleMouseButtonDown(const SDL_Event& event);
  std::optional<bool> MouseDownOnModals(const SDL_Event& event, const WorkspaceLayout& layout, DamageMark mark,
                                        const std::optional<SDL_FRect>& window_rect);
  std::optional<bool> MouseDownOnChrome(const SDL_Event& event, const WorkspaceLayout& layout, DamageMark mark);
  std::optional<bool> MouseDownOnDividersAndPanels(const SDL_Event& event, const WorkspaceLayout& layout,
                                                   DamageMark mark);
  std::optional<bool> MouseDownOpensContextMenu(const SDL_Event& event, const WorkspaceLayout& layout, DamageMark mark);
  bool MouseDownOnEditorArea(const SDL_Event& event, const WorkspaceLayout& layout, DamageMark mark);
  bool HandleMouseButtonUp(const SDL_Event& event);
  void AbandonPointerGestures();
  bool HandleMouseMotion(const SDL_Event& event);
  std::optional<bool> MotionDuringDrag(const SDL_Event& event, const WorkspaceLayout& layout, DamageMark mark);
  std::optional<bool> MotionDuringPointerGesture(const SDL_Event& event, const WorkspaceLayout& layout,
                                                 DamageMark mark);
  std::optional<bool> MotionOverOpenMenus(const SDL_Event& event, const WorkspaceLayout& layout);
  bool MotionHover(const SDL_Event& event, const WorkspaceLayout& layout, DamageMark mark,
                   bool previous_mouse_position_valid, float previous_mouse_x, float previous_mouse_y);
  bool HandleMouseWheel(const SDL_Event& event);
  const ActionAvailability& MakeActionAvailability() const;
  void DrawFilledRect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) const;
  void DrawRect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) const;
  void RenderFrameBase(SDL_Renderer* renderer, const WorkspaceLayout& layout) const;
  void RenderActiveWorkspaceSurface(SDL_Renderer* renderer, const WorkspaceLayout& layout, bool draw_editor_caret);
  void RenderWindowChrome(SDL_Renderer* renderer, const WorkspaceLayout& layout) const;
  void RenderSidebarSurface(SDL_Renderer* renderer, const WorkspaceLayout& layout);
  void RenderOverlaySurface(SDL_Renderer* renderer,
                            const WorkspaceLayout& layout,
                            const OverlaySurfaceViewModel& overlay_vm);
  void RenderFindWidget(SDL_Renderer* renderer, const OverlayFindWidgetViewModel& fw_vm);
  void RenderDebugToolbar(SDL_Renderer* renderer) const;
  void PaintEditorAreaChrome(SDL_Renderer* renderer, const WorkspaceLayout& layout) const;
  std::optional<float> DebugToolbarAvoidBelowY(const WorkspaceLayout& layout) const;
  bool HandleDebugToolbarButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool DebugToolbarVisible() const;
  void RenderBottomPanelSurface(SDL_Renderer* renderer, const WorkspaceLayout& layout);
  void RenderDebugPaneSurface(SDL_Renderer* renderer, const WorkspaceLayout& layout);
  DebugPaneModeRowLayout DebugPaneModeRow(const SDL_FRect& pane_rect) const;
  LogSurfaceLayout ComputeDebugPaneListLayout(const WorkspaceLayout& layout,
                                                  std::size_t line_count) const;
  std::size_t DebugPaneActiveRowCount() const;
  void RevealDebugPaneSelection();
  void FocusDebugCallStackRow(std::size_t row);
  void ActivateDebugBreakpointRow(std::size_t row, bool toggle);
  bool DebugPaneRowIsActionable(std::size_t row) const;
  int DebugPaneScrollRow(std::size_t line_count, int visible_rows) const;
  void SetDebugPaneScrollRow(int scroll_row, std::size_t line_count, int visible_rows);
  void RenderMenuPopups(SDL_Renderer* renderer, const WorkspaceLayout& layout) const;
  void RenderStatusBar(SDL_Renderer* renderer, const WorkspaceLayout& layout) const;
  void PrepareStatusBarFrame(const WorkspaceLayout& layout);
  void RefreshStatusBar();
  const editor::TextViewport* StatusBarSourceViewport() const;
  void ApplyLiveSettings();
  bool SetSettingValue(std::string_view id, std::string value, bool persist = true);
  bool SetSettingAsUserDefault(std::string_view id, std::string value, bool persist = true);
  bool WriteSettingValue(std::string_view id, std::string value, bool as_user_default,
                         bool persist);
  bool ResetSettingValue(std::string_view id);
  bool ResetSettingInScope(std::string_view id, SettingScope scope);
  void ApplyCanonicalPreferenceSideEffects(std::string_view id);
  bool LineNumbersEnabled() const;
  bool SettingWritesToUserDefault(std::string_view id) const;
  bool WriteSettingRespectingScope(std::string_view id, std::string value);
  void ToggleSettingScope(std::string_view id);
  void RefreshSettingsOverlayCatalog();
  void OpenSettingsOverlay();
  void OpenHelpAboutOverlay();
  void CloseSettingsOverlay();
  void RenderSettingsOverlay(SDL_Renderer* renderer) const;
  bool HandleSettingsOverlayButtonDown(const SDL_Event& event, const WorkspaceLayout& layout);
  void StepSetting(std::string_view id, bool forward);
  void BeginSettingValueEdit(std::string_view id);
  void CommitSettingValueEdit();
  void CancelSettingValueEdit();
  void MoveSettingsFontPicker(int delta);
  void ApplySettingsFontPickerIndex(int dropdown_index);
  const std::vector<std::string>& CachedFontFamilies();
  void TogglePluginEnabled(std::string_view plugin_id);
  void EnsureSettingsSelectionVisible();
  void ScrollSettingsOverlayRows(int delta);
  void RenderPromptSurface(
      SDL_Renderer* renderer,
      const WorkspaceLayout& layout) const;
  void RenderDirtyPromptSurface(SDL_Renderer* renderer, const WorkspaceLayout& layout) const;
  void LayOutTextInputMarks(const std::optional<TextInputVisual>& visual, CardPaint& marks) const;
  void LayOutTextInputCaret(const std::optional<TextInputVisual>& visual, CardPaint& caret) const;
  std::optional<TextInputVisual> BuildActiveTextInputVisual(
      const WorkspaceLayout& layout,
      const std::optional<SDL_FRect>& active_editor_pane_rect) const;
  SingleLineViewMetrics ComputeSingleLineViewMetrics(const editor::SingleLineEditor& state,
                                                     std::string_view prefix,
                                                     float available_width) const;
  std::size_t HitTestSingleLineByteOffset(const editor::SingleLineEditor& state,
                                          std::string_view prefix,
                                          float available_width,
                                          float relative_x) const;
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
  bool HandleSingleLineInputMouseDown(const SDL_Event& event, const WorkspaceLayout& layout);
  bool HandleSingleLineInputDrag(const SDL_Event& event, const WorkspaceLayout& layout);
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
  std::optional<HoverTooltip> HoveredTooltip(const WorkspaceLayout& layout) const;
  std::optional<SDL_FRect> HoveredStatusBarSegmentRect(const WorkspaceLayout& layout, float x,
                                                       float y) const;
  std::optional<SDL_FRect> FocusSurfaceRect(const WorkspaceLayout& layout,
                                            FocusTarget focus) const;
  void DrawSurfaceFocusRing(SDL_Renderer* renderer, const SDL_FRect& region) const;
  void EnsureOutputChannelTabOpen(std::string_view channel_id);
  void CloseOutputChannelTab(std::string_view channel_id);
  SDL_FRect ComputeOverlayRect(const SDL_FRect& editor_area) const;
  SDL_FRect FindWidgetAnchorRect(const SDL_FRect& fallback) const;
  SDL_FRect ComputeCaretAnchoredOverlayRect(const SDL_FRect& editor_area,
                                            const SDL_FRect& caret_anchor) const;
  SDL_FRect ComputeCenteredMenuOverlayRect(const SDL_FRect& editor_area) const;
  const std::string& BreadcrumbLabel() const;
  const std::string& BreadcrumbLabel(std::size_t group_index) const;
  mutable std::array<BreadcrumbLabelCache, kMaxEditorGroups> breadcrumb_label_cache_;
  std::string ProjectLabel() const;
  ControlUiQuery::Operations MakeControlUiOperations();
  void ControlUiMenus(const WorkspaceLayout& layout, ControlUiElements& out);
  void ControlUiTabs(const WorkspaceLayout& layout, ControlUiElements& out);
  void ControlUiSidebar(const WorkspaceLayout& layout, ControlUiElements& out);
  void ControlUiPanel(const WorkspaceLayout& layout, ControlUiElements& out);
  void ControlUiButtons(const WorkspaceLayout& layout, ControlUiElements& out);
  void ControlUiOverlay(const WorkspaceLayout& layout, ControlUiElements& out);
  void ControlUiDialogs(const WorkspaceLayout& layout, ControlUiElements& out);
  void ControlUiFields(const WorkspaceLayout& layout, ControlUiElements& out);
  void ControlUiDebug(const WorkspaceLayout& layout, ControlUiElements& out);
  void ControlUiDetails(const WorkspaceLayout& layout, util::JsonObject& root);
  std::string ProjectTabDisplayTitle(std::size_t index) const;
  std::string ProjectTabTooltipLabel(std::size_t index) const;
  std::string_view TruncateLabelView(std::string_view text, float max_width) const;
  std::optional<SDL_FRect> CurrentWindowRect() const;
  std::optional<WorkspaceLayout> CurrentWorkspaceLayout() const;
  WorkspaceLayoutInputs CurrentLayoutInputs(float window_width, float window_height) const;
  const WindowChromeState& CurrentWindowChromeState() const;
  void NoteLayoutInputsChanged();
  void RequestFullRedraw();
  void RequestPreparedFrame();
  void NoteActiveEditableEdit();
  void Notify(NotificationService::Tone tone, std::string message);
  void Notify(NotificationService::Request request);
  void RequestRedrawRect(const SDL_FRect& rect);
  void RequestCursorPresent();
  void RequestOuterLayoutChangeRedraw(const WorkspaceLayout& previous_layout,
                                      SDL_FRect WorkspaceLayout::*first,
                                      SDL_FRect WorkspaceLayout::*second);
  void RequestSidebarLayoutChangeRedraw(const WorkspaceLayout& previous_layout);
  void RequestActiveEditableChangeRedraw(const std::vector<std::string>& before_lines,
                                         const std::vector<std::string>& after_lines);
  void RequestBottomPanelLayoutChangeRedraw(const WorkspaceLayout& previous_layout);
  void RequestSingleLineDragSurfaceRedraw(DamageMark mark, TextInputSurface surface);
  [[nodiscard]] DamageMark MarkDamage() const { return damage_generation_; }
  template <typename RequestRedraw>
  void EnsureRedraw(DamageMark since, RequestRedraw request_redraw) {
    if (damage_generation_ == since) request_redraw();
  }
  void QueueEditorHoverRefresh();
  util::InlineVector<SDL_FRect, 5> CurrentChromeRects() const;
  std::optional<SDL_FRect> CurrentEditorLineRangeRect(std::size_t start_line,
                                                      std::size_t end_line) const;
  std::optional<SDL_FRect> CurrentCompareRowRangeRect(std::size_t start_row,
                                                      std::size_t end_row) const;
  std::optional<SDL_FRect> CurrentMergeConflictRect(std::size_t conflict_index) const;
  std::optional<Uint32> NextCaretBlinkDelayMs() const;
  Uint64 AutosaveDelayMs() const;
  void MaybeArmAutosaveTimer();
  std::optional<Uint32> NextAutosaveDelayMs() const;
  void MaybeArmSessionFlushTimer();
  std::optional<Uint32> NextSessionFlushDelayMs() const;
  void FlushSessionStateForCrashSafety();
  std::optional<Uint32> NextTabSlideDelayMs() const;
  bool AdvanceTabSlide();
  std::optional<SDL_FRect> TabStripRectForKind(TabDragKind kind, std::size_t group_index) const;
  void ResetCaretBlink();
  bool ShouldBlinkCaret() const;
  bool CaretBlinkEnabled() const;
  Uint64 CaretBlinkIntervalMs() const;
  bool CaretBlinkAnimating() const;
  bool CaretVisibleNow() const;
  std::optional<SDL_FRect> ActiveEditorCaretRect(const WorkspaceLayout& layout) const;
  std::optional<SDL_FRect> ActiveTerminalCaretRect(const WorkspaceLayout& layout) const;
  std::optional<TextInputVisual> BuildCompareTextInputVisual(const SDL_FRect& editor_surface) const;
  std::optional<TextInputVisual> BuildMergeTextInputVisual(const SDL_FRect& editor_surface) const;
  void RenderCompareScrollbars(SDL_Renderer* renderer, const SDL_FRect& editor_surface,
                               CompareTabState& compare_tab, const DiffSurfacePaint& paint);
  void RenderMergeScrollbars(SDL_Renderer* renderer, const SDL_FRect& editor_surface,
                             MergeTabState& merge_tab, const DiffSurfacePaint& paint);
  CursorKind CursorKindForPosition(float x, float y) const;
  std::optional<CursorKind> CursorKindOverModalsAndMenus(float x, float y, const WorkspaceLayout& layout) const;
  std::optional<CursorKind> CursorKindOverChrome(float x, float y, const WorkspaceLayout& layout) const;
  std::optional<CursorKind> CursorKindOverSidebar(float x, float y, const WorkspaceLayout& layout) const;
  std::optional<CursorKind> CursorKindOverPanels(float x, float y, const WorkspaceLayout& layout) const;
  CursorKind CursorKindOverEditorArea(float x, float y, const WorkspaceLayout& layout) const;
  CursorKind CursorKindOverCompare(float x, float y, const SDL_FRect& view_surface) const;
  CursorKind CursorKindOverMerge(float x, float y, const SDL_FRect& view_surface) const;
  SDL_Cursor* CursorHandle(CursorKind kind);
  void ClearMouseHoverState();
  void UpdateMouseCursor(float x, float y, bool update_editor_hover = true,
                         bool workspace_layout_recomputed = false,
                         bool during_frame_prepare = false);

  render::Theme theme_ = render::MakeDefaultTheme();
  render::TextRenderer text_renderer_;
  render::TextRenderer editor_text_renderer_{text_renderer_.backend_pool()};
  render::TextRenderer terminal_text_renderer_{text_renderer_.backend_pool()};
  std::size_t terminal_grid_metrics_generation_ = 0;
  std::uint64_t last_applied_terminal_font_settings_revision_ = std::numeric_limits<std::uint64_t>::max();
  editor::EditorViewRenderer editor_view_renderer_;
  WorkspaceContext context_;
  std::unique_ptr<ShellGlueCache> glue_ = std::make_unique<ShellGlueCache>();
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
  static constexpr std::size_t kMenuSlotCount = kMenuIdCount;
  mutable std::array<std::vector<DynamicMenuEntryStorage>, kMenuSlotCount> dynamic_menu_entries_;
  mutable std::array<std::vector<MenuItemSpec>, kMenuSlotCount> dynamic_menu_items_;
  mutable std::vector<ResolvedKeybinding> resolved_keybindings_cache_;
  mutable std::vector<std::string> resolved_keybindings_disabled_ids_snapshot_;
  mutable std::size_t resolved_keybindings_reload_generation_ = std::numeric_limits<std::size_t>::max();
  mutable std::uint64_t resolved_keybindings_settings_revision_ = std::numeric_limits<std::uint64_t>::max();
  mutable StatusItemCache status_item_cache_;
  WorkspaceProjectSearchRuntime project_search_runtime_;
  project::ProjectChangeCoalescer project_change_coalescer_;
  project::GitRepositoryMetadataTracker git_metadata_tracker_;
  std::uint64_t last_applied_project_change_generation_ = 0;
  project::ProjectBackgroundExecutor project_background_executor_;
  GitRepositoryService git_repository_service_{project_background_executor_};
  PatchApplyService patch_apply_service_{project_background_executor_, git_repository_service_};
  CommitWorkflowService commit_workflow_service_{project_background_executor_, git_repository_service_};
  GitOperationService git_operation_service_{project_background_executor_};
  util::MainThreadMailbox compare_picker_mailbox_;
  std::uint64_t compare_picker_generation_ = 0;
  project::ProjectBackgroundExecutor interactive_background_executor_;
  std::function<project::GitFileHistoryResult(const std::filesystem::path& root,
                                              const std::filesystem::path& path)>
      compare_picker_file_history_provider_;
  std::function<std::vector<project::GitBranchReference>(const std::filesystem::path& root)>
      compare_picker_branches_provider_;
  std::function<std::vector<project::GitCommitEntry>(const std::filesystem::path& root,
                                                    std::size_t limit)>
      compare_picker_recent_commits_provider_;
  project::ProjectBackgroundExecutor media_background_executor_;
  render::SurfaceTextureCache surface_texture_cache_{media_background_executor_};
  std::mutex save_tab_mutex_;
  std::unique_ptr<platform::FileIndexWatcher> file_index_watcher_;
  std::atomic<std::uint64_t> file_index_watcher_generation_{0};
  std::atomic<bool> file_index_has_pending_changes_{false};
  util::MainThreadMailbox file_index_refresh_mailbox_;
  util::MainThreadMailbox project_replace_mailbox_;
  std::uint64_t project_replace_generation_ = 0;
  std::atomic<bool> file_index_initial_build_in_flight_{false};
  std::atomic<bool> project_file_event_pending_{false};
  std::atomic<bool> project_index_truncated_notice_pending_{false};
  WorkspacePluginRuntime plugin_runtime_;
  std::size_t reload_plugins_invocation_count_ = 0;
  std::size_t reload_clean_open_buffers_from_disk_invocation_count_ = 0;
  std::size_t render_compare_surface_invocation_count_ = 0;
  std::optional<WorkspaceLayout> prepared_frame_layout_;
  mutable TabStripService tab_strip_service_;
  mutable std::optional<WorkspaceLayoutInputs> layout_memo_inputs_;
  mutable WorkspaceLayout layout_memo_{};
  mutable std::uint64_t layout_memo_generation_ = 0;
  mutable std::optional<WorkspaceLayout> editor_group_rects_memo_layout_;
  mutable std::uint64_t editor_group_rects_memo_tree_revision_ = 0;
  mutable EditorGroupRectsLayout editor_group_rects_memo_{};
  std::size_t prepare_frame_layout_compute_count_ = 0;
  bool prepared_frame_draw_editor_caret_ = false;
  PreparedFrameState frame_state_;
  std::uint64_t prepared_frame_id_ = 0;
  mutable std::vector<VisiblePopupMenuItem> menu_popup_rows_scratch_;
  mutable LspClient::ReadinessSnapshot menu_lsp_readiness_{};
  mutable std::uint64_t menu_lsp_readiness_frame_id_ = 0;
  mutable bool clip_frame_overlay_view_models_valid_ = false;
  mutable std::uint64_t clip_frame_overlay_view_models_frame_id_ = 0;
  mutable WorkspaceLayout clip_frame_overlay_view_models_layout_{};
  mutable std::optional<FrameSurfaceViewModel> clip_cached_frame_vm_;
  mutable std::optional<OverlaySurfaceViewModel> clip_cached_overlay_vm_;
  mutable std::optional<SidebarSurfaceViewModel> prepare_cached_sidebar_vm_;
  mutable std::optional<BottomPanelSurfaceViewModel> prepare_cached_bottom_panel_vm_;
  mutable std::optional<DebugPaneSurfaceViewModel> prepare_cached_debug_pane_vm_;
  mutable std::optional<TextInputSurfaceViewModel> prepare_cached_text_input_vm_;
  std::optional<SDL_FRect> last_terminal_panel_rect_;
  std::uint64_t last_terminal_grid_key_ = 0;
  WorkspaceOutputChannels output_channels_;
  FormatterRegistry formatter_registry_;
  SaveParticipantRegistry save_participant_registry_;
  CompletionRegistry completion_registry_;
  CodeActionRegistry code_action_registry_;
  ToolRegistry tool_registry_;
  TestController test_controller_;
  ScmRegistry scm_registry_;
  AnnotationRegistry annotation_registry_;
  WorkspaceThemeRegistry theme_registry_;
  WorkspaceFileIconRegistry file_icon_registry_;
  VirtualDocumentRegistry virtual_document_registry_;
  PersistenceService persistence_service_;
  RecentsService recents_service_;
  LayoutModeService layout_mode_service_;
  WorkspaceTabStripChrome tab_strip_chrome_;
  StatusBarService status_bar_service_;
  StatusBarModelService status_bar_model_service_;
  NotificationService notification_service_;
  SaveFormatterService save_formatter_service_;
  project::FileIoServices file_io_;
  SettingsOverlayService settings_overlay_service_;
  QuickPickService quick_pick_service_;
  TerminalFindService terminal_find_service_;
  bool HandleTerminalFindKeyDown(const SDL_KeyboardEvent& event, SDL_Keymod modifiers);
  bool HandleTerminalFindMouseDown(float x, float y);
  AssistService assist_service_;
  WorkspaceEditService workspace_edit_service_;
  LspService lsp_service_;
  DebugService debug_service_;
  ControlChannelService control_channel_service_;
  WorkspaceLanguageContract language_contract_;
  PluginEditorEventTracker plugin_editor_event_tracker_;
  struct LiveSettingsEditorSnapshot {
    std::filesystem::path project_root;
    std::size_t tab_size = 0;
    std::size_t indent_width = 0;
    int font_size = 0;
    bool soft_tabs = false;
    bool soft_wrap = false;
    std::string language_contract_inputs;
    std::optional<std::string> save_trim_trailing_whitespace;
    std::optional<std::string> save_ensure_final_newline;
  };
  std::optional<LiveSettingsEditorSnapshot> last_live_settings_editor_snapshot_;
  std::uint64_t last_applied_settings_revision_ = std::numeric_limits<std::uint64_t>::max();
  mutable std::uint64_t project_tabs_hide_when_single_revision_ = std::numeric_limits<std::uint64_t>::max();
  mutable bool project_tabs_hide_when_single_ = false;
  int last_applied_terminal_scrollback_ = -1;
  bool last_applied_follow_out_of_root_symlinks_ = false;
  std::string last_applied_files_exclude_;
  std::size_t replace_all_aggregate_cap_bytes_ = 512ull * 1024 * 1024;
  util::WakeChannel git_blame_event_type_ = 0;
  util::WakeChannel git_sidebar_event_type_ = 0;
  util::WakeChannel terminal_event_type_ = 0;
  util::WakeChannel project_file_event_type_ = 0;
  util::WakeChannel project_open_dialog_event_type_ = 0;
  util::WakeChannel lsp_event_type_ = 0;
  util::WakeChannel dap_event_type_ = 0;
  util::WakeChannel control_event_type_ = 0;
  util::WakeChannel plugin_thread_event_type_ = 0;
  util::WakeChannel highlight_prefetch_event_type_ = 0;
  AutomaticGitRefreshPacer automatic_git_refresh_pacer_{git_sidebar_event_type_};
  bool pending_tree_git_badge_refresh_after_paint_ = false;
  project::GitBlameService git_blame_service_;
  editor::HighlightPrefetchService highlight_prefetch_service_;
  EditorBlameOverlayService editor_blame_overlay_service_;
  std::optional<EditorHoverTarget> active_editor_hover_target_;
  std::optional<SignatureHelpPopup> active_signature_help_;
  bool editor_hover_refresh_pending_ = false;
  mutable SystemClipboard clipboard_;
  std::function<std::optional<std::string>()> primary_selection_text_reader_;
  std::function<bool(std::string_view)> primary_selection_text_writer_;
  std::function<bool(std::string_view)> external_url_opener_;
  std::function<bool(const std::filesystem::path&)> file_manager_opener_;
  SDL_Window* dialog_window_ = nullptr;
  NativePickerState project_dialog_state_;
  NativePickerState open_file_dialog_state_;
  FontFileDialogState font_file_dialog_state_;
  std::vector<std::string> cached_font_families_;
  bool font_families_cached_ = false;
  bool quit_requested_ = false;
  WorkspaceStartupOptions startup_options_;
  std::vector<std::string> available_colorscheme_names_;
  render::ThemeNameCatalog theme_name_catalog_;
  float ui_scale_ = 1.0f;
  float presentation_scale_x_ = 1.0f;
  float presentation_scale_y_ = 1.0f;
  WindowAction pending_window_action_ = WindowAction::None;
  Uint64 caret_blink_epoch_ms_ = 0;
  Uint64 autosave_edit_epoch_ms_ = 0;
  std::uint64_t autosave_last_content_revision_ = 0;
  const editor::TextViewport* autosave_last_viewport_ = nullptr;
  bool autosave_armed_ = false;
  CrashSafetyFlushSchedule session_flush_;
  std::uint64_t session_flush_content_fingerprint_ = 0;
  std::optional<TextInputVisual> prepared_text_input_visual_;
  RenderInvalidation pending_render_invalidation_;
  DamageMark damage_generation_ = 0;
  int post_render_redraws_remaining_ = 0;
  CursorKind cursor_kind_ = CursorKind::Default;
  bool force_cursor_reassert_ = false;
  struct CursorKindFingerprint {
    float x = 0.0f;
    float y = 0.0f;
    bool valid = false;
    std::uint32_t hit_generation = 0;
    std::uint32_t hover_generation = 0;
  };
  CursorKindFingerprint cursor_kind_fingerprint_{};
  std::uint32_t cursor_hit_generation_ = 0;
  std::uint32_t editor_hover_target_generation_ = 0;
  struct SdlCursorDeleter {
    void operator()(SDL_Cursor* cursor) const;
  };
  std::array<std::unique_ptr<SDL_Cursor, SdlCursorDeleter>, kCursorKindCount> system_cursors_{};
  float last_mouse_x_ = 0.0f;
  float last_mouse_y_ = 0.0f;
  bool last_mouse_position_valid_ = false;
  mutable TerminalRowPaintPlan terminal_row_plan_;
  std::vector<editor::RowFillSpan> compare_right_fill_scratch_;
  editor::DecoratedTextRow compare_left_scratch_row_;
  editor::DecoratedTextRow compare_right_scratch_row_;
  std::vector<editor::DecoratedTextFill> compare_whitespace_scratch_;
  editor::DecoratedTextRow merge_incoming_scratch_row_;
  editor::DecoratedTextRow merge_current_scratch_row_;
