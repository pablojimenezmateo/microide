#include "TestSupport.h"

#include "editor/PathKey.h"
#include "editor/PluginDecorationStore.h"
#include "editor/PluginSurfaceStore.h"
#include "perf/AllocationCounter.h"
#include "plugin/LuaRuntime.h"
#include "plugin/PluginHost.h"
#include "plugin/PluginHostRuntimeTypes.h"
#include "plugin/PluginPresentationRegistrationParsers.h"
#include "project/DirectoryTree.h"
#include "workspace/registries/WorkspaceFileIconRegistry.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/registries/WorkspaceStatusRegistry.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::PluginSurfaceStore;
using microide::editor::SurfaceContent;
using microide::editor::SurfacePreviewSlot;
using microide::project::DirectoryTree;
using microide::workspace::ResolveStatusItems;
using microide::workspace::StatusItemCache;
using microide::workspace::StatusItemView;
using microide::workspace::WorkspaceFileIconRegistry;

void TestFileIconRegistryWarmResolveIsAllocationFree() {
  WorkspaceFileIconRegistry registry;

  Expect(registry.Resolve("main.cpp").has_value(), "builtin cpp icon should resolve");
  Expect(registry.Resolve("MAIN.CPP").has_value(), "icon resolution should be case-insensitive");
  Expect(!registry.Resolve("README").has_value(), "extensionless names resolve to no icon");
  Expect(!registry.Resolve(".gitignore").has_value(), "leading dot is not an extension");
  Expect(!registry.Resolve("notes.zzz").has_value(), "unknown extension resolves to no icon");

#if MICROIDE_PERF_HARNESS_BUILD
  const microide::tests::perf::AllocationSnapshot before =
      microide::tests::perf::Allocations::Snapshot();
  for (int i = 0; i < 64; ++i) {
    (void)registry.Resolve("main.cpp");
    (void)registry.Resolve("photo.png");
    (void)registry.Resolve("script.sh");
    (void)registry.Resolve("unknown.zzz");
  }
  const microide::tests::perf::AllocationDelta delta =
      microide::tests::perf::Allocations::DeltaSince(before);
  Expect(delta.allocations == 0 && delta.bytes_allocated == 0,
         "warm WorkspaceFileIconRegistry::Resolve must not allocate on the heap");
#endif
}

void TestFileIconRegistryRevisionBumpsOnMutation() {
  WorkspaceFileIconRegistry registry;
  const std::uint32_t r0 = registry.revision();
  registry.Clear();
  Expect(registry.revision() != r0, "Clear() must advance the icon-theme revision");
}

void TestDirectoryTreeEntriesRevisionAdvancesOnRebuild() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "a.cpp", "int a;\n");
  WriteFile(root / "b.py", "b = 1\n");

  DirectoryTree tree;
  const std::uint64_t before_root = tree.entries_revision();
  Expect(tree.SetRoot(root), "directory tree should open fixture root");
  Expect(tree.entries_revision() != before_root, "SetRoot must advance entries_revision");

  const std::uint64_t after_root = tree.entries_revision();
  tree.Refresh();
  Expect(tree.entries_revision() != after_root, "Refresh must advance entries_revision");
}

void TestFileIconCacheRebuildsOnlyOnRevisionChange() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path() / "project";
  WriteFile(root / "main.cpp", "int main(){}\n");
  WriteFile(root / "data.json", "{}\n");

  DirectoryTree tree;
  Expect(tree.SetRoot(root), "directory tree should open fixture root");
  WorkspaceFileIconRegistry registry;

  std::vector<std::optional<WorkspaceFileIconRegistry::Icon>> icons;
  std::uint64_t cached_tree_rev = ~0ull;
  std::uint32_t cached_icon_rev = ~0u;
  int rebuilds = 0;

  const auto refresh_cache = [&]() {
    const std::uint64_t tree_rev = tree.entries_revision();
    const std::uint32_t icon_rev = registry.revision();
    if (cached_tree_rev == tree_rev && cached_icon_rev == icon_rev) {
      return;
    }
    const auto& entries = tree.entries();
    icons.assign(entries.size(), std::nullopt);
    for (std::size_t i = 0; i < entries.size(); ++i) {
      if (!entries[i].is_directory) {
        icons[i] = registry.Resolve(entries[i].label);
      }
    }
    cached_tree_rev = tree_rev;
    cached_icon_rev = icon_rev;
    ++rebuilds;
  };

  refresh_cache();
  refresh_cache();
  refresh_cache();
  Expect(rebuilds == 1, "stable frames must not rebuild the resolved-icon cache");
  Expect(icons.size() == tree.entries().size(), "cache must stay parallel to tree entries");

  tree.Refresh();
  refresh_cache();
  Expect(rebuilds == 2, "a tree mutation must trigger exactly one cache rebuild");

  registry.Clear();
  refresh_cache();
  Expect(rebuilds == 3, "an icon-theme revision change must trigger a cache rebuild");
}

