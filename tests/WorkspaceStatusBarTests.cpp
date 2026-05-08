#include "TestSupport.h"

#include "workspace/RenderViewModelBuilder.h"
#include "workspace/StatusBarService.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceLayout.h"

#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::ComputeLayout;
using microide::workspace::LayoutMode;
using microide::workspace::LayoutModeInputs;
using microide::workspace::RenderViewModelBuilder;
using microide::workspace::StatusBarSegmentId;
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
  SetSegment(service, StatusBarSegmentId::LayoutMode, "compact");

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
  Expect(!vm.right_segments.empty() && vm.right_segments.back().text != "compact",
         "compact status bar should drop the layout-mode badge before essential right segments");
}

}  // namespace

void RegisterWorkspaceStatusBarTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceStatusBar/BuildsVisibleSegments",
          TestStatusBarBuildsVisibleSegments);
  AddTest(tests, "WorkspaceStatusBar/NoOpsWhenNotReserved",
          TestStatusBarNoOpsWhenNotReserved);
  AddTest(tests, "WorkspaceStatusBar/CompactDropOrder",
          TestStatusBarCompactDropOrder);
}

}  // namespace microide::tests
