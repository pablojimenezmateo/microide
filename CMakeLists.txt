cmake_minimum_required(VERSION 3.28)

project(microide VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

option(MICROIDE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(MICROIDE_ENABLE_SDL3_TTF "Enable SDL3_ttf text backend when available" ON)

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

add_executable(microide
  src/app/main.cpp
  src/app/Application.cpp
  src/editor/EditorViewRenderer.cpp
  src/editor/RuntimeSyntaxGenerated.cpp
  src/editor/RuntimeSyntaxRegistry.cpp
  src/editor/SyntaxHighlighter.cpp
  src/editor/TextLayout.cpp
  src/editor/TextViewport.cpp
  src/compare/CompareModel.cpp
  src/project/DirectoryTree.cpp
  src/project/GitCompareService.cpp
  src/project/FileFinder.cpp
  src/project/FileIndex.cpp
  src/project/GitStatusService.cpp
  src/project/IgnoreMatcher.cpp
  src/project/ProjectSearchService.cpp
  src/project/ProjectRoot.cpp
  src/render/DebugTextBackend.cpp
  src/render/TextRenderer.cpp
  src/render/TextRendererBackend.cpp
  src/render/Theme.cpp
  src/terminal/TerminalSession.cpp
  src/workspace/WorkspaceShell.cpp
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
  src/compare/CompareModel.cpp
  src/project/GitCompareService.cpp
  tests/TestMain.cpp
)

target_include_directories(microide_tests PRIVATE src)
target_compile_definitions(microide_tests PRIVATE
  MICROIDE_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests"
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
