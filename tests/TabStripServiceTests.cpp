#include "TestSupport.h"

#include "workspace/services/LayoutModeService.h"
#include "workspace/services/TabStripService.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceOutputChannels.h"
#include "workspace/WorkspaceTabStripChrome.h"
#include "workspace/state/WorkspaceProjectState.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::BottomPanelTabKind;
using microide::workspace::PanelContentKind;
using microide::workspace::ProjectWorkspaceState;
using microide::workspace::LayoutModeService;
using microide::workspace::TabStripService;
using microide::workspace::WorkspaceContext;
using microide::workspace::WorkspaceTabStripChrome;
using microide::workspace::WorkspaceOutputChannels;

using ChannelInfo = WorkspaceOutputChannels::ChannelInfo;

// TD-2026-07-17A-084: BuildBottomPanelTabs memoizes on a content fingerprint of
// its inputs. A same-state repeat call reuses the cached model list; any change to
// the open ids, a channel label, or the panel content must miss and rebuild.
void TestBottomPanelTabsCacheReusesOnUnchangedState() {
  TabStripService service;
  ProjectWorkspaceState state;
  state.panel.content = PanelContentKind::Output;
  state.panel.output.channel_id = "build";
  state.panel.output.open_channel_ids = {"build", "test"};
  std::vector<ChannelInfo> channels = {{"build", "Build"}, {"test", "Test Results"}};

  const auto first = service.BuildBottomPanelTabs(state, channels);
  Expect(first.size() == 2, "two output tabs are built");
  Expect(first[0].kind == BottomPanelTabKind::Output, "first tab is an output tab");
  Expect(first[0].label == "Build", "output label resolves from channel info");
  Expect(first[1].label == "Test Results", "second output label resolves");

  // Identical inputs -> cache hit -> identical result.
  const auto second = service.BuildBottomPanelTabs(state, channels);
  Expect(second.size() == first.size(), "cache hit returns the same tab count");
  Expect(second[1].label == first[1].label, "cache hit returns the same labels");
}

void TestBottomPanelTabsCacheInvalidatesOnLabelChange() {
  TabStripService service;
  ProjectWorkspaceState state;
  state.panel.content = PanelContentKind::Output;
  state.panel.output.channel_id = "build";
  state.panel.output.open_channel_ids = {"build"};
  std::vector<ChannelInfo> channels = {{"build", "Build"}};

  const auto before = service.BuildBottomPanelTabs(state, channels);
  Expect(before.size() == 1 && before[0].label == "Build", "initial label is Build");

  // A pure label change (same id, same count) must miss the cache and rebuild.
  channels[0].label = "Build (Debug)";
  const auto after = service.BuildBottomPanelTabs(state, channels);
  Expect(after.size() == 1, "still one tab after relabel");
  Expect(after[0].label == "Build (Debug)", "relabel is not served stale from the cache");
}

void TestBottomPanelTabsCacheInvalidatesOnOpenIdChange() {
  TabStripService service;
  ProjectWorkspaceState state;
  state.panel.content = PanelContentKind::Output;
  state.panel.output.channel_id = "build";
  state.panel.output.open_channel_ids = {"build"};
  std::vector<ChannelInfo> channels = {{"build", "Build"}, {"test", "Test"}};

  const auto before = service.BuildBottomPanelTabs(state, channels);
  Expect(before.size() == 1, "one open tab before");

  state.panel.output.open_channel_ids = {"build", "test"};
  const auto after = service.BuildBottomPanelTabs(state, channels);
  Expect(after.size() == 2, "opening a second channel misses the cache and rebuilds");
  Expect(after[1].output_channel_id == "test", "the newly opened channel appears");
}

// TD-2026-08-13-198: the project strip used to be rebuilt from scratch by every
// caller that asked what was on it — five per frame, each producing a title, a
// tooltip, a badge label and a text measurement per project. The chrome adapter
// memoizes the sources on a content fingerprint; these pin that a repeat call
// produces nothing, and that each input that shapes the strip still busts it.
struct ProjectStripHarness {
  WorkspaceContext context;
  TabStripService tab_strip_service;
  LayoutModeService layout_mode_service;
  WorkspaceOutputChannels output_channels;
  WorkspaceTabStripChrome chrome;
  int title_calls = 0;
  int tooltip_calls = 0;

