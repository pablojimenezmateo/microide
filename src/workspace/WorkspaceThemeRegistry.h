#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "render/Theme.h"
#include "render/ThemeFile.h"

namespace microide::plugin {
class PluginHost;
}  // namespace microide::plugin

namespace microide::workspace {

// Host-owned registry of plugin-contributed colour themes (Phase D). A
// contributed theme is data-only: a highlight-group style map. Selecting one
// derives a full render::Theme through the same `BuildThemeFromStyles` path used
// for `.microide` colorscheme files, so plugin themes get identical
// contrast-correction. Derived themes are cached, so re-selecting is O(1).
class WorkspaceThemeRegistry {
 public:
  void Clear();
  // Rebuild from the plugin host's contributed themes. Drops the derived cache.
  void Rebuild(const plugin::PluginHost& host);

  bool Contains(std::string_view id) const;
  // Sorted ids of all contributed themes, for the colorscheme picker.
  std::vector<std::string> Names() const;
  // Human-facing label for a contributed theme id (falls back to the id).
  std::string Label(std::string_view id) const;
  // Derive (and cache) the Theme for a contributed id; nullopt if unknown.
  std::optional<render::Theme> Resolve(std::string_view id) const;

 private:
  struct Entry {
    std::string label;
    render::ThemeStyleMap styles;
  };
  std::unordered_map<std::string, Entry> entries_;
  mutable std::unordered_map<std::string, render::Theme> cache_;
};

}  // namespace microide::workspace
