#include "TestSupport.h"

#include "editor/TextViewport.h"
#include "workspace/RenderViewModelBuilder.h"
#include "workspace/StatusBarModelService.h"
#include "workspace/StatusBarService.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceLayout.h"

#include <algorithm>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::ComputeLayout;
using microide::workspace::LayoutMode;
using microide::workspace::LayoutModeInputs;
using microide::workspace::RenderViewModelBuilder;
using microide::workspace::StatusBarSegmentId;
using microide::workspace::StatusBarSegmentTone;
using microide::workspace::StatusBarSegmentValue;
using microide::workspace::StatusBarService;
using microide::workspace::WorkspaceContext;

void SetSegment(StatusBarService& service,
                StatusBarSegmentId id,
                std::string text,
                bool clickable = false) {
  service.SetSegment(id, StatusBarSegmentValue{
                             .text = std::move(text),
                             .tooltip = {},
                             .clickable = clickable,
                             .visible = true,
                         });
}

void TestStatusBarBuildsVisibleSegments() {
  WorkspaceContext context;
  StatusBarService service;
  SetSegment(service, StatusBarSegmentId::Project, "microide", true);
  SetSegment(service, StatusBarSegmentId::Branch, "main");
  SetSegment(service, StatusBarSegmentId::LineColumn, "Ln 4, Col 2");
  SetSegment(service, StatusBarSegmentId::Lsp, "LSP: Ready", true);

  const auto layout = ComputeLayout(1280.0f, 720.0f, true, true, 280.0f, 160.0f,
                                    LayoutModeInputs{}, true);
  const auto vm = RenderViewModelBuilder(context).BuildStatusBar(layout, service);

  Expect(vm.visible, "status bar view model should be visible when layout reserves the strip");
  Expect(vm.rect.h > 0.0f, "status bar view model should retain the reserved rect");
  Expect(vm.left_segments.size() == 2,
         "status bar should expose project and branch on the left");
  Expect(vm.right_segments.size() == 2,
         "status bar should expose line/column and LSP on the right");
  Expect(vm.left_segments[0].text == "microide" && vm.left_segments[0].clickable,
         "project segment should preserve text and clickability");
  Expect(vm.left_segments[0].id == StatusBarSegmentId::Project,
         "status bar view model should preserve segment ids for click routing");
  Expect(vm.right_segments[1].text == "LSP: Ready" && vm.right_segments[1].clickable,
         "right-side status segments should preserve clickability");
  Expect(vm.right_segments[1].id == StatusBarSegmentId::Lsp,
         "right-side status segments should preserve ids for action dispatch");
}

void TestStatusBarNoOpsWhenNotReserved() {
  WorkspaceContext context;
  StatusBarService service;
  SetSegment(service, StatusBarSegmentId::Project, "microide");

  const auto layout = ComputeLayout(1280.0f, 720.0f, true, true, 280.0f, 160.0f,
                                    LayoutModeInputs{}, false);
  const auto vm = RenderViewModelBuilder(context).BuildStatusBar(layout, service);

  Expect(!vm.visible, "status bar should be hidden when the layout does not reserve it");
  Expect(vm.left_segments.empty() && vm.right_segments.empty(),
         "hidden status bar view model should not carry stale segments");
}

void TestStatusBarCompactDropOrder() {
  WorkspaceContext context;
  StatusBarService service;
  SetSegment(service, StatusBarSegmentId::Project, "microide");
  SetSegment(service, StatusBarSegmentId::Branch, "main");
  SetSegment(service, StatusBarSegmentId::Language, "C++");
  SetSegment(service, StatusBarSegmentId::Indent, "Spaces: 2");
  SetSegment(service, StatusBarSegmentId::Encoding, "UTF-8");
  SetSegment(service, StatusBarSegmentId::LineColumn, "Ln 4, Col 2");
  SetSegment(service, StatusBarSegmentId::Problems, "0 problems");
  SetSegment(service, StatusBarSegmentId::Lsp, "LSP: Ready");

  LayoutModeInputs inputs;
  inputs.user_override = LayoutModeInputs::Override::Compact;
  const auto layout = ComputeLayout(640.0f, 480.0f, true, false, 240.0f, 0.0f,
                                    inputs, true);
  const auto vm = RenderViewModelBuilder(context).BuildStatusBar(layout, service);

  Expect(vm.layout_mode == LayoutMode::Compact,
         "compact status-bar fixture should resolve compact layout mode");
  Expect(vm.left_segments.size() == 2,
         "compact status bar should keep project and branch and drop lower-priority left segments");
  Expect(vm.left_segments[0].text == "microide" && vm.left_segments[1].text == "main",
         "compact status bar should preserve project and branch order");
  const auto find_segment = [&](StatusBarSegmentId id,
                                  const std::vector<microide::workspace::StatusBarSegmentViewModel>&
                                      segments) {
    return std::any_of(segments.begin(), segments.end(),
                       [&](const auto& seg) { return seg.id == id; });
  };
  Expect(!find_segment(StatusBarSegmentId::Encoding, vm.left_segments),
         "compact mode should drop the Encoding segment");
  Expect(!find_segment(StatusBarSegmentId::Language, vm.left_segments),
         "compact mode should drop the Language segment");
  Expect(!find_segment(StatusBarSegmentId::Indent, vm.left_segments),
         "compact mode should drop the Indent segment");
  Expect(find_segment(StatusBarSegmentId::Problems, vm.right_segments),
         "compact mode must keep Problems visible per spec");
  Expect(find_segment(StatusBarSegmentId::Lsp, vm.right_segments),
         "compact mode must keep LSP visible per spec");
  Expect(find_segment(StatusBarSegmentId::LineColumn, vm.right_segments),
         "compact mode must keep LineColumn visible per spec");
}

