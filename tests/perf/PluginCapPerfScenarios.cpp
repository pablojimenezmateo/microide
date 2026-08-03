// Measurement scenarios backing the plugin contribution-cap derivation
// (TD-2026-07-17-019, dev-docs/project/known-tech-debt.md).
//
// The per-kind contribution ceilings in plugin/PluginContributionLimits.h bound
// two main-thread cost classes:
//  - the STATUS ITEM cap bounds the per-revision resolve (copy + parse +
//    stable_sort) that reruns on every ctx.status.update while the status bar
//    is visible (WorkspaceStatusRegistry::ResolveStatusItems);
//  - the generic per-kind cap bounds the per-reload registry rebuilds; the
//    keybinding resolve (chord parse + conflict index) is the most expensive
//    per-entry one (WorkspaceKeybindingRegistry::ResolveKeybindings).
// Both scenarios drive the real resolve seam AT the derived cap, so the
// committed baseline pins the "worst legal plugin load stays within budget"
// claim, and an accidental return to quadratic conflict scanning (the pre-019
// behavior) or a cap raise without re-measurement trips the gate.
#include "perf/PerfHarness.h"

#include <string>
#include <vector>

#include "plugin/PluginContributionLimits.h"
#include "plugin/PluginHost.h"
#include "workspace/registries/WorkspaceKeybindingRegistry.h"
#include "workspace/registries/WorkspaceStatusRegistry.h"

namespace microide::tests::perf {
namespace {

using microide::plugin::PluginHost;

// Representative status items: realistic id/tooltip lengths (heap-allocated),
// short texts (SSO), a mix of alignments/tones/icons, shuffled priorities so
// the stable_sort does real work.
std::vector<PluginHost::ContributedStatusItem> MakeStatusItems(std::size_t count) {
  std::vector<PluginHost::ContributedStatusItem> items;
  items.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    PluginHost::ContributedStatusItem item;
    item.id = "com.example.plugin.status.item-" + std::to_string(i);
    item.text = "⚙ " + std::to_string(i % 97) + " issues";
    item.tooltip = "Example diagnostic status tooltip for contributed item " + std::to_string(i);
    item.alignment = (i % 2 == 0) ? "right" : "left";
    item.priority = static_cast<int>((i * 7919) % 1024);
    item.icon = (i % 3 == 0) ? "gear" : "";
    item.tone = (i % 4 == 0) ? "warning" : "default";
    item.command = "example.plugin.command-" + std::to_string(i % 128);
    item.progress = (i % 5 == 0) ? 0.5f : -1.0f;
    item.plugin_id = "com.example.plugin";
    items.push_back(std::move(item));
  }
  return items;
}

// Representative keybindings: valid parsable chords over a realistic key space,
// so most entries past the first few hundred are conflict-skipped — the worst
// (and typical hostile) per-entry path: chord parse + conflict-index lookup.
std::vector<PluginHost::ContributedKeybinding> MakeKeybindings(std::size_t count) {
  static constexpr const char* kMods[] = {"Ctrl+", "Ctrl+Shift+", "Alt+", "Ctrl+Alt+"};
  static constexpr const char* kContexts[] = {"global", "editor", "sidebar", "terminal"};
  std::vector<PluginHost::ContributedKeybinding> bindings;
  bindings.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    PluginHost::ContributedKeybinding binding;
    binding.id = "com.example.plugin.keybinding-" + std::to_string(i);
    binding.action = "example.plugin.command-" + std::to_string(i % 128);
    binding.key_chord =
        std::string(kMods[i % 4]) + static_cast<char>('A' + static_cast<char>(i % 26));
    binding.context = kContexts[(i / 4) % 4];
    binding.plugin_id = "com.example.plugin";
    bindings.push_back(std::move(binding));
  }
  return bindings;
}

void RunStatusItemsResolveAtCap(ScenarioContext& context) {
  const auto items = MakeStatusItems(microide::plugin::kMaxPluginStatusItems);
  context.Measure("plugin_caps.status_resolve", [&]() {
    const auto resolved = microide::workspace::ResolveStatusItems(items);
    if (resolved.size() != items.size()) {
      throw std::runtime_error("status resolve dropped items");
    }
  });
}

void RunKeybindingsResolveAtCap(ScenarioContext& context) {
  const auto bindings = MakeKeybindings(microide::plugin::kMaxPluginContributionsPerKind);
  context.Measure("plugin_caps.keybinding_resolve", [&]() {
    const auto resolved = microide::workspace::ResolveKeybindings(bindings);
    if (resolved.empty()) {
      throw std::runtime_error("keybinding resolve produced no bindings");
    }
  });
}

const ScenarioRegistration g_perf_plugin_status_items_resolve_at_cap({Scenario{
    .name = "plugin_status_items_resolve_at_cap",
    .smoke = true,
    .baseline_gated = true,
    .run = RunStatusItemsResolveAtCap,
}});
const ScenarioRegistration g_perf_plugin_keybindings_resolve_at_cap({Scenario{
    .name = "plugin_keybindings_resolve_at_cap",
    .smoke = true,
    .baseline_gated = true,
    .run = RunKeybindingsResolveAtCap,
}});

}  // namespace
}  // namespace microide::tests::perf
