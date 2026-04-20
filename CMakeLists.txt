cmake_minimum_required(VERSION 3.28)

project(microide VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

option(MICROIDE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(MICROIDE_ENABLE_SDL3_TTF "Enable SDL3_ttf text backend when available" ON)
option(MICROIDE_ENABLE_LUA_PLUGINS "Enable Lua-based manual plugins" ON)

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

if(NOT PkgConfig_FOUND)
  message(FATAL_ERROR
    "pkg-config is required to locate libpcre2-8 for runtime syntax highlighting.")
endif()

pkg_check_modules(PCRE2 REQUIRED IMPORTED_TARGET libpcre2-8)

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
  src/editor/DecoratedTextGridRenderer.cpp
  src/editor/DiagnosticsRender.cpp
  src/editor/DiagnosticsStore.cpp
  src/editor/EditorViewRenderer.cpp
  src/editor/RuntimeSyntaxRegistry.cpp
  src/editor/SyntaxDefinitionLoader.cpp
  src/editor/SyntaxHighlighter.cpp
  src/editor/TextLayout.cpp
  src/editor/TextViewport.cpp
  src/compare/CompareModel.cpp
  src/compare/MergeModel.cpp
  src/platform/AppDirectories.cpp
  src/platform/FileWatcher.cpp
  src/platform/Filesystem.cpp
  src/platform/Subprocess.cpp
  src/plugin/PluginHost.cpp
  src/project/DirectoryTree.cpp
  src/project/FileOperationService.cpp
  src/project/GitCompareService.cpp
  src/project/GitCommandUtil.cpp
  src/project/FileFinder.cpp
  src/project/FileIndex.cpp
  src/project/GitBlameService.cpp
  src/project/GitPorcelainParser.cpp
  src/project/GitRepository.cpp
  src/project/ProjectFileScanner.cpp
  src/project/GitStatusService.cpp
  src/project/IgnoreMatcher.cpp
  src/project/ProjectSearchService.cpp
  src/project/ProjectRoot.cpp
  src/render/DebugTextBackend.cpp
  src/render/TextRenderer.cpp
  src/render/TextRendererBackend.cpp
  src/render/Theme.cpp
  src/terminal/TerminalSession.cpp
  src/util/PerformanceTrace.cpp
  src/util/StartupTrace.cpp
  src/util/StringUtil.cpp
  src/util/TaskExecutor.cpp
  src/util/TextFileIO.cpp
  src/util/WindowPresentation.cpp
  src/workspace/WorkspaceShellCompare.cpp
  src/workspace/WorkspaceShellCompareRender.cpp
  src/workspace/WorkspaceChromeMouseCoordinator.cpp
  src/workspace/WorkspaceCompareInteractionCoordinator.cpp
  src/workspace/WorkspaceCompareMouseCoordinator.cpp
  src/workspace/WorkspaceActionCoordinator.cpp
  src/workspace/WorkspaceActionContext.cpp
  src/workspace/WorkspaceActionRequests.cpp
  src/workspace/WorkspaceCommandParsing.cpp
  src/workspace/WorkspaceCommandRegistry.cpp
  src/workspace/WorkspaceCommandPromptCoordinator.cpp
  src/workspace/WorkspaceEditActionExecutor.cpp
  src/workspace/WorkspaceEditorMouseCoordinator.cpp
  src/workspace/WorkspaceGlobalActionExecutor.cpp
  src/workspace/WorkspaceKeyInputCoordinator.cpp
  src/workspace/WorkspaceKeyInputCoordinatorModal.cpp
  src/workspace/WorkspaceKeyInputCoordinatorSurfaces.cpp
  src/workspace/WorkspaceKeyInputCoordinatorEditor.cpp
  src/workspace/WorkspaceMergeMouseCoordinator.cpp
  src/workspace/WorkspaceMenuCoordinator.cpp
  src/workspace/WorkspaceMenuRegistry.cpp
  src/workspace/WorkspacePanelMouseCoordinator.cpp
  src/workspace/WorkspaceProjectActionExecutor.cpp
  src/workspace/WorkspaceSearchActionExecutor.cpp
  src/workspace/WorkspaceSidebarActionExecutor.cpp
  src/workspace/WorkspaceTextInputCoordinator.cpp
  src/workspace/WorkspaceTabActionExecutor.cpp
  src/workspace/WorkspaceShellChrome.cpp
  src/workspace/WorkspaceShellBlame.cpp
  src/workspace/WorkspaceShellCursor.cpp
  src/workspace/WorkspaceDiffTabCoordinator.cpp
  src/workspace/WorkspaceDirtyPromptCoordinator.cpp
  src/workspace/WorkspaceShellEditor.cpp
  src/workspace/WorkspaceShellEditorSplits.cpp
  src/workspace/WorkspaceShellInput.cpp
  src/workspace/WorkspaceShellInteraction.cpp
  src/workspace/WorkspaceShellMenu.cpp
  src/workspace/WorkspaceShellMouse.cpp
  src/workspace/WorkspaceShellMouseMotion.cpp
  src/workspace/WorkspaceShellOverlay.cpp
  src/workspace/WorkspaceShellProjectSearch.cpp
  src/workspace/WorkspaceShellBufferSearch.cpp
  src/workspace/WorkspaceShellPresentation.cpp
  src/workspace/WorkspaceShellRedraw.cpp
  src/workspace/WorkspaceShellRenderFrame.cpp
  src/workspace/WorkspaceShellRenderSidebar.cpp
  src/workspace/WorkspaceShellRenderOverlay.cpp
  src/workspace/WorkspaceShellRenderBottomPanel.cpp
  src/workspace/WorkspaceShellRenderMenus.cpp
  src/workspace/WorkspaceShellRenderPrompts.cpp
  src/workspace/WorkspaceShellRenderChrome.cpp
  src/workspace/WorkspaceShellRenderTextInput.cpp
  src/workspace/WorkspaceShellHoverPopup.cpp
  src/workspace/WorkspaceShellHoverTargets.cpp
  src/workspace/WorkspacePersistenceCoordinator.cpp
  src/workspace/WorkspaceShellPlugins.cpp
  src/workspace/WorkspaceShellProjects.cpp
  src/workspace/WorkspacePathUtils.cpp
  src/workspace/WorkspacePathMutationCoordinator.cpp
  src/workspace/WorkspaceProjectSearchPresentation.cpp
  src/workspace/WorkspaceProjectPresentation.cpp
  src/workspace/WorkspaceProjectCatalogCoordinator.cpp
  src/workspace/WorkspacePathMutationCoordinatorDirty.cpp
  src/workspace/WorkspacePathMutationCoordinatorTabs.cpp
  src/workspace/WorkspaceLifecycleCoordinator.cpp
  src/workspace/WorkspaceLayout.cpp
  src/workspace/WorkspaceOutputChannels.cpp
  src/workspace/WorkspacePersistenceFormat.cpp
  src/workspace/WorkspacePluginAssetMonitor.cpp
  src/workspace/WorkspacePluginRuntime.cpp
  src/workspace/WorkspacePersistenceCoordinatorConfig.cpp
  src/workspace/WorkspacePersistenceCoordinatorSession.cpp
  src/workspace/WorkspacePersistenceCoordinatorWorkspaceSession.cpp
  src/workspace/WorkspaceProjectDialogCoordinator.cpp
  src/workspace/WorkspaceShellPrompts.cpp
  src/workspace/WorkspaceShellMerge.cpp
  src/workspace/WorkspaceShellMergeState.cpp
  src/workspace/WorkspaceShellMergeRender.cpp
  src/workspace/WorkspaceSidebarCoordinator.cpp
  src/workspace/WorkspaceSidebarCoordinatorRefresh.cpp
  src/workspace/WorkspaceSidebarCoordinatorActions.cpp
  src/workspace/WorkspaceSidebarMouseCoordinator.cpp
  src/workspace/WorkspaceSidebarMouseCoordinatorScroll.cpp
  src/workspace/WorkspaceTabMouseCoordinator.cpp
  src/workspace/WorkspaceTabCoordinator.cpp
  src/workspace/WorkspaceProjectSearchRuntime.cpp
  src/workspace/WorkspaceProjectStateCoordinator.cpp
  src/workspace/WorkspaceGitSidebarPresentation.cpp
  src/workspace/WorkspaceShellRender.cpp
  src/workspace/WorkspaceShellSidebar.cpp
  src/workspace/WorkspaceShellTerminal.cpp
  src/workspace/WorkspaceShellTerminalTabs.cpp
  src/workspace/WorkspaceSidebarRegistry.cpp
  src/workspace/WorkspaceTerminalSelection.cpp
  src/workspace/WorkspaceTextSearch.cpp
  src/workspace/WorkspaceShell.cpp
)

add_executable(microide
  src/app/main.cpp
  src/app/Application.cpp
  src/app/DirtyRegionPolicy.cpp
  $<TARGET_OBJECTS:microide_runtime_syntax_generated>
  ${MICROIDE_CORE_SOURCES}
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets")
  add_custom_command(TARGET microide POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:microide>/assets")
endif()

if(NOT MICROIDE_SDL3_TTF_TARGET STREQUAL "")
  target_sources(microide PRIVATE src/render/SdlTtfTextBackend.cpp)
  target_compile_definitions(microide PRIVATE MICROIDE_HAS_SDL3_TTF=1)
  target_link_libraries(microide PRIVATE ${MICROIDE_SDL3_TTF_TARGET})
endif()

target_include_directories(microide PRIVATE src)
target_link_libraries(microide PRIVATE ${MICROIDE_SDL3_TARGET} PkgConfig::PCRE2)
if(NOT MICROIDE_LUA_TARGET STREQUAL "")
  target_link_libraries(microide PRIVATE ${MICROIDE_LUA_TARGET})
endif()
target_compile_definitions(microide PRIVATE
  MICROIDE_HAS_LUA_PLUGINS=$<BOOL:${MICROIDE_ENABLE_LUA_PLUGINS}>
)

if(UNIX AND NOT APPLE)
  target_link_libraries(microide PRIVATE util)
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
  tests/AppDirectoriesTests.cpp
  tests/CompareModelTests.cpp
  tests/DiagnosticsStoreTests.cpp
  tests/DirtyRegionPolicyTests.cpp
  tests/FilesystemTests.cpp
  tests/FileOperationServiceTests.cpp
  tests/GitServiceTests.cpp
  tests/GitBlameServiceTests.cpp
  tests/MergeModelTests.cpp
  tests/ProjectSearchServiceTests.cpp
  tests/RegexUtilTests.cpp
  tests/TaskExecutorTests.cpp
  tests/TerminalSessionTests.cpp
  tests/TestMain.cpp
  tests/TestSupport.cpp
  tests/PluginHostTests.cpp
  tests/StringUtilTests.cpp
  tests/SubprocessTests.cpp
  tests/TextRendererTests.cpp
  tests/TextViewportTests.cpp
  tests/WindowPresentationTests.cpp
  tests/WorkspaceShellChromeTests.cpp
  tests/WorkspaceShellEditorBlameTests.cpp
  tests/WorkspaceShellPluginTests.cpp
  tests/WorkspaceShellPromptTests.cpp
  tests/WorkspaceShellCompareTests.cpp
  tests/WorkspaceShellProjectTests.cpp
  tests/WorkspaceShellSearchTests.cpp
  tests/WorkspaceShellSessionTests.cpp
  tests/WorkspaceShellSourceControlTests.cpp
  tests/WorkspaceShellSharedCoreTests.cpp
  tests/WorkspaceShellSharedLayoutTests.cpp
  tests/WorkspaceShellSharedSearchTests.cpp
  tests/WorkspaceShellSharedTerminalTests.cpp
  tests/WorkspaceShellTerminalTests.cpp
)

if(NOT MICROIDE_SDL3_TTF_TARGET STREQUAL "")
  target_sources(microide_tests PRIVATE src/render/SdlTtfTextBackend.cpp)
  target_compile_definitions(microide_tests PRIVATE MICROIDE_HAS_SDL3_TTF=1)
  target_link_libraries(microide_tests PRIVATE ${MICROIDE_SDL3_TTF_TARGET})
endif()

target_include_directories(microide_tests PRIVATE src)
target_link_libraries(microide_tests PRIVATE ${MICROIDE_SDL3_TARGET} PkgConfig::PCRE2)
if(NOT MICROIDE_LUA_TARGET STREQUAL "")
  target_link_libraries(microide_tests PRIVATE ${MICROIDE_LUA_TARGET})
endif()
target_compile_definitions(microide_tests PRIVATE
  MICROIDE_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests"
  MICROIDE_TESTING=1
  MICROIDE_HAS_LUA_PLUGINS=$<BOOL:${MICROIDE_ENABLE_LUA_PLUGINS}>
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

add_executable(microide_diff_bench
  $<TARGET_OBJECTS:microide_runtime_syntax_generated>
  src/editor/DecoratedTextGridRenderer.cpp
  src/editor/DiagnosticsRender.cpp
  src/editor/DiagnosticsStore.cpp
  src/editor/EditorViewRenderer.cpp
  src/editor/RuntimeSyntaxRegistry.cpp
  src/editor/SyntaxHighlighter.cpp
  src/editor/TextLayout.cpp
  src/editor/TextViewport.cpp
  src/compare/CompareModel.cpp
  src/platform/Subprocess.cpp
  src/platform/Filesystem.cpp
  src/project/GitCompareService.cpp
  src/project/GitCommandUtil.cpp
  src/project/GitPorcelainParser.cpp
  src/project/GitRepository.cpp
  src/render/DebugTextBackend.cpp
  src/render/TextRenderer.cpp
  src/render/TextRendererBackend.cpp
  src/render/Theme.cpp
  src/util/PerformanceTrace.cpp
  src/util/StartupTrace.cpp
  src/util/StringUtil.cpp
  src/util/TextFileIO.cpp
  tools/CompareBench.cpp
)

target_include_directories(microide_diff_bench PRIVATE src)
target_link_libraries(microide_diff_bench PRIVATE ${MICROIDE_SDL3_TARGET} PkgConfig::PCRE2)

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
  src/project/IgnoreMatcher.cpp
  src/project/ProjectFileScanner.cpp
  src/project/ProjectSearchService.cpp
  src/util/TaskExecutor.cpp
  tools/ProjectSearchBench.cpp
)

target_include_directories(microide_search_bench PRIVATE src)
target_link_libraries(microide_search_bench PRIVATE ${MICROIDE_SDL3_TARGET} PkgConfig::PCRE2)

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