void TestPreviewSurfacesEmptyStoreIsAllocationFree() {
  PluginSurfaceStore store;
  const std::vector<microide::editor::SurfaceRef>& first = store.PreviewSurfaces();
  Expect(first.empty(), "empty store yields no preview surfaces");
  const std::vector<microide::editor::SurfaceRef>& second = store.PreviewSurfaces();
  Expect(&first == &second, "PreviewSurfaces must return a stable cached reference");

#if MICROIDE_PERF_HARNESS_BUILD
  const microide::tests::perf::AllocationSnapshot before =
      microide::tests::perf::Allocations::Snapshot();
  for (int i = 0; i < 64; ++i) {
    (void)store.PreviewSurfaces();
  }
  const microide::tests::perf::AllocationDelta delta =
      microide::tests::perf::Allocations::DeltaSince(before);
  Expect(delta.allocations == 0 && delta.bytes_allocated == 0,
         "PreviewSurfaces on an empty store must not allocate");
#endif
}

void TestEmptyStorePathLookupsAreAllocationFree() {
  microide::editor::PluginDecorationStore decorations;
  PluginSurfaceStore surfaces;
  const std::filesystem::path path = "/project/src/main.cpp";
  Expect(decorations.FindByPath(path) == nullptr, "empty decoration store finds nothing");
  Expect(surfaces.AnchoredSurfacesForPath(path).empty(), "empty surface store anchors nothing");

#if MICROIDE_PERF_HARNESS_BUILD
  const microide::tests::perf::AllocationSnapshot before =
      microide::tests::perf::Allocations::Snapshot();
  for (int i = 0; i < 64; ++i) {
    (void)decorations.FindByPath(path);
    (void)surfaces.AnchoredSurfacesForPath(path);
  }
  const microide::tests::perf::AllocationDelta delta =
      microide::tests::perf::Allocations::DeltaSince(before);
  Expect(delta.allocations == 0 && delta.bytes_allocated == 0,
         "empty-store path lookups must not allocate on the render hot path");
#endif
}

void TestPopulatedStoreKeyLookupIsAllocationFree() {
  using microide::editor::NormalizedPathKey;
  using microide::editor::PluginDecorationData;
  using microide::editor::TextStyleDecoration;

  microide::editor::PluginDecorationStore decorations;
  const std::filesystem::path path = "/project/src/main.cpp";
  PluginDecorationData data;
  data.text_styles = {TextStyleDecoration{.line = 1, .start_column = 0, .end_column = 4}};
  Expect(decorations.ReplaceForOwnerFile("lsp", path, std::move(data)),
         "publishing decorations should change the store");

  const std::string cached_key = NormalizedPathKey(path);
  Expect(decorations.FindByPathKey(cached_key) != nullptr,
         "key lookup should find the published decorations");

#if MICROIDE_PERF_HARNESS_BUILD
  const microide::tests::perf::AllocationSnapshot before =
      microide::tests::perf::Allocations::Snapshot();
  for (int i = 0; i < 64; ++i) {
    (void)decorations.FindByPathKey(cached_key);
  }
  const microide::tests::perf::AllocationDelta delta =
      microide::tests::perf::Allocations::DeltaSince(before);
  Expect(delta.allocations == 0 && delta.bytes_allocated == 0,
         "key-based lookup against a populated store must not allocate per frame");
#endif
}

