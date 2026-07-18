#include "TestSupport.h"

#include "workspace/TabStripService.h"
#include "workspace/WorkspaceOutputChannels.h"
#include "workspace/WorkspaceProjectState.h"

#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::BottomPanelTabKind;
using microide::workspace::PanelContentKind;
using microide::workspace::ProjectWorkspaceState;
using microide::workspace::TabStripService;
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

}  // namespace

void RegisterTabStripServiceTests(std::vector<TestCase>& tests) {
  AddTest(tests, "TabStripService/BottomPanelTabsCacheReusesOnUnchangedState",
          TestBottomPanelTabsCacheReusesOnUnchangedState);
  AddTest(tests, "TabStripService/BottomPanelTabsCacheInvalidatesOnLabelChange",
          TestBottomPanelTabsCacheInvalidatesOnLabelChange);
  AddTest(tests, "TabStripService/BottomPanelTabsCacheInvalidatesOnOpenIdChange",
          TestBottomPanelTabsCacheInvalidatesOnOpenIdChange);
}

}  // namespace microide::tests
