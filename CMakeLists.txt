cmake_minimum_required(VERSION 3.28)

project(microide VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

option(MICROIDE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(MICROIDE_ENABLE_SDL3_TTF "Enable SDL3_ttf text backend when available" ON)
option(MICROIDE_ENABLE_LUA_PLUGINS "Enable Lua-based manual plugins" ON)
option(MICROIDE_PERF_HARNESS_BUILD "Enable perf harness instrumentation and targets" OFF)
option(MICROIDE_FUZZ "Enable libFuzzer targets" OFF)

# When CMAKE_INTERPROCEDURAL_OPTIMIZATION is requested (e.g. via the
# microide-perf preset or a release build), verify the toolchain actually
# supports it. Without LTO, cross-translation-unit calls in hot paths
# (notably TextViewport ↔ EditorViewRenderer ↔ ViewState/HighlightCache)
# cannot be inlined and per-frame regressions show up on render-heavy
# scenarios like editor_sticky_scroll_scroll.
if(CMAKE_INTERPROCEDURAL_OPTIMIZATION)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _microide_ipo_supported OUTPUT _microide_ipo_output)
  if(NOT _microide_ipo_supported)
    message(WARNING
      "CMAKE_INTERPROCEDURAL_OPTIMIZATION was requested but is not supported "
      "by this toolchain; disabling. Diagnostic:\n${_microide_ipo_output}")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
  else()
    message(STATUS "Interprocedural optimization (LTO) enabled.")
  endif()
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/microide")

find_package(PkgConfig QUIET)
find_package(SDL3 CONFIG QUIET)

set(MICROIDE_SDL3_TARGET "")

if(TARGET SDL3::SDL3)
  set(MICROIDE_SDL3_TARGET SDL3::SDL3)
elseif(TARGET SDL3::SDL3-shared)
  set(MICROIDE_SDL3_TARGET SDL3::SDL3-shared)
elseif(TARGET SDL3::SDL3-static)
  set(MICROIDE_SDL3_TARGET SDL3::SDL3-static)
else()
  if(PkgConfig_FOUND)
    pkg_check_modules(SDL3 QUIET IMPORTED_TARGET sdl3)
    if(TARGET PkgConfig::SDL3)
      set(MICROIDE_SDL3_TARGET PkgConfig::SDL3)
    endif()
  endif()
endif()

if(MICROIDE_SDL3_TARGET STREQUAL "")
  message(FATAL_ERROR
    "SDL3 was not found. Install SDL3 development files and rerun CMake.\n"
    "Expected either an SDL3 CMake package or pkg-config entry named 'sdl3'.")
endif()

set(MICROIDE_SDL3_TTF_TARGET "")

if(MICROIDE_ENABLE_SDL3_TTF)
  find_package(SDL3_ttf CONFIG QUIET)

  if(TARGET SDL3_ttf::SDL3_ttf)
    set(MICROIDE_SDL3_TTF_TARGET SDL3_ttf::SDL3_ttf)
  elseif(TARGET SDL3_ttf::SDL3_ttf-shared)
    set(MICROIDE_SDL3_TTF_TARGET SDL3_ttf::SDL3_ttf-shared)
  elseif(TARGET SDL3_ttf::SDL3_ttf-static)
    set(MICROIDE_SDL3_TTF_TARGET SDL3_ttf::SDL3_ttf-static)
  elseif(PkgConfig_FOUND)
    pkg_check_modules(SDL3_TTF QUIET IMPORTED_TARGET sdl3-ttf)
    if(TARGET PkgConfig::SDL3_TTF)
      set(MICROIDE_SDL3_TTF_TARGET PkgConfig::SDL3_TTF)
    endif()
  endif()
endif()

set(MICROIDE_PCRE2_TARGET "")
find_package(PCRE2 QUIET COMPONENTS 8BIT)
if(TARGET PCRE2::8BIT)
  set(MICROIDE_PCRE2_TARGET PCRE2::8BIT)
elseif(PkgConfig_FOUND)
  pkg_check_modules(PCRE2 QUIET IMPORTED_TARGET libpcre2-8)
  if(TARGET PkgConfig::PCRE2)
    set(MICROIDE_PCRE2_TARGET PkgConfig::PCRE2)
  endif()
endif()

if(MICROIDE_PCRE2_TARGET STREQUAL "")
  message(FATAL_ERROR
    "PCRE2 was not found. Install PCRE2 development files and rerun CMake.")
endif()

set(MICROIDE_LUA_TARGET "")

if(MICROIDE_ENABLE_LUA_PLUGINS)
  if(PkgConfig_FOUND)
    pkg_check_modules(LUA54 QUIET IMPORTED_TARGET lua5.4)
    if(TARGET PkgConfig::LUA54)
      set(MICROIDE_LUA_TARGET PkgConfig::LUA54)
    else()
      pkg_check_modules(LUA_5_4 QUIET IMPORTED_TARGET lua-5.4)
      if(TARGET PkgConfig::LUA_5_4)
        set(MICROIDE_LUA_TARGET PkgConfig::LUA_5_4)
      endif()
    endif()
  endif()

  if(MICROIDE_LUA_TARGET STREQUAL "")
    find_path(MICROIDE_LUA_INCLUDE_DIR lua.hpp PATH_SUFFIXES lua5.4)
    find_library(MICROIDE_LUA_LIBRARY NAMES lua5.4)
    if(MICROIDE_LUA_INCLUDE_DIR AND MICROIDE_LUA_LIBRARY)
      add_library(microide_lua INTERFACE)
      target_include_directories(microide_lua INTERFACE ${MICROIDE_LUA_INCLUDE_DIR})
      target_link_libraries(microide_lua INTERFACE ${MICROIDE_LUA_LIBRARY})
      set(MICROIDE_LUA_TARGET microide_lua)
    else()
      message(WARNING "Lua 5.4 not found; disabling Lua plugin support.")
      set(MICROIDE_ENABLE_LUA_PLUGINS OFF)
    endif()
  endif()
endif()

add_library(microide_runtime_syntax_generated OBJECT
  src/editor/RuntimeSyntaxGenerated.cpp
)

target_include_directories(microide_runtime_syntax_generated PRIVATE src)

set(MICROIDE_CORE_SOURCES
  src/app/BackgroundTaskCounter.cpp
  src/editor/BracketScanner.cpp
  src/editor/DecoratedTextGridRenderer.cpp
  src/editor/DiagnosticsRender.cpp
  src/editor/DiagnosticsStore.cpp
  src/editor/EditorViewRenderer.cpp
  src/editor/FoldingModel.cpp
  src/editor/IndentDetect.cpp
  src/editor/IndentGuides.cpp
  src/editor/RuntimeSyntaxRegistry.cpp
  src/editor/ShapingActions.cpp
  src/editor/SingleLineEditor.cpp
  src/editor/SingleLineKeyHandler.cpp
  src/editor/SnippetEngine.cpp
  src/editor/SyntaxDefinitionLoader.cpp
  src/editor/SyntaxHighlighter.cpp
  src/editor/TextLayout.cpp
  src/editor/SaveNormalization.cpp
  src/editor/TextLayoutCache.cpp
  src/editor/TextViewport.cpp
  src/editor/TextViewportEditEngine.cpp
  src/editor/TextViewportFileIO.cpp
  src/editor/TextViewportHighlightCache.cpp
  src/editor/TextViewportLanguageBehavior.cpp
  src/editor/TextViewportMultiCaret.cpp
  src/editor/TextViewportUndoHistory.cpp
  src/editor/TextViewportViewState.cpp
  src/compare/CompareModel.cpp
  src/compare/ComparePatchExport.cpp
  src/project/PatchApplyTypes.cpp
  src/project/PatchGenerator.cpp
  src/project/GitPatchApply.cpp
  src/compare/ComparePresentationModel.cpp
  src/compare/CompareReviewTypes.cpp
  src/compare/BranchReviewStateTypes.cpp
  src/compare/BranchReviewStateService.cpp
  src/compare/CompareSemanticMetadata.cpp
  src/compare/MergeConflictKind.cpp
  src/compare/MergeModel.cpp
  src/platform/AppDirectories.cpp
  src/platform/AsyncSubprocess.cpp
  src/platform/FileIndexWatcher.cpp
  src/platform/FileWatcher.cpp
  src/platform/Filesystem.cpp
  src/platform/HostIntegration.cpp
  src/platform/HostPlatform.cpp
  src/platform/ProcessBackend.cpp
  src/platform/RuntimePaths.cpp
  src/platform/Subprocess.cpp
  src/platform/TerminalBackend.cpp
  src/platform/Trash.cpp
  src/persistence/PersistedRecord.cpp
  src/persistence/PersistedRecordDump.cpp
  src/persistence/PersistedRecordReader.cpp
  src/persistence/PersistedRecordWriter.cpp
  src/plugin/PluginLuaInterop.cpp
  src/plugin/PluginLuaProviderRegistrationInterop.cpp
  src/plugin/PluginLuaBufferProjectInterop.cpp
  src/plugin/PluginLuaContextInterop.cpp
  src/plugin/PluginContributionInterop.cpp
  src/plugin/PluginAsyncCallbackInterop.cpp
  src/plugin/PluginAsyncStateInterop.cpp
  src/plugin/PluginBufferLifecycleInterop.cpp
  src/plugin/PluginInstallRoot.cpp
  src/plugin/PluginDiscoveryInterop.cpp
  src/plugin/PluginDataDirectoryInterop.cpp
  src/plugin/PluginHoverQueryInterop.cpp
  src/plugin/PluginDiagnosticsInterop.cpp
  src/plugin/PluginLifecycleCallbackInterop.cpp
  src/plugin/PluginLifecycleLoadInterop.cpp
  src/plugin/PluginLifecycleResetInterop.cpp
  src/plugin/PluginPathInterop.cpp
  src/plugin/PluginProcessInterop.cpp
  src/plugin/PluginProjectLifecycleInterop.cpp
  src/plugin/PluginProviderQueryInterop.cpp
  src/plugin/PluginRuntimeApiInterop.cpp
  src/plugin/PluginRegistryInterop.cpp
  src/plugin/PluginProviderRegistrationParsers.cpp
  src/plugin/PluginRegistrationParsers.cpp
  src/plugin/PluginSidebarHoverInterop.cpp
  src/plugin/PluginStatusInterop.cpp
  src/plugin/PluginStateTeardownInterop.cpp
  src/plugin/PluginWorkspaceInterop.cpp
  src/plugin/LuaRuntime.cpp
  src/plugin/PluginHost.cpp
  src/project/DirectoryTree.cpp
  src/project/FileOperationService.cpp
  src/project/GitCompareService.cpp
  src/project/GitCommandUtil.cpp
  src/project/FileFinder.cpp
  src/project/FileIndex.cpp
  src/project/GitBlameParser.cpp
  src/project/GitBlameService.cpp
  src/project/GitPorcelainParser.cpp
  src/project/GitPorcelainV2Parser.cpp
  src/project/GitRepository.cpp
  src/project/GitRepositoryState.cpp
  src/project/CommitWorkflowChecks.cpp
  src/project/GitCommitExecutor.cpp
  src/project/GitRepositoryMetadataTracker.cpp
  src/project/ProjectChangeCoalescer.cpp
  src/project/ProjectChangeNormalizer.cpp
  src/project/ProjectFileScanner.cpp
  src/project/GitStatusService.cpp
  src/project/IgnoreMatcher.cpp
  src/project/PatternCache.cpp
  src/project/ProjectBackgroundExecutor.cpp
  src/project/SubprocessHelper.cpp
  src/project/ProjectSearchService.cpp
  src/project/ProjectRoot.cpp
  src/render/DebugTextBackend.cpp
  src/render/TextRenderer.cpp
  src/render/TextRendererBackend.cpp
  src/render/Theme.cpp
  src/terminal/TerminalSession.cpp
  src/terminal/TerminalAnsiColors.cpp
  src/terminal/TerminalSessionOutput.cpp
  src/terminal/TerminalSessionEscape.cpp
  src/terminal/TerminalSessionInput.cpp
  src/terminal/TerminalSessionInputEncoding.cpp
  src/terminal/TerminalSessionScreen.cpp
  src/terminal/TerminalBase64.cpp
  src/terminal/TerminalCsiParser.cpp
  src/terminal/TerminalMouseEncoder.cpp
  src/terminal/TerminalOscClipboard.cpp
  src/terminal/TerminalProcessControl.cpp
  src/util/JsonValue.cpp
  src/util/Parse.cpp
  src/util/PerformanceCounters.cpp
  src/util/PerformanceTrace.cpp
  src/util/StartupTrace.cpp
  src/util/StringUtil.cpp
  src/util/TaskExecutor.cpp
  src/util/TextFileIO.cpp
  src/util/WindowPresentation.cpp
  src/workspace/WorkspaceShellCompare.cpp
  src/workspace/WorkspaceShellCompareMerge.cpp
  src/workspace/WorkspaceShellCompareRender.cpp
  src/workspace/StatusBarModelService.cpp
  src/workspace/TabStripService.cpp
  src/workspace/WorkspaceTabStripChrome.cpp
  src/workspace/WorkspaceChromeMouseCoordinator.cpp
  src/workspace/WorkspaceCompareInteractionCoordinator.cpp
  src/workspace/WorkspaceCompareMouseCoordinator.cpp
  src/workspace/WorkspaceActionCoordinator.cpp
  src/workspace/WorkspaceActionAvailability.cpp
  src/workspace/WorkspaceActionServices.cpp
  src/workspace/WorkspaceActionServicesContextFactory.cpp
  src/workspace/WorkspaceActionRequests.cpp
  src/workspace/WorkspaceCommandParsing.cpp
  src/workspace/WorkspaceCommandRegistry.cpp
  src/workspace/WorkspaceCommandPromptCoordinator.cpp
  src/workspace/WorkspaceEditActionExecutor.cpp
  src/workspace/WorkspaceEditorMouseCoordinator.cpp
  src/workspace/WorkspaceEventOrchestrator.cpp
  src/workspace/WorkspaceGlobalActionExecutor.cpp
  src/workspace/WorkspaceIndentDetectApply.cpp
  src/workspace/WorkspaceKeyInputCoordinator.cpp
  src/workspace/WorkspaceKeyInputCoordinatorModal.cpp
  src/workspace/WorkspaceKeyInputCoordinatorSurfaces.cpp
  src/workspace/WorkspaceKeyInputCoordinatorSidebar.cpp
  src/workspace/WorkspaceKeyInputCoordinatorEditor.cpp
  src/workspace/WorkspaceMergeMouseCoordinator.cpp
  src/workspace/WorkspaceMenuCoordinator.cpp
  src/workspace/WorkspaceMenuRegistry.cpp
  src/workspace/WorkspacePanelMouseCoordinator.cpp
  src/workspace/WorkspaceProjectActionExecutor.cpp
  src/workspace/WorkspaceSearchActionExecutor.cpp
  src/workspace/WorkspaceSidebarActionExecutor.cpp
  src/workspace/WorkspaceTextInputCoordinator.cpp
  src/workspace/WorkspaceTextInputCoordinatorTerminal.cpp
  src/workspace/WorkspaceTabActionExecutor.cpp
  src/workspace/EditorBlameOverlayService.cpp
  src/workspace/WorkspaceShellCursor.cpp
  src/workspace/WorkspaceDiffTabCoordinator.cpp
  src/workspace/WorkspaceDirtyPromptCoordinator.cpp
  src/workspace/PromptSurfaceService.cpp
  src/workspace/WorkspaceShellEditor.cpp
  src/workspace/WorkspaceShellEditorSplits.cpp
  src/workspace/WorkspaceShellInteraction.cpp
  src/workspace/WorkspaceShellMenu.cpp
  src/workspace/WorkspaceShellMouse.cpp
  src/workspace/WorkspaceShellMouseMotion.cpp
  src/workspace/WorkspaceShellOverlay.cpp
  src/workspace/WorkspaceShellProjectSearch.cpp
  src/workspace/WorkspaceShellProjectChanges.cpp
  src/workspace/WorkspaceShellBufferSearch.cpp
  src/workspace/WorkspaceShellPresentation.cpp
  src/workspace/CaretRedraw.cpp
  src/workspace/WorkspaceShellRedraw.cpp
  src/workspace/WorkspaceShellRenderFrame.cpp
  src/workspace/WorkspaceShellRenderSidebar.cpp
  src/workspace/WorkspaceShellRenderOverlay.cpp
  src/workspace/WorkspaceShellRenderBottomPanel.cpp
  src/workspace/WorkspaceShellRenderMenus.cpp
  src/workspace/WorkspaceShellRenderPrompts.cpp
  src/workspace/WorkspaceShellRenderChrome.cpp
  src/workspace/WorkspaceShellRenderStatusBar.cpp
  src/workspace/WorkspaceShellRenderSettingsOverlay.cpp
  src/workspace/WorkspaceShellRenderTextInput.cpp
  src/workspace/RenderViewModelBuilder.cpp
  src/workspace/WorkspaceShellHoverPopup.cpp
  src/workspace/WorkspaceShellHoverTargets.cpp
  src/workspace/WorkspacePersistenceCoordinator.cpp
  src/workspace/WorkspaceShellPlugins.cpp
  src/workspace/WorkspaceShellLsp.cpp
  src/workspace/WorkspaceShellTestAccess.cpp
  src/workspace/WorkspaceShellOutput.cpp
  src/workspace/WorkspaceShellSettingsOverlay.cpp
  src/workspace/WorkspaceShellProjects.cpp
  src/workspace/WorkspacePathUtils.cpp
  src/workspace/WorkspacePathMutationCoordinator.cpp
  src/workspace/WorkspaceProjectSearchPresentation.cpp
  src/workspace/WorkspaceProjectPresentation.cpp
  src/workspace/WorkspaceProjectCatalogCoordinator.cpp
  src/workspace/AssistService.cpp
  src/workspace/CompareTabReview.cpp
  src/workspace/BranchReviewStateBridge.cpp
  src/workspace/CompareMergeService.cpp
  src/workspace/MergeResolverContext.cpp
  src/workspace/MergeResultValidation.cpp
  src/workspace/FileUri.cpp
  src/workspace/CompareMergeRender.cpp
  src/workspace/ProjectCatalogService.cpp
  src/workspace/WorkspacePathMutationCoordinatorDirty.cpp
  src/workspace/WorkspacePathMutationCoordinatorTabs.cpp
  src/workspace/WorkspaceLifecycleCoordinator.cpp
  src/workspace/WorkspaceLayout.cpp
  src/workspace/WorkspaceLayoutMergeAndChrome.cpp
  src/workspace/WorkspaceOutputChannels.cpp
  src/workspace/TerminalPanelService.cpp
  src/workspace/WorkspacePersistenceBinaryFormat.cpp
  src/workspace/WorkspacePersistenceBinaryFormatSessions.cpp
  src/workspace/GitRepositoryService.cpp
  src/workspace/PatchApplyService.cpp
  src/workspace/PersistenceService.cpp
  src/workspace/WorkspacePluginAssetMonitor.cpp
  src/workspace/WorkspacePluginRuntime.cpp
  src/workspace/WorkspaceProjectFileMonitor.cpp
  src/workspace/WorkspacePersistenceCoordinatorConfig.cpp
  src/workspace/WorkspacePersistenceCoordinatorSession.cpp
  src/workspace/WorkspacePersistenceCoordinatorWorkspaceSession.cpp
  src/workspace/WorkspaceProjectDialogCoordinator.cpp
  src/workspace/WorkspaceShellPrompts.cpp
  src/workspace/WorkspaceShellMerge.cpp
  src/workspace/WorkspaceShellMergeState.cpp
  src/workspace/WorkspaceShellMergeRender.cpp
  src/workspace/WorkspaceSidebarCoordinator.cpp
  src/workspace/SidebarService.cpp
  src/workspace/WorkspaceSidebarCoordinatorRefresh.cpp
  src/workspace/WorkspaceSidebarCoordinatorActions.cpp
  src/workspace/WorkspaceSidebarMouseCoordinator.cpp
  src/workspace/WorkspaceSidebarMouseCoordinatorScroll.cpp
  src/workspace/WorkspaceTabMouseCoordinator.cpp
  src/workspace/EditorTabService.cpp
  src/workspace/WorkspaceTabCoordinator.cpp
  src/workspace/WorkspaceTabCoordinatorSplits.cpp
  src/workspace/WorkspaceTabCoordinatorShellBridge.cpp
  src/workspace/WorkspaceProjectSearchRuntime.cpp
  src/workspace/WorkspaceProjectStateCoordinator.cpp
  src/workspace/WorkspaceGitOutgoingBase.cpp
  src/workspace/GitSidebarCommandCenter.cpp
  src/workspace/WorkspaceGitSidebarPresentation.cpp
  src/workspace/WorkspaceRootView.cpp
  src/workspace/WorkspaceShellBootstrapper.cpp
  src/workspace/WorkspaceShellRender.cpp
  src/workspace/WorkspaceShellSidebar.cpp
  src/workspace/WorkspaceShellCommitWorkflow.cpp
  src/workspace/CommitWorkflowState.cpp
  src/workspace/CommitWorkflowService.cpp
  src/workspace/WorkspaceShellSidebarMouse.cpp
  src/workspace/WorkspaceShellSingleLineInputMouse.cpp
  src/workspace/WorkspaceShellTerminal.cpp
  src/workspace/WorkspaceShellTerminalTabs.cpp
  src/workspace/WorkspaceKeybindingRegistry.cpp
  src/workspace/WorkspaceLanguageContract.cpp
  src/workspace/WorkspaceFoldingRefresh.cpp
  src/workspace/WorkspaceSettingsRegistry.cpp
  src/workspace/SettingsOverlayService.cpp
  src/workspace/WorkspaceStatusRegistry.cpp
  src/workspace/WorkspaceSidebarRegistry.cpp
  src/workspace/WorkspaceLspClient.cpp
  src/workspace/WorkspaceLspClientRequests.cpp
  src/workspace/WorkspaceLspManager.cpp
  src/workspace/WorkspaceFormatterRegistry.cpp
  src/workspace/WorkspaceSaveParticipants.cpp
  src/workspace/WorkspaceCompletionRegistry.cpp
  src/workspace/WorkspaceCodeActionRegistry.cpp
  src/workspace/WorkspaceToolRegistry.cpp
  src/workspace/WorkspaceToolDownloader.cpp
  src/workspace/WorkspaceTestController.cpp
  src/workspace/WorkspaceScmRegistry.cpp
  src/workspace/WorkspaceAnnotationRegistry.cpp
  src/workspace/WorkspaceVirtualDocument.cpp
  src/workspace/WorkspaceReviewComments.cpp
  src/workspace/WorkspaceTerminalSelection.cpp
  src/workspace/WorkspaceTextSearch.cpp
  src/workspace/WorkspaceViewTree.cpp
  src/workspace/WorkspaceShell.cpp
)

add_executable(microide
  src/app/main.cpp
  src/app/AppStartupOptions.cpp
  src/app/Application.cpp
  src/app/DirtyRegionPolicy.cpp
  $<TARGET_OBJECTS:microide_runtime_syntax_generated>
  ${MICROIDE_CORE_SOURCES}
)

if(APPLE)
  set_target_properties(microide PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "microide"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.microide.app"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
  )
elseif(WIN32)
  set_target_properties(microide PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets")
  set(MICROIDE_RUNTIME_ASSET_DIR "$<TARGET_FILE_DIR:microide>/assets")
  if(APPLE)
    set(MICROIDE_RUNTIME_ASSET_DIR "$<TARGET_BUNDLE_DIR:microide>/Contents/Resources/assets")
  endif()
  add_custom_command(TARGET microide POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "${MICROIDE_RUNTIME_ASSET_DIR}")
endif()

if(NOT MICROIDE_SDL3_TTF_TARGET STREQUAL "")
  target_sources(microide PRIVATE src/render/SdlTtfTextBackend.cpp)
  target_compile_definitions(microide PRIVATE MICROIDE_HAS_SDL3_TTF=1)
  target_link_libraries(microide PRIVATE ${MICROIDE_SDL3_TTF_TARGET})
endif()

target_include_directories(microide PRIVATE src)
target_link_libraries(microide PRIVATE ${MICROIDE_SDL3_TARGET} ${MICROIDE_PCRE2_TARGET})
if(NOT MICROIDE_LUA_TARGET STREQUAL "")
  target_link_libraries(microide PRIVATE ${MICROIDE_LUA_TARGET})
endif()
target_compile_definitions(microide PRIVATE
  MICROIDE_HAS_LUA_PLUGINS=$<BOOL:${MICROIDE_ENABLE_LUA_PLUGINS}>
)

if(UNIX AND NOT APPLE)
  target_link_libraries(microide PRIVATE util)
endif()

if(APPLE)
  target_link_libraries(microide PRIVATE "-framework CoreFoundation" "-framework CoreServices")
endif()

if(WIN32)
  get_filename_component(MICROIDE_TOOLCHAIN_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
  add_custom_command(TARGET microide POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            -DINPUT_FILE=$<TARGET_FILE:microide>
            -DOUTPUT_DIR=$<TARGET_FILE_DIR:microide>
            -DSEARCH_DIRS=${MICROIDE_TOOLCHAIN_BIN_DIR}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyRuntimeDependencies.cmake"
    COMMAND_EXPAND_LISTS)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(microide PRIVATE -Wall -Wextra -Wpedantic)
  if(MICROIDE_WARNINGS_AS_ERRORS)
    target_compile_options(microide PRIVATE -Werror)
  endif()
elseif(MSVC)
  target_compile_options(microide PRIVATE /W4)
  if(MICROIDE_WARNINGS_AS_ERRORS)
    target_compile_options(microide PRIVATE /WX)
  endif()
endif()

add_executable(microide_tests
  src/app/DirtyRegionPolicy.cpp
  $<TARGET_OBJECTS:microide_runtime_syntax_generated>
  ${MICROIDE_CORE_SOURCES}
  tests/ApplicationTests.cpp
  tests/AppDirectoriesTests.cpp
  tests/AppStartupOptionsTests.cpp
  src/app/AppStartupOptions.cpp
  tests/CompareModelTests.cpp
  tests/CompareReviewTests.cpp
  tests/BranchReviewStateTests.cpp
  tests/PatchApplyTests.cpp
  tests/DiagnosticsStoreTests.cpp
  tests/DirectoryTreeTests.cpp
  tests/DirtyRegionPolicyTests.cpp
  tests/FilesystemTests.cpp
  tests/FileFinderTests.cpp
  tests/FileIndexTests.cpp
  tests/FileOperationServiceTests.cpp
  tests/GitServiceTests.cpp
  tests/GitRepositoryStateTests.cpp
  tests/GitRepositoryServiceTests.cpp
  tests/CommitWorkflowTests.cpp
  tests/GitSidebarCommandCenterTests.cpp
  tests/WorkspaceLspClientTests.cpp
  tests/GitBlameServiceTests.cpp
  tests/MergeModelTests.cpp
  tests/MergeConflictResolutionTests.cpp
  tests/GitMergeConflictFixtures.cpp
  tests/ProjectSearchServiceTests.cpp
  tests/ProjectChangeTests.cpp
  tests/ExternalRepoChangeTests.cpp
  tests/RegexUtilTests.cpp
  tests/RuntimePathsTests.cpp
  tests/TaskExecutorTests.cpp
  tests/ProjectBackgroundExecutorTests.cpp
  tests/RenderViewModelBuilderTests.cpp
  tests/TerminalSessionTests.cpp
  tests/ThemeTests.cpp
  tests/TestMain.cpp
  tests/TestRunnerCli.cpp
  tests/TestRunnerCliTests.cpp
  tests/TestSupport.cpp
  tests/PluginHostTests.cpp
  tests/ParseTests.cpp
  tests/ArchitectureInvariantsTests.cpp
  tests/architecture/ArchitectureFileScanner.cpp
  tests/architecture/ArchitectureRuleHelpers.cpp
  tests/architecture/WorkspaceArchitectureRules.cpp
  tests/architecture/WorkspaceShellArchitectureRules.cpp
  tests/architecture/WorkspaceCoordinatorArchitectureRules.cpp
  tests/architecture/WorkspaceServiceArchitectureRules.cpp
  tests/architecture/WorkspaceViewModelArchitectureRules.cpp
  tests/architecture/WorkspaceFileSizeArchitectureRules.cpp
  tests/architecture/PluginArchitectureRules.cpp
  tests/architecture/TerminalArchitectureRules.cpp
  tests/BackgroundTaskCounterTests.cpp
  tests/FileIndexWatcherTests.cpp
  tests/PatternCacheTests.cpp
  tests/WorkspaceToolDownloaderTests.cpp
  tests/PersistedRecordDumpTests.cpp
  tests/PersistedRecordIoTests.cpp
  tests/PersistedStateRecordTests.cpp
  tests/StringUtilTests.cpp
  tests/SubprocessTests.cpp
  tests/TextRendererTests.cpp
  tests/TextViewportTests.cpp
  tests/EditorEssentialsTests.cpp
  tests/EditorRenderViewModelAllocationTests.cpp
  tests/EditorSnippetTests.cpp
  tests/FoldingModelTests.cpp
  tests/EditorFoldingTests.cpp
  tests/EditorMultiCaretTests.cpp
  tests/WindowPresentationTests.cpp
  tests/WorkspaceMenuRegistryTests.cpp
  tests/WorkspaceSettingsRegistryTests.cpp
  tests/WorkspaceShellChromeTests.cpp
  tests/WorkspaceShellEditorBlameTests.cpp
  tests/WorkspaceShellPluginTests.cpp
  tests/WorkspaceShellPromptTests.cpp
  tests/WorkspaceShellCompareTests.cpp
  tests/WorkspaceShellCursorTests.cpp
  tests/WorkspaceShellProjectTests.cpp
  tests/WorkspaceShellSearchTests.cpp
  tests/WorkspaceShellSessionTests.cpp
  tests/WorkspaceShellSourceControlTests.cpp
  tests/WorkspaceStatusBarTests.cpp
  tests/WorkspaceShellSharedCoreTests.cpp
  tests/WorkspaceShellSharedLayoutTests.cpp
  tests/WorkspaceShellSharedSearchTests.cpp
  tests/WorkspaceShellSharedTerminalTests.cpp
  tests/WorkspaceShellTerminalTests.cpp
  tests/ContributionRegistryTests.cpp
  tests/Phase3Tests.cpp
  tests/Phase4Tests.cpp
  tests/Phase5Tests.cpp
  tests/SingleLineEditorTests.cpp
  tests/WheelAccumulatorTests.cpp
  tests/AllocationCounterTests.cpp
  tests/PerfBaselineTests.cpp
  tests/PerfHarnessIsolationTests.cpp
  tests/perf/AllocationCounter.cpp
  tests/perf/Baseline.cpp
  tests/perf/PerfHarnessIsolation.cpp
)

if(NOT MICROIDE_SDL3_TTF_TARGET STREQUAL "")
  target_sources(microide_tests PRIVATE src/render/SdlTtfTextBackend.cpp)
  target_compile_definitions(microide_tests PRIVATE MICROIDE_HAS_SDL3_TTF=1)
  target_link_libraries(microide_tests PRIVATE ${MICROIDE_SDL3_TTF_TARGET})
endif()

target_include_directories(microide_tests PRIVATE src tests)
target_link_libraries(microide_tests PRIVATE ${MICROIDE_SDL3_TARGET} ${MICROIDE_PCRE2_TARGET})
if(NOT MICROIDE_LUA_TARGET STREQUAL "")
  target_link_libraries(microide_tests PRIVATE ${MICROIDE_LUA_TARGET})
endif()

if(APPLE)
  target_link_libraries(microide_tests PRIVATE "-framework CoreFoundation" "-framework CoreServices")
endif()

target_compile_definitions(microide_tests PRIVATE
  MICROIDE_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests"
  MICROIDE_TESTING=1
  MICROIDE_HAS_LUA_PLUGINS=$<BOOL:${MICROIDE_ENABLE_LUA_PLUGINS}>
  MICROIDE_PERF_HARNESS_BUILD=$<BOOL:${MICROIDE_PERF_HARNESS_BUILD}>
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(microide_tests PRIVATE -Wall -Wextra -Wpedantic)
  if(MICROIDE_WARNINGS_AS_ERRORS)
    target_compile_options(microide_tests PRIVATE -Werror)
  endif()
elseif(MSVC)
  target_compile_options(microide_tests PRIVATE /W4)
  if(MICROIDE_WARNINGS_AS_ERRORS)
    target_compile_options(microide_tests PRIVATE /WX)
  endif()
endif()

add_test(NAME microide_tests COMMAND microide_tests)

if(MICROIDE_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "MICROIDE_FUZZ requires clang/clang++ with libFuzzer support.")
  endif()

  function(microide_configure_fuzz_target target_name)
    target_include_directories(${target_name} PRIVATE src tests)
    target_compile_options(${target_name} PRIVATE
      -fsanitize=fuzzer,address
      -fno-omit-frame-pointer
    )
    target_link_options(${target_name} PRIVATE
      -fsanitize=fuzzer,address
    )
  endfunction()

  add_executable(PersistedRecordReaderFuzz
    src/persistence/PersistedRecord.cpp
    src/persistence/PersistedRecordReader.cpp
    src/persistence/PersistedRecordWriter.cpp
    tests/fuzz/PersistedRecordReaderFuzz.cpp
  )
  microide_configure_fuzz_target(PersistedRecordReaderFuzz)

  add_executable(SearchRegexFuzz
    tests/fuzz/SearchRegexFuzz.cpp
  )
  target_link_libraries(SearchRegexFuzz PRIVATE ${MICROIDE_PCRE2_TARGET})
  microide_configure_fuzz_target(SearchRegexFuzz)

  add_executable(GitBlameParserFuzz
    src/util/Parse.cpp
    src/project/GitBlameParser.cpp
    tests/fuzz/GitBlameParserFuzz.cpp
  )
  microide_configure_fuzz_target(GitBlameParserFuzz)
endif()

if(MICROIDE_PERF_HARNESS_BUILD)
  add_executable(microide_perf
    src/app/DirtyRegionPolicy.cpp
    $<TARGET_OBJECTS:microide_runtime_syntax_generated>
    ${MICROIDE_CORE_SOURCES}
    tests/perf/AllocationCounter.cpp
    tests/perf/PerfHarness.cpp
    tests/perf/PerfHarnessIsolation.cpp
    tests/perf/Baseline.cpp
    tests/perf/AdvisoryPerfScenarios.cpp
    tests/perf/GitWorkstationPerfScenarios.cpp
    tests/perf/EditorEssentialsPerfScenarios.cpp
    tests/perf/PerfMain.cpp
  )

  if(NOT MICROIDE_SDL3_TTF_TARGET STREQUAL "")
    target_sources(microide_perf PRIVATE src/render/SdlTtfTextBackend.cpp)
    target_compile_definitions(microide_perf PRIVATE MICROIDE_HAS_SDL3_TTF=1)
    target_link_libraries(microide_perf PRIVATE ${MICROIDE_SDL3_TTF_TARGET})
  endif()

  target_include_directories(microide_perf PRIVATE src tests)
  target_link_libraries(microide_perf PRIVATE
    ${MICROIDE_SDL3_TARGET}
    ${MICROIDE_PCRE2_TARGET}
  )
  if(NOT MICROIDE_LUA_TARGET STREQUAL "")
    target_link_libraries(microide_perf PRIVATE ${MICROIDE_LUA_TARGET})
  endif()
  target_compile_definitions(microide_perf PRIVATE
    MICROIDE_HAS_LUA_PLUGINS=$<BOOL:${MICROIDE_ENABLE_LUA_PLUGINS}>
    MICROIDE_TESTING=1
    MICROIDE_PERF_HARNESS_BUILD=1
  )

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(microide_perf PRIVATE -Wall -Wextra -Wpedantic)
    if(MICROIDE_WARNINGS_AS_ERRORS)
      target_compile_options(microide_perf PRIVATE -Werror)
    endif()
  elseif(MSVC)
    target_compile_options(microide_perf PRIVATE /W4)
    if(MICROIDE_WARNINGS_AS_ERRORS)
      target_compile_options(microide_perf PRIVATE /WX)
    endif()
  endif()

  add_test(NAME microide_perf_tests
           COMMAND microide_perf --smoke --iterations=1
           WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

add_executable(microide_diff_bench
  $<TARGET_OBJECTS:microide_runtime_syntax_generated>
  src/editor/BracketScanner.cpp
  src/editor/DecoratedTextGridRenderer.cpp
  src/editor/DiagnosticsRender.cpp
  src/editor/DiagnosticsStore.cpp
  src/editor/EditorViewRenderer.cpp
  src/editor/FoldingModel.cpp
  src/editor/IndentGuides.cpp
  src/editor/RuntimeSyntaxRegistry.cpp
  src/editor/SyntaxHighlighter.cpp
  src/editor/TextLayout.cpp
  src/editor/SaveNormalization.cpp
  src/editor/TextLayoutCache.cpp
  src/editor/TextViewport.cpp
  src/editor/TextViewportEditEngine.cpp
  src/editor/TextViewportFileIO.cpp
  src/editor/TextViewportHighlightCache.cpp
  src/editor/TextViewportLanguageBehavior.cpp
  src/editor/TextViewportMultiCaret.cpp
  src/editor/TextViewportUndoHistory.cpp
  src/editor/TextViewportViewState.cpp
  src/compare/CompareModel.cpp
  src/platform/RuntimePaths.cpp
  src/platform/Subprocess.cpp
  src/platform/Filesystem.cpp
  src/project/GitCompareService.cpp
  src/project/GitCommandUtil.cpp
  src/project/GitPorcelainParser.cpp
  src/project/GitPorcelainV2Parser.cpp
  src/project/GitRepository.cpp
  src/project/GitRepositoryState.cpp
  src/project/CommitWorkflowChecks.cpp
  src/project/GitCommitExecutor.cpp
  src/render/DebugTextBackend.cpp
  src/render/TextRenderer.cpp
  src/render/TextRendererBackend.cpp
  src/render/Theme.cpp
  src/util/PerformanceCounters.cpp
  src/util/PerformanceTrace.cpp
  src/util/Parse.cpp
  src/util/StartupTrace.cpp
  src/util/StringUtil.cpp
  src/util/TextFileIO.cpp
  tools/CompareBench.cpp
)

target_include_directories(microide_diff_bench PRIVATE src)
target_link_libraries(microide_diff_bench PRIVATE ${MICROIDE_SDL3_TARGET} ${MICROIDE_PCRE2_TARGET})

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(microide_diff_bench PRIVATE -Wall -Wextra -Wpedantic)
  if(MICROIDE_WARNINGS_AS_ERRORS)
    target_compile_options(microide_diff_bench PRIVATE -Werror)
  endif()
elseif(MSVC)
  target_compile_options(microide_diff_bench PRIVATE /W4)
  if(MICROIDE_WARNINGS_AS_ERRORS)
    target_compile_options(microide_diff_bench PRIVATE /WX)
  endif()
endif()

add_executable(microide_search_bench
  src/app/BackgroundTaskCounter.cpp
  src/util/PerformanceCounters.cpp
  src/util/StringUtil.cpp
  src/project/IgnoreMatcher.cpp
  src/project/ProjectFileScanner.cpp
  src/project/ProjectSearchService.cpp
  src/util/TaskExecutor.cpp
  tools/ProjectSearchBench.cpp
)

target_include_directories(microide_search_bench PRIVATE src)
target_link_libraries(microide_search_bench PRIVATE ${MICROIDE_SDL3_TARGET} ${MICROIDE_PCRE2_TARGET})

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(microide_search_bench PRIVATE -Wall -Wextra -Wpedantic)
  if(MICROIDE_WARNINGS_AS_ERRORS)
    target_compile_options(microide_search_bench PRIVATE -Werror)
  endif()
elseif(MSVC)
  target_compile_options(microide_search_bench PRIVATE /W4)
  if(MICROIDE_WARNINGS_AS_ERRORS)
    target_compile_options(microide_search_bench PRIVATE /WX)
  endif()
endif()
