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
  // `clickable` is derived from the command now, so the fixture supplies one when
  // the caller wants an actionable segment.
  service.SetSegment(id, StatusBarSegmentValue{
                             .text = std::move(text),
                             .tooltip = {},
                             .command = clickable ? "sidebar-show" : "",
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
                         .command = "sidebar-show",
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

// The status-bar model is refreshed once per PAINTED FRAME, and on a steady frame
// every segment says exactly what it said last frame. It used to rebuild each one
// anyway: a fresh `StatusBarSegmentValue` (one allocation per non-SSO field), then
// a move-assign into the slot that also freed the slot's identically sized
// buffers. Six allocations a frame, forever, for text that had not changed
// (TD-2026-08-14-229). Publishing through views and assigning INTO the slot makes
// a settled frame allocation-free, and this is the assertion that says so.
void TestStatusBarSteadyRefreshDoesNotAllocate() {
  WorkspaceContext context;
  StatusBarService service;
  editor::TextViewport viewport;

  microide::workspace::StatusBarModelService model;
  microide::workspace::StatusBarModelService::Operations ops;
  ops.is_git_repo_valid = [](const std::filesystem::path&) { return false; };
  // Long enough to be past std::string's small-string buffer, which is the whole
  // point: an SSO-sized label would pass this test on the old code too.
  ops.active_lsp_status_strings = [](bool, std::string& text, std::string& tooltip,
                                     StatusBarSegmentTone& tone) {
    text = "LSP: clangd indexing the workspace";
    tooltip = "language server is indexing the workspace, some results may be partial";
    tone = StatusBarSegmentTone::Info;
  };

  // Two warm-up refreshes: the first fills every memo and every slot buffer, the
  // second proves the memos hold. Only the third is measured.
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
  Expect(service.Segment(StatusBarSegmentId::Lsp).tone == StatusBarSegmentTone::Info,
         "the settled refresh should still publish the segment tone");
}

void TestStatusBarRepoAvailabilityReflectsInSessionGitInit() {
  // The repo-availability probe must not be cached by project_root alone: an
  // in-session `git init` (or `.git` removal) must be reflected, not stay stale
  // until the project root changes. It is keyed on the repository marker
  // generation, which ApplyProjectChangeBatch bumps for every repository change
  // — and `.git` appearing is now one (GitRepositoryMetadataTracker).
  WorkspaceContext context;
  context.current_project_state.root = "/tmp/statusbar-git-init-probe";
  StatusBarService service;

  microide::workspace::StatusBarModelService model;
  microide::workspace::StatusBarModelService::Operations ops;
  bool repo_valid = false;
  std::size_t probes = 0;
  ops.is_git_repo_valid = [&](const std::filesystem::path&) {
    ++probes;
    return repo_valid;
  };
  ops.active_lsp_status_strings = [](bool, std::string&, std::string&, StatusBarSegmentTone&) {};

  model.Refresh(service, ops, context.current_project_state, nullptr);
  Expect(service.Segment(StatusBarSegmentId::Project).text == "no-scm",
         "a non-repo project must show no-scm");
  Expect(probes == 1, "the first refresh probes");

  // The status bar rebuilds every frame. With nothing claiming the repository
  // changed, the probe must not run again — it is a filesystem stat.
  model.Refresh(service, ops, context.current_project_state, nullptr);
  model.Refresh(service, ops, context.current_project_state, nullptr);
  Expect(probes == 1, "an unchanged repository must not be re-probed per frame");

  // Simulate `git init`: the marker generation moves, and the same project root
  // now reports a valid repo.
  repo_valid = true;
  ++context.current_project_state.sidebar.git.repository_marker_generation;
  model.Refresh(service, ops, context.current_project_state, nullptr);
  Expect(probes == 2, "a repository change re-probes");
  Expect(service.Segment(StatusBarSegmentId::Project).text.find("[clean]") != std::string::npos,
         "an in-session git init must be reflected without a stale project_root cache");

  // And the reverse transition, which is the same event.
  repo_valid = false;
  ++context.current_project_state.sidebar.git.repository_marker_generation;
  model.Refresh(service, ops, context.current_project_state, nullptr);
  Expect(service.Segment(StatusBarSegmentId::Project).text == "no-scm",
         "removing .git in-session must be reflected too");
}

// A git checkout with no `git status` snapshot yet labelled itself
// "no-scm [clean]" -- a contradiction, since only source control can know a tree
// is clean. That is the state for the first seconds after opening a project, and
// it persists indefinitely if the user never opens the Source Control view. The
// branch now comes from `<gitdir>/HEAD` (one file read, no subprocess) until a
// real snapshot supersedes it.
void TestStatusBarNamesBranchFromHeadBeforeFirstGitSnapshot() {
  WorkspaceContext context;
  context.current_project_state.root = "/tmp/statusbar-head-branch";
  StatusBarService service;

  microide::workspace::StatusBarModelService model;
  microide::workspace::StatusBarModelService::Operations ops;
  ops.is_git_repo_valid = [](const std::filesystem::path&) { return true; };
  ops.active_lsp_status_strings = [](bool, std::string&, std::string&, StatusBarSegmentTone&) {};
  std::size_t head_reads = 0;
  ops.read_head_branch = [&](const std::filesystem::path&) -> std::optional<std::string> {
    ++head_reads;
    return std::string("main");
  };

  model.Refresh(service, ops, context.current_project_state, nullptr);
  Expect(service.Segment(StatusBarSegmentId::Project).text == "main [clean]",
         "a repo with no snapshot yet should name its branch, not report no-scm");
  Expect(head_reads == 1, "the HEAD read should happen once");

  // The status bar rebuilds every frame; HEAD must not be re-read each time.
  model.Refresh(service, ops, context.current_project_state, nullptr);
  model.Refresh(service, ops, context.current_project_state, nullptr);
  Expect(head_reads == 1, "the HEAD branch must be cached per project root, not re-read per frame");

  // A detached HEAD has no branch name, but it is still a repository.
  WorkspaceContext detached;
  detached.current_project_state.root = "/tmp/statusbar-head-detached";
  StatusBarService detached_service;
  microide::workspace::StatusBarModelService detached_model;
  ops.read_head_branch = [](const std::filesystem::path&) { return std::nullopt; };
  detached_model.Refresh(detached_service, ops, detached.current_project_state, nullptr);
  Expect(detached_service.Segment(StatusBarSegmentId::Project).text == "detached [clean]",
         "a detached HEAD is not 'no source control'");

  // No repository at all still reads no-scm.
  WorkspaceContext bare;
  bare.current_project_state.root = "/tmp/statusbar-head-none";
  StatusBarService bare_service;
  microide::workspace::StatusBarModelService bare_model;
  ops.is_git_repo_valid = [](const std::filesystem::path&) { return false; };
  bare_model.Refresh(bare_service, ops, bare.current_project_state, nullptr);
  Expect(bare_service.Segment(StatusBarSegmentId::Project).text == "no-scm",
         "a project outside any repository still reports no-scm");
}

}  // namespace

void RegisterWorkspaceStatusBarTests(std::vector<TestCase>& tests) {
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
  AddTest(tests, "WorkspaceStatusBar/PropagatesSemanticTone",
          TestStatusBarPropagatesSemanticTone);
}

}  // namespace microide::tests