  explicit ProjectStripHarness(std::size_t project_count) {
    for (std::size_t i = 0; i < project_count; ++i) {
      auto project = std::make_unique<ProjectWorkspaceState>();
      WorkspaceContext::RebindProjectState(*project);
      project->root = std::filesystem::path("/tmp/project") / std::to_string(i);
      context.project_catalog.entries.push_back(std::move(project));
    }
    context.current_project_state.root = context.project_catalog.entries[0]->root;
    chrome.Configure(context, tab_strip_service, layout_mode_service, output_channels,
                     WorkspaceTabStripChrome::Operations{
                         .project_tab_display_title =
                             [this](std::size_t index) {
                               ++title_calls;
                               return context.project_catalog.entries[index]->root.filename().string();
                             },
                         .project_tab_tooltip_label =
                             [this](std::size_t index) {
                               ++tooltip_calls;
                               return context.project_catalog.entries[index]->root.string();
                             },
                         .project_catalog_entry =
                             [this](std::size_t index) -> const ProjectWorkspaceState* {
                               return context.project_catalog.entries[index].get();
                             },
                         .project_catalog_root =
                             [this](std::size_t index) -> const std::filesystem::path& {
                               return context.project_catalog.entries[index]->root;
                             },
                         .current_window_rect =
                             []() { return std::optional<SDL_FRect>(SDL_FRect{0, 0, 1200, 800}); },
                         .measure_width =
                             [](std::string_view text) {
                               return static_cast<float>(text.size()) * 8.0f;
                             },
                     });
  }

  const std::vector<microide::workspace::VisibleStripTab>& Compute() {
    return chrome.ComputeVisibleProjectTabs(SDL_FRect{0.0f, 0.0f, 1200.0f, 28.0f});
  }
};

void TestProjectStripSourcesAreMemoized() {
  ProjectStripHarness harness(3);
  const auto& first = harness.Compute();
  Expect(first.size() == 3, "three project tabs are laid out");
  const int title_calls_after_first = harness.title_calls;
  Expect(title_calls_after_first == 3, "each project produced its title once");

  for (int i = 0; i < 5; ++i) {
    const auto& repeat = harness.Compute();
    Expect(repeat.size() == 3, "repeat calls return the same laid-out strip");
  }
  Expect(harness.title_calls == title_calls_after_first,
         "repeat calls produce no titles at all");
  Expect(harness.tooltip_calls == 3, "repeat calls produce no tooltip labels either");
  Expect(&first == &harness.Compute(), "the memoized vector is returned by reference");
}

void TestProjectStripCacheMissesOnDirtyFlip() {
  ProjectStripHarness harness(2);
  const auto& before = harness.Compute();
  Expect(before.size() == 2, "two project tabs before");
  const int titles_before = harness.title_calls;

  // A dirty buffer prefixes the title with "*", so the fingerprint must move.
  microide::workspace::TabEntry tab;
  tab.kind = microide::workspace::TabEntry::Kind::Editor;
  tab.editor_state.emplace();
  tab.editor_state->viewport.InsertText("dirty");
  harness.context.current_project_state.focused_group().open_tabs.push_back(std::move(tab));

  harness.Compute();
  Expect(harness.title_calls > titles_before, "a dirty flip rebuilds the strip sources");
}

void TestProjectStripCacheMissesOnGeometryEpochAndLayoutMode() {
  ProjectStripHarness harness(2);
  harness.Compute();
  const int titles_before = harness.title_calls;

  // Font metrics change with no catalog state change at all; the service epoch
  // is what carries that into the fingerprint.
  harness.tab_strip_service.InvalidateTabStripGeometry();
  harness.Compute();
  Expect(harness.title_calls > titles_before, "a geometry-epoch bump rebuilds the sources");

  const int titles_after_epoch = harness.title_calls;
  harness.layout_mode_service.SetCurrentMode(microide::workspace::LayoutMode::Compact);
  const auto& compact = harness.Compute();
  Expect(harness.title_calls > titles_after_epoch, "a layout-mode change rebuilds the sources");
  Expect(!compact.empty() && !compact.front().show_badge, "compact mode drops the project badge");
}

}  // namespace

void RegisterTabStripServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TabStripService/ProjectStripSourcesAreMemoized",
          TestProjectStripSourcesAreMemoized);
  AddTest(tests, "TabStripService/ProjectStripCacheMissesOnDirtyFlip",
          TestProjectStripCacheMissesOnDirtyFlip);
  AddTest(tests, "TabStripService/ProjectStripCacheMissesOnGeometryEpochAndLayoutMode",
          TestProjectStripCacheMissesOnGeometryEpochAndLayoutMode);
  AddTest(tests, "TabStripService/BottomPanelTabsCacheReusesOnUnchangedState",
          TestBottomPanelTabsCacheReusesOnUnchangedState);
  AddTest(tests, "TabStripService/BottomPanelTabsCacheInvalidatesOnLabelChange",
          TestBottomPanelTabsCacheInvalidatesOnLabelChange);
  AddTest(tests, "TabStripService/BottomPanelTabsCacheInvalidatesOnOpenIdChange",
          TestBottomPanelTabsCacheInvalidatesOnOpenIdChange);
}

}  // namespace microide::tests
