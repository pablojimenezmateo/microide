#include "TestSupport.h"

#include "support/SoftwareCanvas.h"

#include "workspace/shell/WorkspaceShellTestAccess.h"
#include "workspace/state/WorkspacePromptState.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using WorkspaceShellTestAccess = microide::workspace::WorkspaceShell::TestAccess;
using microide::workspace::DirtyPromptState;
using microide::workspace::PromptSurfaceState;
using microide::workspace::WorkspaceShell;

constexpr int kWindowWidth = 1100;
constexpr int kWindowHeight = 720;

std::filesystem::path MakeProject(const std::filesystem::path& root) {
  std::filesystem::create_directories(root / "deeply" / "nested" / "sub");
  const std::filesystem::path source = root / "deeply" / "nested" / "sub" / "main.cpp";
  WriteFile(source, "int main() { return 0; }\n");
  return source;
}

void OpenShell(WorkspaceShell& shell, const std::filesystem::path& root,
               const std::filesystem::path& source) {
  WorkspaceShellTestAccess::SetProjectRoot(shell, root);
  WorkspaceShellTestAccess::SetWindowSize(shell, kWindowWidth, kWindowHeight);
  WorkspaceShellTestAccess::OpenSingleEditorTab(shell, source);
}

void PaintPromptSurface(WorkspaceShell& shell, SoftwareCanvas& canvas) {
  Expect(WorkspaceShellTestAccess::PromptSurfaceVisible(shell),
         "the prompt surface must be visible for its paint to be exercised");
  WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
  const SDL_Color center = canvas.PixelAt(kWindowWidth / 2, kWindowHeight / 2);
  Expect(center.a == 255, "a painted prompt frame should be fully opaque");
}

void TestPromptSurfacePaintsEveryAction() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = MakeProject(root);
  SoftwareCanvas canvas(kWindowWidth, kWindowHeight);

  WorkspaceShell shell;
  OpenShell(shell, root, source);

  using Action = PromptSurfaceState::Action;
  using Kind = PromptSurfaceState::Kind;
  const struct {
    Action action;
    Kind kind;
  } cases[] = {
      {Action::SaveAs, Kind::TextInput},
      {Action::CreateFile, Kind::TextInput},
      {Action::CreateDirectory, Kind::TextInput},
      {Action::RenamePath, Kind::TextInput},
      {Action::DeletePath, Kind::Confirm},
      {Action::DiscardGitChanges, Kind::Confirm},
      {Action::DiscardGitEntry, Kind::Confirm},
      {Action::DiscardPatchPreview, Kind::Confirm},
      {Action::SetGitOutgoingBaseRef, Kind::TextInput},
      {Action::OpenExternalUrl, Kind::Confirm},
      {Action::ConfirmCommitAmend, Kind::Confirm},
      {Action::ConfirmCommitNoVerify, Kind::Confirm},
      {Action::ConfirmCommitWarnings, Kind::Confirm},
      {Action::SetBreakpointCondition, Kind::TextInput},
      {Action::SetBreakpointHitCondition, Kind::TextInput},
      {Action::SetBreakpointLogMessage, Kind::TextInput},
      {Action::AddWatchExpression, Kind::TextInput},
      {Action::EditWatchExpression, Kind::TextInput},
      {Action::EvaluateReplInput, Kind::TextInput},
      {Action::GoToLine, Kind::TextInput},
      {Action::RenameSymbol, Kind::TextInput},
      {Action::ConfirmRenameSave, Kind::Confirm},
  };
  const std::string long_detail =
      "https://example.invalid/a/very/long/link/that/will/not/fit/in/the/dialog/width";

  for (const auto& entry : cases) {
    WorkspaceShellTestAccess::OpenPromptSurfaceForTest(shell, entry.action, entry.kind, source,
                                                       long_detail, "typed input text");
    PaintPromptSurface(shell, canvas);

    WorkspaceShellTestAccess::SetPromptSurfaceSelectedButton(shell, 1);
    PaintPromptSurface(shell, canvas);

    WorkspaceShellTestAccess::OpenPromptSurfaceForTest(shell, entry.action, entry.kind, source,
                                                       std::string{}, std::string{});
    PaintPromptSurface(shell, canvas);
  }
}