void TestPreviewSurfacesCacheInvalidatesOnRevisionChange() {
  PluginSurfaceStore store;
  SurfaceContent bottom;
  bottom.preview = SurfacePreviewSlot::Bottom;
  bottom.intrinsic_height = 1.0f;
  store.ReplaceForOwnerSurface("owner", "a", bottom);
  Expect(store.PreviewSurfaces().size() == 1, "a preview-requesting surface is listed");

#if MICROIDE_PERF_HARNESS_BUILD
  const microide::tests::perf::AllocationSnapshot before =
      microide::tests::perf::Allocations::Snapshot();
  (void)store.PreviewSurfaces();
  const microide::tests::perf::AllocationDelta delta =
      microide::tests::perf::Allocations::DeltaSince(before);
  Expect(delta.allocations == 0, "a same-revision PreviewSurfaces call must be a free cache hit");
#endif

  SurfaceContent second;
  second.preview = SurfacePreviewSlot::Bottom;
  second.intrinsic_height = 1.0f;
  store.ReplaceForOwnerSurface("owner", "b", second);
  Expect(store.PreviewSurfaces().size() == 2, "a new preview surface invalidates the cache");

  Expect(store.SetPreviewSlot("owner", "a", SurfacePreviewSlot::None),
         "withdrawing a preview reports a change");
  Expect(store.PreviewSurfaces().size() == 1, "withdrawn surfaces drop out after invalidation");
}

void TestStatusItemsEmptyHostIsAllocationFree() {
  plugin::PluginHost host;
  Expect(ResolveStatusItems(host).empty(), "a host with no contributions yields no status items");

#if MICROIDE_PERF_HARNESS_BUILD
  const microide::tests::perf::AllocationSnapshot before =
      microide::tests::perf::Allocations::Snapshot();
  for (int i = 0; i < 64; ++i) {
    Expect(ResolveStatusItems(host).empty(), "status items stay empty across calls");
  }
  const microide::tests::perf::AllocationDelta delta =
      microide::tests::perf::Allocations::DeltaSince(before);
  Expect(delta.allocations == 0 && delta.bytes_allocated == 0,
         "ResolveStatusItems must not allocate when no plugin contributes status items");
#endif
}

void TestStatusItemCacheReturnsStableReferenceWhenUnchanged() {
  plugin::PluginHost host;
  StatusItemCache cache;
  const std::vector<StatusItemView>& first = ResolveStatusItems(host, cache);
  Expect(first.empty(), "an empty host resolves to no status items");
  const std::vector<StatusItemView>& second = ResolveStatusItems(host, cache);
  Expect(&first == &second, "a same-revision resolve returns the cached view by reference");

#if MICROIDE_PERF_HARNESS_BUILD
  const microide::tests::perf::AllocationSnapshot before =
      microide::tests::perf::Allocations::Snapshot();
  for (int i = 0; i < 64; ++i) {
    (void)ResolveStatusItems(host, cache);
  }
  const microide::tests::perf::AllocationDelta delta =
      microide::tests::perf::Allocations::DeltaSince(before);
  Expect(delta.allocations == 0 && delta.bytes_allocated == 0,
         "a cached status-item resolve at a stable revision must not allocate");
#endif
}

