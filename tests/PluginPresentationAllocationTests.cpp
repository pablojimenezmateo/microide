#include "TestSupport.h"

#include "editor/PluginSurfaceStore.h"
#include "perf/AllocationCounter.h"
#include "plugin/PluginHost.h"
#include "project/DirectoryTree.h"
#include "workspace/WorkspaceFileIconRegistry.h"
#include "workspace/WorkspaceProjectState.h"
#include "workspace/WorkspaceStatusRegistry.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace microide::tests {
namespace {

using microide::editor::PluginSurfaceStore;
using microide::editor::SurfaceContent;
using microide::editor::SurfacePreviewSlot;
using microide::project::DirectoryTree;
using microide::workspace::ResolveStatusItems;
using microide::workspace::WorkspaceFileIconRegistry;

// --- Change 1: file icons resolve once per tree mutation, allocation-free per frame ---

void TestFileIconRegistryWarmResolveIsAllocationFree() {
  WorkspaceFileIconRegistry registry;  // No plugin themes: builtin extensions only.

  // Builtins still resolve with zero plugins loaded, and lookups are
  // case-insensitive (the filename is lower-cased into the reused scratch).
  Expect(registry.Resolve("main.cpp").has_value(), "builtin cpp icon should resolve");
  Expect(registry.Resolve("MAIN.CPP").has_value(), "icon resolution should be case-insensitive");
  Expect(!registry.Resolve("README").has_value(), "extensionless names resolve to no icon");
  Expect(!registry.Resolve(".gitignore").has_value(), "leading dot is not an extension");
  Expect(!registry.Resolve("notes.zzz").has_value(), "unknown extension resolves to no icon");

#if MICROIDE_PERF_HARNESS_BUILD
  // The first call above warmed the scratch buffer; steady-state Resolve must not
  // touch the heap, so the once-per-mutation cache rebuild stays allocation-bounded
  // and the per-frame render path (which only indexes the cache) pays nothing.
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

// Exercises the exact lazy-rebuild contract the sidebar render path relies on:
// the resolved-icon cache is rebuilt only when the tree entries or the icon-theme
// revision change, so stable frames perform no resolution at all.
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
      return;  // Stable frame: reuse the cache, resolve nothing.
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

  registry.Clear();  // Simulate a plugin icon-theme reload.
  refresh_cache();
  Expect(rebuilds == 3, "an icon-theme revision change must trigger a cache rebuild");
}

// --- Change 2: preview surfaces are revision-cached, free when empty ---

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

void TestPreviewSurfacesCacheInvalidatesOnRevisionChange() {
  PluginSurfaceStore store;
  SurfaceContent bottom;
  bottom.preview = SurfacePreviewSlot::Bottom;
  bottom.intrinsic_height = 1.0f;
  store.ReplaceForOwnerSurface("owner", "a", bottom);
  Expect(store.PreviewSurfaces().size() == 1, "a preview-requesting surface is listed");

#if MICROIDE_PERF_HARNESS_BUILD
  // A repeat call at the same revision is a pure cache hit: no allocation.
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
  store.ReplaceForOwnerSurface("owner", "b", second);  // Mutation bumps revision_.
  Expect(store.PreviewSurfaces().size() == 2, "a new preview surface invalidates the cache");

  Expect(store.SetPreviewSlot("owner", "a", SurfacePreviewSlot::None),
         "withdrawing a preview reports a change");
  Expect(store.PreviewSurfaces().size() == 1, "withdrawn surfaces drop out after invalidation");
}

// --- Change 3: status items resolve to nothing, free, with no plugins ---

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

// --- Change 4: the plugin presentation bundle is lazily allocated ---

// The decoration/surface stores live behind a unique_ptr that stays null until a
// producer (Lua plugin or LSP) publishes, and drops back to null once both stores
// drain. This is the single render-path gate that makes an unloaded session pay
// zero bytes and zero per-frame branching beyond one null check.
void TestPluginPresentationIsLazilyAllocatedAndReleased() {
  using microide::workspace::ProjectWorkspaceState;

  ProjectWorkspaceState state;
  Expect(state.plugin_presentation_if_present() == nullptr,
         "a fresh project has no plugin presentation allocated");

  // First publish allocates the bundle.
  Expect(state.EnsurePluginPresentation().decorations.ReplaceForOwnerFile(
             "owner", "file.cpp", microide::editor::PluginDecorationData{}) == false,
         "publishing empty decoration data is a no-op merge");
  // Empty data merges to nothing, so the store reports empty and a release request
  // collapses the bundle straight back to null.
  state.MaybeReleasePluginPresentation();
  Expect(state.plugin_presentation_if_present() == nullptr,
         "an empty publish must not keep the bundle alive");

  // A real contribution keeps the bundle present.
  microide::editor::PluginDecorationData data;
  data.gutter_marks.push_back(microide::editor::GutterMarkDecoration{.line = 0});
  Expect(state.EnsurePluginPresentation().decorations.ReplaceForOwnerFile("owner", "file.cpp",
                                                                          data),
         "a non-empty publish changes the merged view");
  state.MaybeReleasePluginPresentation();
  Expect(state.plugin_presentation_if_present() != nullptr,
         "a live contribution keeps the bundle allocated");

  // Clearing the only contribution releases the bundle again (zero footprint).
  Expect(state.plugin_presentation->decorations.ClearOwner("owner"),
         "clearing the sole owner changes the merged view");
  state.MaybeReleasePluginPresentation();
  Expect(state.plugin_presentation_if_present() == nullptr,
         "draining the last contribution releases the bundle back to null");
}

}  // namespace

void RegisterPluginPresentationAllocationTests(std::vector<TestCase>& tests) {
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
  AddTest(tests, "PluginPresentation/PreviewSurfacesCacheInvalidatesOnRevisionChange",
          TestPreviewSurfacesCacheInvalidatesOnRevisionChange);
  AddTest(tests, "PluginPresentation/StatusItemsEmptyHostIsAllocationFree",
          TestStatusItemsEmptyHostIsAllocationFree);
  AddTest(tests, "PluginPresentation/PresentationIsLazilyAllocatedAndReleased",
          TestPluginPresentationIsLazilyAllocatedAndReleased);
}

}  // namespace microide::tests
