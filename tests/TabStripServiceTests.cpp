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

// TD-2026-08-14-209: the bottom-panel strip had a model cache but no LAID-OUT
// cache, and four call sites asked for the layout per frame. The model cache
// deliberately does not key on which tab is active or how far the strip is
// scrolled — neither shapes the model list — so the layout cache must.
void TestBottomPanelVisibleTabsAreMemoized() {
  TabStripService service;
  ProjectWorkspaceState state;
  state.panel.content = PanelContentKind::Output;
  state.panel.output.channel_id = "build";
  state.panel.output.open_channel_ids = {"build", "test", "lint"};
  std::vector<ChannelInfo> channels = {
      {"build", "Build"}, {"test", "Test Results"}, {"lint", "Lint"}};
  const SDL_FRect header{0.0f, 700.0f, 1200.0f, 26.0f};
  int measure_calls = 0;
  const auto measure = [&](std::string_view text) {
    ++measure_calls;
    return static_cast<float>(text.size()) * 8.0f;
  };

  const auto& first = service.ComputeVisibleBottomPanelTabs(
      state, header, microide::workspace::LayoutMode::Regular, measure, channels);
  Expect(first.size() == 3, "three output tabs are laid out");
  const int calls_after_first = measure_calls;

  for (int i = 0; i < 4; ++i) {
    const auto& repeat = service.ComputeVisibleBottomPanelTabs(
        state, header, microide::workspace::LayoutMode::Regular, measure, channels);
    Expect(repeat.size() == 3, "repeat calls return the same laid-out strip");
  }
  Expect(measure_calls == calls_after_first, "repeat calls measure no text at all");
  Expect(&first == &service.ComputeVisibleBottomPanelTabs(
                       state, header, microide::workspace::LayoutMode::Regular, measure, channels),
         "the memoized vector is returned by reference");

  // The strip scroll index shapes the LAYOUT but not the model, so the model
  // fingerprint cannot see it — the layout key has to.
  state.panel.tab_scroll_index = 1;
  const auto& scrolled = service.ComputeVisibleBottomPanelTabs(
      state, header, microide::workspace::LayoutMode::Regular, measure, channels);
  Expect(!scrolled.empty() && scrolled.front().index == 1,
         "scrolling the strip re-lays it out rather than serving the old scroll");

  // Same for the header rect.
  const SDL_FRect narrow{0.0f, 700.0f, 200.0f, 26.0f};
  const auto& resized = service.ComputeVisibleBottomPanelTabs(
      state, narrow, microide::workspace::LayoutMode::Regular, measure, channels);
  Expect(resized.size() <= scrolled.size(), "a narrower header fits no more tabs than a wide one");

  // And a genuine model change still misses both caches. Scroll back to the top
  // first: with the strip scrolled past it, tab 0 is not laid out at all and this
  // would be asserting about a tab that is off-screen rather than about the cache.
  state.panel.tab_scroll_index = 0;
  channels[0].label = "Build (Debug)";
  const auto& relabelled = service.ComputeVisibleBottomPanelTabs(
      state, header, microide::workspace::LayoutMode::Regular, measure, channels);
  bool found_relabelled = false;
  for (const auto& tab : relabelled) {
    found_relabelled = found_relabelled || tab.display_title == "Build (Debug)";
  }
  Expect(found_relabelled, "a relabelled channel is not served stale from the layout cache");
}

// A font-size / font-family change moves every measured width without touching a
// single thing the layout key above can see: the header rect is a constant height
// over a user-resized panel, the model fingerprint hashes labels rather than their
// widths, and the mode/active/scroll fields are all unchanged. The editor strip and
// the project strip both take the geometry epoch for exactly this; the bottom panel
// was the one strip that did not, so its tabs kept the old font's widths — wrong
// rects to paint AND wrong rects to hit-test (TD-2026-08-14-215).
void TestBottomPanelVisibleTabsMissOnGeometryEpoch() {
  TabStripService service;
  ProjectWorkspaceState state;
  state.panel.content = PanelContentKind::Output;
  state.panel.output.channel_id = "build";
  state.panel.output.open_channel_ids = {"build", "test"};
  std::vector<ChannelInfo> channels = {{"build", "Build"}, {"test", "Test Results"}};
  const SDL_FRect header{0.0f, 700.0f, 1200.0f, 26.0f};

  float glyph_width = 8.0f;
  const auto measure = [&](std::string_view text) {
    return static_cast<float>(text.size()) * glyph_width;
  };

  const auto& before = service.ComputeVisibleBottomPanelTabs(
      state, header, microide::workspace::LayoutMode::Regular, measure, channels);
  Expect(before.size() == 2, "two output tabs are laid out");
  const float width_before = before[0].rect.w;

  // Exactly what a font change does: same state, wider glyphs, epoch bumped.
  glyph_width = 16.0f;
  service.InvalidateTabStripGeometry();
  const auto& after = service.ComputeVisibleBottomPanelTabs(
      state, header, microide::workspace::LayoutMode::Regular, measure, channels);
  Expect(!after.empty(), "the strip still lays out after the font change");
  Expect(after[0].rect.w > width_before,
         "a geometry-epoch bump re-measures instead of serving the old font's widths");
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
  // `emplace()` on purpose: it is the spelling that did not compile under clang
  // while `EditorTabState` was nested inside `TabEntry` (TD-2026-08-14-214), so
  // this call site is the regression test for the hoist. Do not "simplify" it
  // back to an assignment.
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
  AddTest(tests, "TabStripService/BottomPanelVisibleTabsAreMemoized",
          TestBottomPanelVisibleTabsAreMemoized);
  AddTest(tests, "TabStripService/BottomPanelVisibleTabsMissOnGeometryEpoch",
          TestBottomPanelVisibleTabsMissOnGeometryEpoch);
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
