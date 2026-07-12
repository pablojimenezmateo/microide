#include "editor/PluginSurfaceStore.h"

#include "editor/PathKey.h"
#include "util/PathMatch.h"

#include <algorithm>
#include <utility>

namespace microide::editor {

std::string PluginSurfaceStore::PathKey(const std::filesystem::path& path) {
  return NormalizedPathKey(path);
}

void PluginSurfaceStore::RebuildAnchorIndex() {
  anchored_by_path_.clear();
  for (const auto& [owner, surfaces] : by_owner_) {
    for (const auto& [surface_id, content] : surfaces) {
      if (!content.anchor.has_value() || !content.has_body()) {
        continue;
      }
      const std::string key = PathKey(content.anchor->path);
      if (key.empty()) {
        continue;
      }
      anchored_by_path_[key].push_back(
          AnchoredSurface{content.anchor->line, owner, surface_id, &content});
    }
  }
  for (auto& [key, list] : anchored_by_path_) {
    std::sort(list.begin(), list.end(), [](const AnchoredSurface& a, const AnchoredSurface& b) {
      if (a.line != b.line) return a.line < b.line;
      if (a.owner != b.owner) return a.owner < b.owner;
      return a.surface_id < b.surface_id;
    });
  }
}

bool PluginSurfaceStore::RetargetPathPrefix(const std::filesystem::path& old_prefix,
                                            const std::filesystem::path& new_prefix) {
  const std::filesystem::path normalized_old = old_prefix.lexically_normal();
  const std::filesystem::path normalized_new = new_prefix.lexically_normal();
  if (normalized_old.empty() || normalized_new.empty() || normalized_old == normalized_new) {
    return false;
  }
  bool changed = false;
  for (auto& [owner, surfaces] : by_owner_) {
    for (auto& [surface_id, content] : surfaces) {
      if (!content.anchor.has_value()) {
        continue;
      }
      if (!util::PathEqualsOrWithin(content.anchor->path, normalized_old)) {
        continue;
      }
      content.anchor->path = util::ReplacePathPrefix(content.anchor->path, normalized_old,
                                                     normalized_new);
      changed = true;
    }
  }
  if (changed) {
    RebuildAnchorIndex();
    ++revision_;
  }
  return changed;
}

bool PluginSurfaceStore::ClearPathPrefix(const std::filesystem::path& path_prefix) {
  const std::filesystem::path normalized_prefix = path_prefix.lexically_normal();
  if (normalized_prefix.empty()) {
    return false;
  }
  bool changed = false;
  for (auto& [owner, surfaces] : by_owner_) {
    for (auto& [surface_id, content] : surfaces) {
      if (content.anchor.has_value() &&
          util::PathEqualsOrWithin(content.anchor->path, normalized_prefix)) {
        content.anchor.reset();
        changed = true;
      }
    }
  }
  if (changed) {
    RebuildAnchorIndex();
    ++revision_;
  }
  return changed;
}

bool PluginSurfaceStore::ReplaceForOwnerSurface(std::string_view owner,
                                                std::string_view surface_id,
                                                SurfaceContent content) {
  const std::string owner_key(owner);
  const std::string id_key(surface_id);
  if (owner_key.empty() || id_key.empty()) {
    return false;
  }

  auto& surfaces = by_owner_[owner_key];
  if (!content.has_body() && content.preview == SurfacePreviewSlot::None) {
    // An empty, non-preview surface is a removal.
    const bool changed = surfaces.erase(id_key) > 0;
    if (surfaces.empty()) {
      by_owner_.erase(owner_key);
    }
    if (changed) {
      RebuildAnchorIndex();
      ++revision_;
    }
    return changed;
  }

  const auto existing = surfaces.find(id_key);
  if (existing != surfaces.end() && existing->second == content) {
    return false;  // identical republish: no-op
  }
  // Cap distinct surfaces per owner. A hostile plugin publishing a fresh
  // surface_id in a loop would otherwise grow the store without bound (each
  // SurfaceContent holds a heap display list, outside the Lua memory cap since
  // PublishSurface moves it host-side) AND make every publish O(all surfaces)
  // via RebuildAnchorIndex — an O(N^2) UI-thread hang. Updates to existing ids
  // are always allowed; only brand-new ids past the cap are refused.
  if (existing == surfaces.end() && surfaces.size() >= kMaxSurfacesPerOwner) {
    return false;
  }
  surfaces[id_key] = std::move(content);
  RebuildAnchorIndex();
  ++revision_;
  return true;
}

bool PluginSurfaceStore::ClearOwnerSurface(std::string_view owner, std::string_view surface_id) {
  const auto owner_it = by_owner_.find(owner);
  if (owner_it == by_owner_.end()) {
    return false;
  }
  const auto surface_it = owner_it->second.find(surface_id);
  if (surface_it == owner_it->second.end()) {
    return false;
  }
  owner_it->second.erase(surface_it);
  if (owner_it->second.empty()) {
    by_owner_.erase(owner_it);
  }
  RebuildAnchorIndex();
  ++revision_;
  return true;
}

bool PluginSurfaceStore::ClearOwner(std::string_view owner) {
  const auto owner_it = by_owner_.find(owner);
  if (owner_it == by_owner_.end()) {
    return false;
  }
  by_owner_.erase(owner_it);
  RebuildAnchorIndex();
  ++revision_;
  return true;
}

bool PluginSurfaceStore::SetPreviewSlot(std::string_view owner, std::string_view surface_id,
                                        SurfacePreviewSlot slot) {
  const auto owner_it = by_owner_.find(owner);
  if (owner_it == by_owner_.end()) {
    return false;
  }
  const auto surface_it = owner_it->second.find(surface_id);
  if (surface_it == owner_it->second.end() || surface_it->second.preview == slot) {
    return false;
  }
  surface_it->second.preview = slot;
  ++revision_;
  return true;
}

void PluginSurfaceStore::Clear() {
  if (by_owner_.empty()) {
    return;
  }
  by_owner_.clear();
  anchored_by_path_.clear();
  ++revision_;
}

const SurfaceContent* PluginSurfaceStore::Find(std::string_view owner,
                                               std::string_view surface_id) const {
  const auto owner_it = by_owner_.find(owner);
  if (owner_it == by_owner_.end()) {
    return nullptr;
  }
  const auto surface_it = owner_it->second.find(surface_id);
  return surface_it == owner_it->second.end() ? nullptr : &surface_it->second;
}

const std::vector<SurfaceRef>& PluginSurfaceStore::PreviewSurfaces() const {
  if (by_owner_.empty()) {
    // No surfaces at all: keep the cache empty and stamped so we never iterate.
    preview_cache_.clear();
    preview_cache_revision_ = revision_;
    return preview_cache_;
  }
  if (preview_cache_revision_ == revision_) {
    return preview_cache_;  // Nothing changed since the last build.
  }
  preview_cache_.clear();
  for (const auto& [owner, surfaces] : by_owner_) {
    for (const auto& [surface_id, content] : surfaces) {
      if (content.preview != SurfacePreviewSlot::None) {
        preview_cache_.push_back(SurfaceRef{owner, surface_id, &content});
      }
    }
  }
  std::sort(preview_cache_.begin(), preview_cache_.end(),
            [](const SurfaceRef& a, const SurfaceRef& b) {
              if (a.owner != b.owner) return a.owner < b.owner;
              return a.surface_id < b.surface_id;
            });
  preview_cache_revision_ = revision_;
  return preview_cache_;
}

std::span<const AnchoredSurface> PluginSurfaceStore::AnchoredSurfacesForPathKey(
    std::string_view path_key) const {
  // Hot path: called per frame with a precomputed key. Skip the lookup when no
  // surfaces are anchored; the heterogeneous find never allocates because the
  // key string already lives on the document.
  if (anchored_by_path_.empty()) {
    return {};
  }
  const auto it = anchored_by_path_.find(path_key);
  if (it == anchored_by_path_.end()) {
    return {};
  }
  return std::span<const AnchoredSurface>(it->second.data(), it->second.size());
}

std::span<const AnchoredSurface> PluginSurfaceStore::AnchoredSurfacesForPath(
    const std::filesystem::path& path) const {
  if (anchored_by_path_.empty()) {
    return {};
  }
  return AnchoredSurfacesForPathKey(PathKey(path));
}

}  // namespace microide::editor
