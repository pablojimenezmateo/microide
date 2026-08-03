#include "workspace/registries/WorkspaceThemeRegistry.h"

#include <algorithm>

#include "plugin/PluginHost.h"

namespace microide::workspace {

void WorkspaceThemeRegistry::Clear() {
  entries_.clear();
  cache_.clear();
}

void WorkspaceThemeRegistry::Rebuild(const plugin::PluginHost& host) {
  entries_.clear();
  cache_.clear();
  for (const auto& theme : host.ContributedThemes()) {
    Entry entry;
    entry.label = theme.label.empty() ? theme.id : theme.label;
    for (const auto& style : theme.styles) {
      if (style.group.empty()) {
        continue;
      }
      render::ThemeStyle parsed;
      parsed.foreground = style.foreground;
      parsed.background = style.background;
      parsed.reverse = style.reverse;
      entry.styles[style.group] = parsed;
    }
    entries_[theme.id] = std::move(entry);
  }
}

bool WorkspaceThemeRegistry::Contains(std::string_view id) const {
  return entries_.find(std::string(id)) != entries_.end();
}

std::vector<std::string> WorkspaceThemeRegistry::Names() const {
  std::vector<std::string> names;
  names.reserve(entries_.size());
  for (const auto& [id, entry] : entries_) {
    names.push_back(id);
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::string WorkspaceThemeRegistry::Label(std::string_view id) const {
  const auto it = entries_.find(std::string(id));
  return it == entries_.end() ? std::string(id) : it->second.label;
}

std::optional<render::Theme> WorkspaceThemeRegistry::Resolve(std::string_view id) const {
  const std::string key(id);
  if (const auto cached = cache_.find(key); cached != cache_.end()) {
    return cached->second;
  }
  const auto it = entries_.find(key);
  if (it == entries_.end()) {
    return std::nullopt;
  }
  render::Theme theme = render::BuildThemeFromStyles(it->second.styles);
  cache_.emplace(key, theme);
  return theme;
}

}  // namespace microide::workspace