void TestPromptSurfacePaintsWithMoreButtonsThanLabels() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = MakeProject(root);
  SoftwareCanvas canvas(kWindowWidth, kWindowHeight);

  WorkspaceShell shell;
  OpenShell(shell, root, source);

  WorkspaceShellTestAccess::OpenPromptSurfaceForTest(
      shell, PromptSurfaceState::Action::DeletePath, PromptSurfaceState::Kind::Confirm, source,
      std::string{});
  bool saw_more_rects_than_labels = false;
  for (const int count : {1, 2, 3, 7}) {
    WorkspaceShellTestAccess::SetPromptSurfaceButtonCount(shell, count);
    const std::size_t rects = WorkspaceShellTestAccess::PromptSurfaceButtonRectCount(shell);
    const std::size_t labels = WorkspaceShellTestAccess::PromptSurfaceLabelCount(shell);
    Expect(rects == static_cast<std::size_t>(std::max(1, count)),
           "the rect count follows button_count, clamped only from below");
    Expect(labels == 2, "the action labels are a fixed pair whatever button_count says");
    if (rects > labels) {
      saw_more_rects_than_labels = true;
    }
    PaintPromptSurface(shell, canvas);
  }
  Expect(saw_more_rects_than_labels,
         "the mismatch this bound exists for must actually occur in the sweep");
}

void TestDirtyPromptPaintsEveryKind() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = MakeProject(root);
  SoftwareCanvas canvas(kWindowWidth, kWindowHeight);

  WorkspaceShell shell;
  OpenShell(shell, root, source);

  const DirtyPromptState::Kind kinds[] = {
      DirtyPromptState::Kind::CloseTab,     DirtyPromptState::Kind::CloseTabs,
      DirtyPromptState::Kind::CloseProject, DirtyPromptState::Kind::Quit,
      DirtyPromptState::Kind::RenamePath,   DirtyPromptState::Kind::DeletePath,
  };
  for (const DirtyPromptState::Kind kind : kinds) {
    for (const std::size_t dirty_count : {std::size_t{1}, std::size_t{7}}) {
      WorkspaceShellTestAccess::ShowDirtyPromptForTest(shell, kind, dirty_count);
      Expect(WorkspaceShellTestAccess::DirtyPromptVisible(shell),
             "the dirty prompt must be visible for its paint to be exercised");
      WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
      for (const int selected : {0, 1, 2}) {
        WorkspaceShellTestAccess::SetDirtyPromptSelectedAction(shell, selected);
        WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
      }
      const SDL_Color center = canvas.PixelAt(kWindowWidth / 2, kWindowHeight / 2);
      Expect(center.a == 255, "a painted dirty prompt should be fully opaque");
    }
  }
}

void TestPromptsPaintAtDegenerateWindowSizes() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  const std::filesystem::path source = MakeProject(root);

  for (const int width : {40, 120, 360}) {
    for (const int height : {40, 100}) {
      SoftwareCanvas canvas(width, height);
      WorkspaceShell shell;
      OpenShell(shell, root, source);
      WorkspaceShellTestAccess::SetWindowSize(shell, width, height);

      WorkspaceShellTestAccess::OpenPromptSurfaceForTest(
          shell, PromptSurfaceState::Action::OpenExternalUrl,
          PromptSurfaceState::Kind::Confirm, source,
          "https://example.invalid/long/enough/to/need/truncating");
      WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());

      WorkspaceShellTestAccess::OpenPromptSurfaceForTest(
          shell, PromptSurfaceState::Action::SaveAs, PromptSurfaceState::Kind::TextInput,
          source, std::string{}, "a typed path that is much wider than this window");
      WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());

      WorkspaceShellTestAccess::ShowDirtyPromptForTest(shell, DirtyPromptState::Kind::Quit, 3);
      WorkspaceShellTestAccess::RenderFrameWithRenderer(shell, canvas.renderer());
    }
  }
}

}

void RegisterWorkspaceShellRenderPromptsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShellRenderPrompts/PaintsEveryAction",
          TestPromptSurfacePaintsEveryAction);
  AddTest(tests, "WorkspaceShellRenderPrompts/PaintsWithMoreButtonsThanLabels",
          TestPromptSurfacePaintsWithMoreButtonsThanLabels);
  AddTest(tests, "WorkspaceShellRenderPrompts/DirtyPromptPaintsEveryKind",
          TestDirtyPromptPaintsEveryKind);
  AddTest(tests, "WorkspaceShellRenderPrompts/PaintsAtDegenerateWindowSizes",
          TestPromptsPaintAtDegenerateWindowSizes);
}

}
