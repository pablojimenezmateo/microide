#include "TestSupport.h"

#include "editor/TextViewport.h"
#include "workspace/render/RenderViewModelBuilder.h"
#include "workspace/services/StatusBarModelService.h"
#include "workspace/services/StatusBarService.h"
#include "workspace/WorkspaceContext.h"
#include "perf/AllocationCounter.h"
#include "workspace/WorkspaceLayout.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::ComputeLayout;
using microide::workspace::LayoutMode;
using microide::workspace::LayoutModeInputs;
using microide::workspace::RenderViewModelBuilder;
using microide::workspace::StatusBarSegmentId;
using microide::workspace::StatusTone;
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
                             .command = clickable ? "sidebar-show" : "",
                             .visible = true,
                         });
}

void TestStatusBarSetSegmentReportsPaintedChanges() {
  StatusBarService service;
  const auto value = [](std::string text, std::string tooltip) {
    return StatusBarSegmentValue{.text = std::move(text), .tooltip = std::move(tooltip), .visible = true};
  };
  Expect(service.SetSegment(StatusBarSegmentId::Remote, value("worky", "a")), "new text paints");
  Expect(!service.SetSegment(StatusBarSegmentId::Remote, value("worky", "b")),
         "a tooltip-only change paints nothing");
  Expect(service.SetSegment(StatusBarSegmentId::Remote, value("worky · 1 port", "b")), "new text paints");
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

struct FixedAdvanceMeasure {
  float MeasureWidth(std::string_view text) const { return 8.0f * static_cast<float>(text.size()); }
};

void TestStatusBarSegmentsNeverOverlapOnANarrowBar() {
  microide::workspace::StatusBarViewModel vm;
  vm.visible = true;
  vm.rect = SDL_FRect{0.0f, 140.0f, 213.0f, 20.0f};
  const auto seg = [](StatusBarSegmentId id, std::string_view text) {
    microide::workspace::StatusBarSegmentViewModel s;
    s.id = id;
    s.text = text;
    s.clickable = true;
    return s;
  };
  vm.left_segments.push_back(seg(StatusBarSegmentId::Project, "taskflow"));
  vm.left_segments.push_back(seg(StatusBarSegmentId::Branch, "main"));
  vm.left_segments.push_back(seg(StatusBarSegmentId::Language, "Markdown"));
  vm.right_segments.push_back(seg(StatusBarSegmentId::LineColumn, "Ln 1, Col 1"));
  vm.right_segments.push_back(seg(StatusBarSegmentId::Lsp, "LSP: Idle"));

  std::vector<std::pair<StatusBarSegmentId, SDL_FRect>> visited;
  microide::workspace::ForEachStatusBarSegmentRect(
      vm, FixedAdvanceMeasure{},
      [&](const microide::workspace::StatusBarSegmentViewModel& segment, const SDL_FRect& rect) {
        visited.emplace_back(segment.id, rect);
      });

  for (std::size_t i = 0; i < visited.size(); ++i) {
    const SDL_FRect& a = visited[i].second;
    Expect(a.x >= vm.rect.x && a.x + a.w <= vm.rect.x + vm.rect.w,
           "every visited status segment must lie inside the bar");
    for (std::size_t j = i + 1; j < visited.size(); ++j) {
      const SDL_FRect& b = visited[j].second;
      Expect(a.x + a.w <= b.x || b.x + b.w <= a.x,
             "two status segments must never be laid over each other");
    }
  }
  const auto has = [&](StatusBarSegmentId id) {
    return std::any_of(visited.begin(), visited.end(),
                       [&](const auto& entry) { return entry.first == id; });
  };
  Expect(has(StatusBarSegmentId::LineColumn) && has(StatusBarSegmentId::Lsp),
         "the right cluster keeps priority when the bar is too narrow for everything");
  Expect(!has(StatusBarSegmentId::Language),
         "a left segment with no room left is dropped, not drawn under the right cluster");

  vm.rect.w = 1000.0f;
  visited.clear();
  microide::workspace::ForEachStatusBarSegmentRect(
      vm, FixedAdvanceMeasure{},
      [&](const microide::workspace::StatusBarSegmentViewModel& segment, const SDL_FRect& rect) {
        visited.emplace_back(segment.id, rect);
      });
  Expect(visited.size() == 5, "a wide bar must lay out every segment");
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
                                  const microide::workspace::StatusBarSegmentList& segments) {
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
  WorkspaceContext context;
  StatusBarService service;
  service.SetSegment(StatusBarSegmentId::Problems,
                     StatusBarSegmentValue{
                         .text = "10 errors, 0 warnings",
                         .tooltip = {},
                         .command = "sidebar-show",
                         .visible = true,
                         .tone = StatusTone::Error,
                     });

  const auto layout = ComputeLayout(1280.0f, 720.0f, true, true, 280.0f, 160.0f,
                                    LayoutModeInputs{}, true);
  const auto vm = RenderViewModelBuilder(context).BuildStatusBar(layout, service);

  Expect(vm.right_segments.size() == 1, "only the Problems segment should be present");
  Expect(vm.right_segments[0].id == StatusBarSegmentId::Problems,
         "the single segment should be Problems");
  Expect(vm.right_segments[0].tone == StatusTone::Error,
         "builder must propagate the semantic tone so the render path never re-parses text");
}

void TestStatusBarLspToneFromTypedSeverityNotLabelText() {
  WorkspaceContext context;
  StatusBarService service;
  editor::TextViewport viewport;

  microide::workspace::StatusBarModelService model;
  microide::workspace::StatusBarModelService::Operations ops;
  ops.git_availability = [](const std::filesystem::path&) {
    return microide::project::GitAvailability::NotARepository;
  };
  ops.active_lsp_status_strings = [](bool, std::string& text, std::string& tooltip,
                                     StatusTone& tone) {
    text = "LSP: clangd Not Ready";
    tooltip = "language server failed to start";
    tone = StatusTone::Error;
  };

  model.Refresh(service, ops, context.current_project_state, &viewport);

  const auto& lsp = service.Segment(StatusBarSegmentId::Lsp);
  Expect(lsp.visible, "the Lsp segment should be visible with a non-empty status");
  Expect(lsp.text == "LSP: clangd Not Ready", "the label text should pass through verbatim");
  Expect(lsp.tone == StatusTone::Error,
         "tone must follow the typed severity (Error), not a 'Ready' substring match");
}

void TestStatusBarSteadyRefreshDoesNotAllocate() {
  WorkspaceContext context;
  StatusBarService service;
  editor::TextViewport viewport;

  microide::workspace::StatusBarModelService model;
  microide::workspace::StatusBarModelService::Operations ops;
  ops.git_availability = [](const std::filesystem::path&) {
    return microide::project::GitAvailability::NotARepository;
  };
  ops.active_lsp_status_strings = [](bool, std::string& text, std::string& tooltip,
                                     StatusTone& tone) {
    text = "LSP: clangd indexing the workspace";
    tooltip = "language server is indexing the workspace, some results may be partial";
    tone = StatusTone::Info;
  };

  model.Refresh(service, ops, context.current_project_state, &viewport);
  model.Refresh(service, ops, context.current_project_state, &viewport);
#if MICROIDE_PERF_HARNESS_BUILD
  const perf::AllocationSnapshot before = perf::Allocations::Snapshot();
#endif
  model.Refresh(service, ops, context.current_project_state, &viewport);
#if MICROIDE_PERF_HARNESS_BUILD
  const perf::AllocationDelta delta = perf::Allocations::DeltaSince(before);
  Expect(delta.allocations == 0,
         "a status-bar refresh that changes nothing should not allocate");
#endif
  Expect(service.Segment(StatusBarSegmentId::Lsp).text == "LSP: clangd indexing the workspace",
         "the settled refresh should still publish the segment text");
  Expect(service.Segment(StatusBarSegmentId::Lsp).tooltip ==
             "language server is indexing the workspace, some results may be partial",
         "the settled refresh should still publish the segment tooltip");
  Expect(service.Segment(StatusBarSegmentId::Lsp).tone == StatusTone::Info,
         "the settled refresh should still publish the segment tone");
}

void TestStatusBarRepoAvailabilityReflectsInSessionGitInit() {
  WorkspaceContext context;
  context.current_project_state.root = "/tmp/statusbar-git-init-probe";
  StatusBarService service;

  microide::workspace::StatusBarModelService model;
  microide::workspace::StatusBarModelService::Operations ops;
  bool repo_valid = false;
  std::size_t probes = 0;
  ops.git_availability = [&](const std::filesystem::path&) {
    ++probes;
    return repo_valid ? microide::project::GitAvailability::Repository
                      : microide::project::GitAvailability::NotARepository;
  };
  ops.active_lsp_status_strings = [](bool, std::string&, std::string&, StatusTone&) {};

  model.Refresh(service, ops, context.current_project_state, nullptr);
  Expect(service.Segment(StatusBarSegmentId::Project).text == "no-scm",
         "a non-repo project must show no-scm");
  Expect(probes == 1, "the first refresh probes");

  model.Refresh(service, ops, context.current_project_state, nullptr);
  model.Refresh(service, ops, context.current_project_state, nullptr);
  Expect(probes == 1, "an unchanged repository must not be re-probed per frame");

  repo_valid = true;
  ++context.current_project_state.sidebar.git.repository_marker_generation;
  model.Refresh(service, ops, context.current_project_state, nullptr);
  Expect(probes == 2, "a repository change re-probes");
  Expect(service.Segment(StatusBarSegmentId::Project).text.find("[clean]") != std::string::npos,
         "an in-session git init must be reflected without a stale project_root cache");

  repo_valid = false;
  ++context.current_project_state.sidebar.git.repository_marker_generation;
  model.Refresh(service, ops, context.current_project_state, nullptr);
  Expect(service.Segment(StatusBarSegmentId::Project).text == "no-scm",
         "removing .git in-session must be reflected too");
}

void TestStatusBarRendersUnknownGitAvailabilityAsUnknown() {
  WorkspaceContext context;
  context.current_project_state.root = "/tmp/statusbar-git-unknown";
  StatusBarService service;

  microide::workspace::StatusBarModelService model;
  microide::workspace::StatusBarModelService::Operations ops;
  ops.git_availability = [](const std::filesystem::path&) {
    return microide::project::GitAvailability::Unknown;
  };
  ops.active_lsp_status_strings = [](bool, std::string&, std::string&, StatusTone&) {};

  model.Refresh(service, ops, context.current_project_state, nullptr);
  const auto& segment = service.Segment(StatusBarSegmentId::Project);
  Expect(segment.visible, "the project segment stays visible while the answer is pending");
  Expect(segment.text == "scm-unknown",
         "a pending answer must not be rendered as 'no source control'");
  Expect(segment.text.find("no-scm") == std::string::npos,
         "and must not claim no-scm anywhere in the label");

  microide::workspace::StatusBarModelService not_a_repo_model;
  StatusBarService not_a_repo_service;
  ops.git_availability = [](const std::filesystem::path&) {
    return microide::project::GitAvailability::NotARepository;
  };
  not_a_repo_model.Refresh(not_a_repo_service, ops, context.current_project_state, nullptr);
  Expect(not_a_repo_service.Segment(StatusBarSegmentId::Project).text == "no-scm",
         "a settled 'not a repository' still says so");
}

void TestStatusBarNamesBranchFromHeadBeforeFirstGitSnapshot() {
  WorkspaceContext context;
  context.current_project_state.root = "/tmp/statusbar-head-branch";
  StatusBarService service;

  microide::workspace::StatusBarModelService model;
  microide::workspace::StatusBarModelService::Operations ops;
  ops.git_availability = [](const std::filesystem::path&) {
    return microide::project::GitAvailability::Repository;
  };
  ops.active_lsp_status_strings = [](bool, std::string&, std::string&, StatusTone&) {};
  std::size_t head_reads = 0;
  ops.read_head_branch = [&](const std::filesystem::path&) -> std::optional<std::string> {
    ++head_reads;
    return std::string("main");
  };

  model.Refresh(service, ops, context.current_project_state, nullptr);
  Expect(service.Segment(StatusBarSegmentId::Project).text == "main [clean]",
         "a repo with no snapshot yet should name its branch, not report no-scm");
  Expect(head_reads == 1, "the HEAD read should happen once");

  model.Refresh(service, ops, context.current_project_state, nullptr);
  model.Refresh(service, ops, context.current_project_state, nullptr);
  Expect(head_reads == 1, "the HEAD branch must be cached per project root, not re-read per frame");

  ++context.current_project_state.sidebar.git.repository_marker_generation;
  ops.read_head_branch = [&](const std::filesystem::path&) -> std::optional<std::string> {
    ++head_reads;
    return std::string("feature/stable-sort");
  };
  model.Refresh(service, ops, context.current_project_state, nullptr);
  Expect(head_reads == 2, "a repository change must re-read HEAD");
  Expect(service.Segment(StatusBarSegmentId::Project).text == "feature/stable-sort [clean]",
         "a checkout with no snapshot yet should show the new branch");
  model.Refresh(service, ops, context.current_project_state, nullptr);
  Expect(head_reads == 2, "the re-read is cached until the next repository change");

  WorkspaceContext detached;
  detached.current_project_state.root = "/tmp/statusbar-head-detached";
  StatusBarService detached_service;
  microide::workspace::StatusBarModelService detached_model;
  ops.read_head_branch = [](const std::filesystem::path&) { return std::nullopt; };
  detached_model.Refresh(detached_service, ops, detached.current_project_state, nullptr);
  Expect(detached_service.Segment(StatusBarSegmentId::Project).text == "detached [clean]",
         "a detached HEAD is not 'no source control'");

  WorkspaceContext bare;
  bare.current_project_state.root = "/tmp/statusbar-head-none";
  StatusBarService bare_service;
  microide::workspace::StatusBarModelService bare_model;
  ops.git_availability = [](const std::filesystem::path&) {
    return microide::project::GitAvailability::NotARepository;
  };
  bare_model.Refresh(bare_service, ops, bare.current_project_state, nullptr);
  Expect(bare_service.Segment(StatusBarSegmentId::Project).text == "no-scm",
         "a project outside any repository still reports no-scm");
}

void TestStatusBarProblemsTextAgreesInNumber() {
  Expect(workspace::ProblemsSegmentText(1, 0) == "1 error, 0 warnings",
         "one error: " + workspace::ProblemsSegmentText(1, 0));
  Expect(workspace::ProblemsSegmentText(2, 1) == "2 errors, 1 warning",
         "one warning: " + workspace::ProblemsSegmentText(2, 1));
  Expect(workspace::ProblemsSegmentText(0, 3) == "0 errors, 3 warnings", "plural");
}

}

void RegisterWorkspaceStatusBarTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceStatusBar/ProblemsTextAgreesInNumber", TestStatusBarProblemsTextAgreesInNumber);
  AddTest(tests, "WorkspaceStatusBar/SetSegmentReportsPaintedChanges",
          TestStatusBarSetSegmentReportsPaintedChanges);
  AddTest(tests, "WorkspaceStatusBar/RendersUnknownGitAvailabilityAsUnknown",
          TestStatusBarRendersUnknownGitAvailabilityAsUnknown);
  AddTest(tests, "WorkspaceStatusBar/NamesBranchFromHeadBeforeFirstGitSnapshot",
          TestStatusBarNamesBranchFromHeadBeforeFirstGitSnapshot);
  AddTest(tests, "WorkspaceStatusBar/RepoAvailabilityReflectsInSessionGitInit",
          TestStatusBarRepoAvailabilityReflectsInSessionGitInit);
  AddTest(tests, "WorkspaceStatusBar/SteadyRefreshDoesNotAllocate",
          TestStatusBarSteadyRefreshDoesNotAllocate);
  AddTest(tests, "WorkspaceStatusBar/LspToneFromTypedSeverityNotLabelText",
          TestStatusBarLspToneFromTypedSeverityNotLabelText);
  AddTest(tests, "WorkspaceStatusBar/BuildsVisibleSegments",
          TestStatusBarBuildsVisibleSegments);
  AddTest(tests, "WorkspaceStatusBar/NoOpsWhenNotReserved",
          TestStatusBarNoOpsWhenNotReserved);
  AddTest(tests, "WorkspaceStatusBar/CompactDropOrder",
          TestStatusBarCompactDropOrder);
  AddTest(tests, "WorkspaceStatusBar/SegmentsNeverOverlapOnANarrowBar",
          TestStatusBarSegmentsNeverOverlapOnANarrowBar);
  AddTest(tests, "WorkspaceStatusBar/PropagatesSemanticTone",
          TestStatusBarPropagatesSemanticTone);
}

}
