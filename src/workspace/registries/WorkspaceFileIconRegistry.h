#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "editor/PluginDecorationStore.h"
#include "util/TransparentStringHash.h"

namespace microide::plugin {
class PluginHost;
}

namespace microide::workspace {

class WorkspaceFileIconRegistry {
 public:
  struct Icon {
    editor::GutterIconShape shape = editor::GutterIconShape::Dot;
    SDL_Color color{};
  };

  void Clear();
  void Rebuild(const plugin::PluginHost& host);

  std::optional<Icon> Resolve(std::string_view filename) const;

  std::uint32_t revision() const { return revision_; }

  bool has_entries() const { return !by_name_.empty() || !by_extension_.empty(); }

 private:
  using IconMap =
      std::unordered_map<std::string, Icon, util::TransparentStringHash, std::equal_to<>>;
  IconMap by_name_;
  IconMap by_extension_;
  std::uint32_t revision_ = 0;
  mutable std::string lower_scratch_;
};

}