void TestStatusBarPropagatesSemanticTone() {
  // The render path colors diagnostic segments from `tone`, not from the
  // display text, so the builder must carry tone through verbatim. This guards
  // against regressing to text-scanning (e.g. `text.find("0")`), which
  // mis-classifies counts like "10 errors, 0 warnings".
  WorkspaceContext context;
  StatusBarService service;
  service.SetSegment(StatusBarSegmentId::Problems,
                     StatusBarSegmentValue{
                         .text = "10 errors, 0 warnings",
                         .tooltip = {},
                         .clickable = true,
                         .visible = true,
                         .tone = StatusBarSegmentTone::Error,
                     });

  const auto layout = ComputeLayout(1280.0f, 720.0f, true, true, 280.0f, 160.0f,
                                    LayoutModeInputs{}, true);
  const auto vm = RenderViewModelBuilder(context).BuildStatusBar(layout, service);

  Expect(vm.right_segments.size() == 1, "only the Problems segment should be present");
  Expect(vm.right_segments[0].id == StatusBarSegmentId::Problems,
         "the single segment should be Problems");
  Expect(vm.right_segments[0].tone == StatusBarSegmentTone::Error,
         "builder must propagate the semantic tone so the render path never re-parses text");
}

void TestStatusBarLspToneFromTypedSeverityNotLabelText() {
  // Regression: the Lsp segment tone must come from the operation's typed
  // severity, never from substring-scanning the label. A server whose label
  // literally contains "Ready" but is in a failed state must render Error.
  WorkspaceContext context;
  StatusBarService service;
  editor::TextViewport viewport;  // non-null active viewport gates the Lsp segment

  microide::workspace::StatusBarModelService model;
  microide::workspace::StatusBarModelService::Operations ops;
  ops.is_git_repo_valid = [](const std::filesystem::path&) { return false; };
  ops.active_lsp_status_strings = [](bool, std::string& text, std::string& tooltip,
                                     StatusBarSegmentTone& tone) {
    text = "LSP: clangd Not Ready";  // contains "Ready" as a substring
    tooltip = "language server failed to start";
    tone = StatusBarSegmentTone::Error;
  };

  model.Refresh(service, ops, context.current_project_state, &viewport);

  const auto& lsp = service.Segment(StatusBarSegmentId::Lsp);
  Expect(lsp.visible, "the Lsp segment should be visible with a non-empty status");
  Expect(lsp.text == "LSP: clangd Not Ready", "the label text should pass through verbatim");
  Expect(lsp.tone == StatusBarSegmentTone::Error,
         "tone must follow the typed severity (Error), not a 'Ready' substring match");
}

void TestStatusBarRepoAvailabilityReflectsInSessionGitInit() {
  // The repo-availability probe must not be cached by project_root alone: an
  // in-session `git init` (or `.git` removal) must be reflected on the next
  // refresh, not stay stale until the project root changes.
  WorkspaceContext context;
  context.current_project_state.root = "/tmp/statusbar-git-init-probe";
  StatusBarService service;

  microide::workspace::StatusBarModelService model;
  microide::workspace::StatusBarModelService::Operations ops;
  bool repo_valid = false;
  ops.is_git_repo_valid = [&](const std::filesystem::path&) { return repo_valid; };
  ops.active_lsp_status_strings = [](bool, std::string&, std::string&, StatusBarSegmentTone&) {};

  model.Refresh(service, ops, context.current_project_state, nullptr);
  Expect(service.Segment(StatusBarSegmentId::Project).text == "no-scm",
         "a non-repo project must show no-scm");

  // Simulate `git init`: the same project root now reports a valid repo.
  repo_valid = true;
  model.Refresh(service, ops, context.current_project_state, nullptr);
  Expect(service.Segment(StatusBarSegmentId::Project).text.find("[clean]") != std::string::npos,
         "an in-session git init must be reflected without a stale project_root cache");
}

}  // namespace

void RegisterWorkspaceStatusBarTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceStatusBar/RepoAvailabilityReflectsInSessionGitInit",
          TestStatusBarRepoAvailabilityReflectsInSessionGitInit);
  AddTest(tests, "WorkspaceStatusBar/LspToneFromTypedSeverityNotLabelText",
          TestStatusBarLspToneFromTypedSeverityNotLabelText);
  AddTest(tests, "WorkspaceStatusBar/BuildsVisibleSegments",
          TestStatusBarBuildsVisibleSegments);
  AddTest(tests, "WorkspaceStatusBar/NoOpsWhenNotReserved",
          TestStatusBarNoOpsWhenNotReserved);
  AddTest(tests, "WorkspaceStatusBar/CompactDropOrder",
          TestStatusBarCompactDropOrder);
  AddTest(tests, "WorkspaceStatusBar/PropagatesSemanticTone",
          TestStatusBarPropagatesSemanticTone);
}

}  // namespace microide::tests
