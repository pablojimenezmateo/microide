#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <set>
#include <string>
#include <utility>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "render/PluginDisplayList.h"
#include "util/TransparentStringHash.h"

namespace microide::editor {

// Where a plugin wants its surface previewed. `None` means the surface exists but
// is not (yet) shown in a panel; it may still render inline if it has an anchor.
enum class SurfacePreviewSlot : std::uint8_t { None, Bottom, Side };

// A decoded-raster reference. The pixels live in the host's SurfaceTextureCache
// keyed by `content_hash`; the store only carries the handle + intrinsic size.
struct RasterHandle {
  std::uint64_t content_hash = 0;
  int width = 0;
  int height = 0;

  bool operator==(const RasterHandle&) const = default;
};

// A clickable region in surface-content-local coordinates. A click dispatches
// `command` through the host's existing command runner (the same validated path
// Phase B code-lens clicks use) — no bespoke per-surface callback registry.
struct SurfaceHitRegion {
  SDL_FRect rect{0.0f, 0.0f, 0.0f, 0.0f};
  std::string command;

  bool operator==(const SurfaceHitRegion& o) const {
    return rect.x == o.rect.x && rect.y == o.rect.y && rect.w == o.rect.w &&
           rect.h == o.rect.h && command == o.command;
  }
};

// Optional inline anchor: render the surface as an inert block inset after
// `line` (0-based) of `path`. Only honored when inline surfaces are enabled.
struct SurfaceAnchor {
  std::filesystem::path path;
  std::uint32_t line = 0;

  bool operator==(const SurfaceAnchor&) const = default;
};

// One plugin surface: either a structured display list or a raster handle, plus
// its intrinsic size, optional inline anchor, hit regions, and preview request.
struct SurfaceContent {
  std::variant<std::monostate, render::PluginDisplayList, RasterHandle> body;
  float intrinsic_width = 0.0f;
  float intrinsic_height = 0.0f;
  std::string title;
  std::optional<SurfaceAnchor> anchor;
  std::vector<SurfaceHitRegion> hit_regions;
  SurfacePreviewSlot preview = SurfacePreviewSlot::None;

  bool has_body() const { return body.index() != 0; }
  bool operator==(const SurfaceContent&) const = default;
};

// A resolved anchored surface for one file, sorted by line so the inset resolver
// can slice per visible row. Pointers stay valid until the next store mutation.
struct AnchoredSurface {
  std::uint32_t line = 0;
  std::string owner;
  std::string surface_id;
  const SurfaceContent* content = nullptr;
};

// A surface requesting a panel preview, used to enumerate bottom-panel tabs.
struct SurfaceRef {
  std::string owner;
  std::string surface_id;
  const SurfaceContent* content = nullptr;
};

// Owner/id-keyed store of plugin-published content surfaces, modeled on
// PluginDecorationStore. A re-publish replaces one (owner, surface_id)
// atomically. Mutators return the redraw signal: true when something actually
// changed. An anchored-by-path index is rebuilt on mutation so the inline-inset
// resolver is O(surfaces for the file), not O(all surfaces).
class PluginSurfaceStore {
 public:
  bool ReplaceForOwnerSurface(std::string_view owner, std::string_view surface_id,
                              SurfaceContent content);
  bool ClearOwnerSurface(std::string_view owner, std::string_view surface_id);
  bool ClearOwner(std::string_view owner);
  // Host-side override of a surface's preview request (e.g. the user closed its
  // preview tab). No-op if the surface is missing or already at `slot`.
  bool SetPreviewSlot(std::string_view owner, std::string_view surface_id,
                      SurfacePreviewSlot slot);
  void Clear();

  const SurfaceContent* Find(std::string_view owner, std::string_view surface_id) const;

  // Surfaces that asked to be previewed, sorted by (owner, surface_id) for a
  // stable tab order. Returns a reference into a revision-keyed cache: the list
  // is rebuilt only when the store mutates, so the per-frame panel-tab path pays
  // nothing when nothing changed (and nothing at all when the store is empty).
  // The returned reference (and the SurfaceContent pointers it holds) stays valid
  // until the next non-const store call.
  const std::vector<SurfaceRef>& PreviewSurfaces() const;

  // Anchored surfaces for `path`, sorted by line. Empty span when none.
  std::span<const AnchoredSurface> AnchoredSurfacesForPath(
      const std::filesystem::path& path) const;
  // Hot-path variant taking a precomputed NormalizedPathKey (see
  // TextViewport::path_key). Allocation-free: the map uses heterogeneous lookup.
  std::span<const AnchoredSurface> AnchoredSurfacesForPathKey(std::string_view path_key) const;

  // Retarget every surface anchor at/under `old_prefix` to `new_prefix` (a file or
  // directory rename), mirroring PluginDecorationStore::RetargetPathPrefix so a
  // renamed file's inline surfaces follow it instead of stranding on the old path.
  bool RetargetPathPrefix(const std::filesystem::path& old_prefix,
                          const std::filesystem::path& new_prefix);
  // Drop the anchor of every surface at/under `path_prefix` (a file/dir delete), so
  // a deleted file's surfaces no longer anchor to a path that no longer exists.
  bool ClearPathPrefix(const std::filesystem::path& path_prefix);

  bool has_anchored() const { return !anchored_by_path_.empty(); }
  std::uint64_t revision() const { return revision_; }
  bool empty() const { return by_owner_.empty(); }

 private:
  static std::string PathKey(const std::filesystem::path& path);
  void RebuildAnchorIndex();

  // Upper bound on distinct surface_ids a single plugin (owner) may hold. Bounds
  // both host-side memory (each surface owns a heap display list) and the
  // O(surfaces) RebuildAnchorIndex cost paid on every publish. Well beyond any
  // real plugin's surface count; a flood of fresh ids hits this instead of
  // growing without bound. See ReplaceForOwnerSurface.
  static constexpr std::size_t kMaxSurfacesPerOwner = 8192;

  using SurfaceMap = std::unordered_map<std::string, SurfaceContent,
                                        util::TransparentStringHash, std::equal_to<>>;

  std::unordered_map<std::string, SurfaceMap, util::TransparentStringHash, std::equal_to<>>
      by_owner_;
  std::unordered_map<std::string, std::vector<AnchoredSurface>, util::TransparentStringHash,
                     std::equal_to<>>
      anchored_by_path_;
  // Host-side "user dismissed this preview" overrides keyed by (owner, surface_id).
  // A republish of the same surface with a non-None preview slot must NOT silently
  // reopen a preview the user closed, so ReplaceForOwnerSurface forces the stored
  // preview back to None while the pair is dismissed. Cleared when the user re-opens
  // the preview (SetPreviewSlot to a non-None slot) or the surface is removed.
  // (TD-2026-07-16-62.)
  std::set<std::pair<std::string, std::string>> dismissed_previews_;
  std::uint64_t revision_ = 0;
  // Revision-keyed cache for PreviewSurfaces(). `mutable` so the const accessor
  // can refresh it lazily; invalidated whenever `revision_` advances.
  mutable std::vector<SurfaceRef> preview_cache_;
  mutable std::uint64_t preview_cache_revision_ = ~0ull;
};

}  // namespace microide::editor
