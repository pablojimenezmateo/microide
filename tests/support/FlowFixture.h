#pragma once

// A flow test's scene: a project with one file open in a 1280x720 window, its
// first frame painted. Click() drives a control the `ui` query names, and
// ExpectFresh() paints an event's damage as the app loop would and expects the
// result to equal a full repaint (support/RetainedRedraw.h).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "TestSupport.h"
#include "TestSupportSdl.h"
#include "WorkspaceShellEventHelpers.h"
#include "support/ProjectSearchWait.h"
#include "support/RetainedRedraw.h"
#include "support/SoftwareCanvas.h"
#include "util/JsonValue.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

namespace microide::tests {

struct FlowFixture {
  static constexpr int kWidth = 1280;
  static constexpr int kHeight = 720;

  explicit FlowFixture(std::string_view file_name, std::string_view content)
      : FlowFixture(file_name, content, kWidth, kHeight) {}

  // `before_open` runs on the project's directory, its file written, before the
  // project opens (a test that needs a git repository makes one there).
  FlowFixture(std::string_view file_name, std::string_view content,
              const std::function<void(const std::filesystem::path&)>& before_open)
      : FlowFixture(file_name, content, kWidth, kHeight, before_open) {}

  // A window of another size, as a monkey cell had it.
  FlowFixture(std::string_view file_name, std::string_view content, int window_width, int window_height,
              const std::function<void(const std::filesystem::path&)>& before_open = {})
      : width(window_width), height(window_height), canvas{window_width, window_height} {
    EnsureDummySdlVideoInitialized();
    root = temp_dir.path() / "project";
    file = root / std::string(file_name);
    WriteFile(file, std::string(content));
    if (before_open) {
      before_open(root);
    }
    Expect(workspace::WorkspaceShell::TestAccess::OpenProjectTab(shell, root, false, false), "the flow fixture should open its project");
    // The initial index scan runs in the background, and a project search reads its
    // candidates from the index: a flow that searches before it lands searched no
    // files (QaFlowRegression/SearchResultsCaughtUpAtFramePrepRepaintTheirCount, on
    // a loaded machine). Start every flow from an indexed project.
    Expect(WaitForFileIndexSize(shell, 1, std::chrono::seconds(10)),
           "the flow fixture's project should be indexed");
    workspace::WorkspaceShell::TestAccess::SetWindowSize(shell, width, height);
    workspace::WorkspaceShell::TestAccess::OpenSingleEditorTab(shell, file);
    workspace::WorkspaceShell::TestAccess::ShowTreeSidebar(shell);
    workspace::WorkspaceShell::TestAccess::SetFocusEditor(shell);
    workspace::WorkspaceShell::TestAccess::NoteLayoutInputsChanged(shell);
    PinCaret();
    shell.Render(canvas.renderer(), width, height);
    (void)shell.ConsumePendingRenderInvalidation();
  }

  void PinCaret() { workspace::WorkspaceShell::TestAccess::SetCaretBlinkEpochMs(shell, SDL_GetTicks()); }

  // The element the ui query names `name`, or nullopt.
  std::optional<SDL_FRect> ElementRect(std::string_view name) {
    const util::JsonValue ui = workspace::WorkspaceShell::TestAccess::ControlUi(shell);
    for (const util::JsonValue& entry : ui["elements"].AsArray()) {
      if (entry["name"].AsString() == name) {
        const auto& r = entry["rect"].AsArray();
        return SDL_FRect{static_cast<float>(r[0].AsInt()), static_cast<float>(r[1].AsInt()),
                         static_cast<float>(r[2].AsInt()), static_cast<float>(r[3].AsInt())};
      }
    }
    return std::nullopt;
  }

  workspace::RenderInvalidation Click(const SDL_FRect& rect) {
    const float x = rect.x + rect.w / 2.0f;
    const float y = rect.y + rect.h / 2.0f;
    return DispatchEvents(shell, {MouseMotionEvent(x, y),
                                  MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y),
                                  MouseButtonEvent(SDL_EVENT_MOUSE_BUTTON_UP, x, y)});
  }

  // Paints `redraw` as the app would and expects it to equal a full repaint.
  void ExpectFresh(const workspace::RenderInvalidation& redraw, const std::string& what) {
    if (const char* dump = std::getenv("MQA_TEST_DUMP"); dump != nullptr) {
      for (const auto& r : redraw.rects) {
        std::fprintf(stderr, "%s: rect %.0f,%.0f %.0fx%.0f\n", what.c_str(), r.x, r.y, r.w, r.h);
      }
    }
    const auto diff = PaintAndDiffWithFullRepaint(shell, canvas, width, height, redraw,
                                                  [this]() { PinCaret(); });
    Expect(diff.identical(), what + " should paint what a full repaint paints; " +
                                 DescribePixelDiff(diff));
  }

  int width = kWidth;
  int height = kHeight;
  TemporaryDirectory temp_dir;
  std::filesystem::path root;
  std::filesystem::path file;
  workspace::WorkspaceShell shell;
  SoftwareCanvas canvas;
};

}  // namespace microide::tests