void TestLspSemanticDecorationsClearPerFileAndRelease() {
  using microide::workspace::ProjectWorkspaceState;
  ProjectWorkspaceState state;

  microide::editor::PluginDecorationData a;
  a.gutter_marks.push_back(microide::editor::GutterMarkDecoration{.line = 0});
  microide::editor::PluginDecorationData b;
  b.gutter_marks.push_back(microide::editor::GutterMarkDecoration{.line = 0});
  Expect(state.EnsurePluginPresentation().decorations.ReplaceForOwnerFile("lsp:semantic", "a.cpp",
                                                                          a),
         "publishing semantic decorations for a.cpp changes the merged view");
  Expect(state.EnsurePluginPresentation().decorations.ReplaceForOwnerFile("lsp:semantic", "b.cpp",
                                                                          b),
         "publishing semantic decorations for b.cpp changes the merged view");

  Expect(state.plugin_presentation->decorations.ClearOwnerFile("lsp:semantic", "a.cpp"),
         "closing a.cpp clears its semantic decorations");
  state.MaybeReleasePluginPresentation();
  Expect(state.plugin_presentation_if_present() != nullptr,
         "the bundle stays alive while b.cpp still has decorations");
  Expect(state.plugin_presentation->decorations.FindByPath("a.cpp") == nullptr,
         "a.cpp has no remaining merged decorations after close");
  Expect(state.plugin_presentation->decorations.FindByPath("b.cpp") != nullptr,
         "b.cpp decorations are untouched by closing a.cpp");

  Expect(state.plugin_presentation->decorations.ClearOwnerFile("lsp:semantic", "b.cpp"),
         "closing b.cpp clears its semantic decorations");
  state.MaybeReleasePluginPresentation();
  Expect(state.plugin_presentation_if_present() == nullptr,
         "draining the last semantic decorations releases the bundle back to null");
}

void TestPluginPresentationIsLazilyAllocatedAndReleased() {
  using microide::workspace::ProjectWorkspaceState;

  ProjectWorkspaceState state;
  Expect(state.plugin_presentation_if_present() == nullptr,
         "a fresh project has no plugin presentation allocated");

  Expect(state.EnsurePluginPresentation().decorations.ReplaceForOwnerFile(
             "owner", "file.cpp", microide::editor::PluginDecorationData{}) == false,
         "publishing empty decoration data is a no-op merge");
  state.MaybeReleasePluginPresentation();
  Expect(state.plugin_presentation_if_present() == nullptr,
         "an empty publish must not keep the bundle alive");

  microide::editor::PluginDecorationData data;
  data.gutter_marks.push_back(microide::editor::GutterMarkDecoration{.line = 0});
  Expect(state.EnsurePluginPresentation().decorations.ReplaceForOwnerFile("owner", "file.cpp",
                                                                          data),
         "a non-empty publish changes the merged view");
  state.MaybeReleasePluginPresentation();
  Expect(state.plugin_presentation_if_present() != nullptr,
         "a live contribution keeps the bundle allocated");

  Expect(state.plugin_presentation->decorations.ClearOwner("owner"),
         "clearing the sole owner changes the merged view");
  state.MaybeReleasePluginPresentation();
  Expect(state.plugin_presentation_if_present() == nullptr,
         "draining the last contribution releases the bundle back to null");
}

void TestFileIconRegistryHasNoEntriesWithoutPlugins() {
  WorkspaceFileIconRegistry registry;
  Expect(!registry.has_entries(),
         "a registry with no plugin contributions reports has_entries() == false");
  registry.Clear();
  Expect(!registry.has_entries(), "Clear() leaves the registry without plugin entries");
  Expect(registry.Resolve("main.cpp").has_value(),
         "built-in icons still resolve without a plugin theme");
  Expect(!registry.has_entries(),
         "resolving a built-in icon must not populate the contribution maps");
}

void TestFileIconCacheGateLeavesNoFootprintWhenDisabled() {
  using microide::workspace::FileIconRenderCache;
  FileIconRenderCache cache;
  cache.icons.assign(8, std::nullopt);
  cache.tree_revision = 3;
  cache.icon_revision = 1;
  cache.Reset();
  Expect(cache.icons.empty(), "Reset() drops the resolved-icon vector");
  Expect(cache.tree_revision == ~0ull && cache.icon_revision == ~0u,
         "Reset() restores the sentinel revisions so a later re-enable rebuilds");

#if MICROIDE_PERF_HARNESS_BUILD
  const microide::tests::perf::AllocationSnapshot before =
      microide::tests::perf::Allocations::Snapshot();
  for (int i = 0; i < 64; ++i) {
    if (!cache.icons.empty()) {
      cache.Reset();
    }
  }
  const microide::tests::perf::AllocationDelta delta =
      microide::tests::perf::Allocations::DeltaSince(before);
  Expect(delta.allocations == 0 && delta.bytes_allocated == 0,
         "a disabled file-icon frame must not allocate");
#endif
}

}

