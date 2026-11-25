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
}

namespace microide::workspace {

class WorkspaceThemeRegistry {
 public:
  void Rebuild(const plugin::PluginHost& host);

  bool Contains(std::string_view id) const;
  std::vector<std::string> Names() const;
  std::string Label(std::string_view id) const;
  std::optional<render::Theme> Resolve(std::string_view id) const;

 private:
  struct Entry {
    std::string label;
    render::ThemeStyleMap styles;
  };
  std::unordered_map<std::string, Entry> entries_;
  mutable std::unordered_map<std::string, render::Theme> cache_;
};

}
