#include "editor/PluginSurfaceStore.h"

#include <algorithm>
#include <utility>

namespace microide::editor {

std::string PluginSurfaceStore::PathKey(const std::filesystem::path& path) {
  return path.empty() ? std::string{} : path.lexically_normal().generic_string();
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

std::span<const AnchoredSurface> PluginSurfaceStore::AnchoredSurfacesForPath(
    const std::filesystem::path& path) const {
  const auto it = anchored_by_path_.find(PathKey(path));
  if (it == anchored_by_path_.end()) {
    return {};
  }
  return std::span<const AnchoredSurface>(it->second.data(), it->second.size());
}

}  // namespace microide::editor