void TestThemeStyleMapRespectsCap() {
#if MICROIDE_HAS_LUA_PLUGINS
  std::string create_error;
  auto runtime = microide::plugin::LuaRuntime::Create(&create_error);
  Expect(runtime != nullptr, "lua runtime should create");
  lua_State* state = runtime->state();

  lua_newtable(state);
  lua_pushstring(state, "big");
  lua_setfield(state, -2, "id");
  lua_newtable(state);
  for (int i = 0; i < 2000; ++i) {
    const std::string group = "grp" + std::to_string(i);
    lua_pushstring(state, "#112233");
    lua_setfield(state, -2, group.c_str());
  }
  lua_setfield(state, -2, "styles");

  std::vector<microide::plugin::PluginHost::ContributedTheme> themes;
  std::string error_message;
  const bool ok = microide::plugin::presentation_interop::RegisterTheme(
      state, "test.plugin", &themes, &error_message);
  lua_pop(state, 1);

  Expect(ok, "a valid (if oversized) theme table should register: " + error_message);
  Expect(themes.size() == 1, "exactly one theme should be contributed");
  Expect(themes.front().styles.size() <= 512,
         "an oversized style map must be truncated to the retained-style cap (512)");
#endif
}

void RegisterPluginPresentationAllocationTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PluginPresentation/ThemeStyleMapRespectsCap", TestThemeStyleMapRespectsCap);
  AddTest(tests, "PluginPresentation/FileIconRegistryWarmResolveIsAllocationFree",
          TestFileIconRegistryWarmResolveIsAllocationFree);
  AddTest(tests, "PluginPresentation/FileIconRegistryRevisionBumpsOnMutation",
          TestFileIconRegistryRevisionBumpsOnMutation);
  AddTest(tests, "PluginPresentation/DirectoryTreeEntriesRevisionAdvancesOnRebuild",
          TestDirectoryTreeEntriesRevisionAdvancesOnRebuild);
  AddTest(tests, "PluginPresentation/FileIconCacheRebuildsOnlyOnRevisionChange",
          TestFileIconCacheRebuildsOnlyOnRevisionChange);
  AddTest(tests, "PluginPresentation/PreviewSurfacesEmptyStoreIsAllocationFree",
          TestPreviewSurfacesEmptyStoreIsAllocationFree);
  AddTest(tests, "PluginPresentation/EmptyStorePathLookupsAreAllocationFree",
          TestEmptyStorePathLookupsAreAllocationFree);
  AddTest(tests, "PluginPresentation/PopulatedStoreKeyLookupIsAllocationFree",
          TestPopulatedStoreKeyLookupIsAllocationFree);
  AddTest(tests, "PluginPresentation/PreviewSurfacesCacheInvalidatesOnRevisionChange",
          TestPreviewSurfacesCacheInvalidatesOnRevisionChange);
  AddTest(tests, "PluginPresentation/StatusItemsEmptyHostIsAllocationFree",
          TestStatusItemsEmptyHostIsAllocationFree);
  AddTest(tests, "PluginPresentation/StatusItemCacheReturnsStableReferenceWhenUnchanged",
          TestStatusItemCacheReturnsStableReferenceWhenUnchanged);
  AddTest(tests, "PluginPresentation/LspSemanticDecorationsClearPerFileAndRelease",
          TestLspSemanticDecorationsClearPerFileAndRelease);
  AddTest(tests, "PluginPresentation/PresentationIsLazilyAllocatedAndReleased",
          TestPluginPresentationIsLazilyAllocatedAndReleased);
  AddTest(tests, "PluginPresentation/FileIconRegistryHasNoEntriesWithoutPlugins",
          TestFileIconRegistryHasNoEntriesWithoutPlugins);
  AddTest(tests, "PluginPresentation/FileIconCacheGateLeavesNoFootprintWhenDisabled",
          TestFileIconCacheGateLeavesNoFootprintWhenDisabled);
}

}
