#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "editor/PluginDecorationStore.h"  // editor::GutterIconShape
#include "util/TransparentStringHash.h"

namespace microide::plugin {
class PluginHost;
}  // namespace microide::plugin

namespace microide::workspace {

// Host-owned mapping from a file's name/extension to a built-in gutter-icon
// shape + colour, drawn in the file-tree leading slot (Phase D). Built-in
// defaults cover common file types; plugin-contributed file-icon themes override
// them by extension or exact filename. No raster assets — shapes reuse the
// Phase-A GutterIconRegistry vocabulary so the per-row path stays allocation- and
// string-light (one hashed lookup on a short key).
class WorkspaceFileIconRegistry {
 public:
  struct Icon {
    editor::GutterIconShape shape = editor::GutterIconShape::Dot;
    SDL_Color color{};
  };

  void Clear();
  // Rebuild plugin overrides from the host's contributed file-icon themes.
  void Rebuild(const plugin::PluginHost& host);

  // Resolve an icon for a file by its (base) name. Filename rules win over
  // extension rules; plugin rules win over built-ins. Returns nullopt for
  // directories or unknown types so the caller draws nothing.
  std::optional<Icon> Resolve(std::string_view filename) const;

  // Monotonic counter bumped whenever the plugin overrides change (Clear /
  // Rebuild). Lets render-side resolved-icon caches invalidate on theme reloads
  // without re-resolving every entry per frame.
  std::uint32_t revision() const { return revision_; }

 private:
  // Plugin overrides. Keys are lower-cased. Transparent hashing lets Resolve look
  // up by string_view without materialising a temporary key.
  using IconMap =
      std::unordered_map<std::string, Icon, util::TransparentStringHash, std::equal_to<>>;
  IconMap by_name_;
  IconMap by_extension_;
  std::uint32_t revision_ = 0;
  // Reused lowercase scratch so Resolve allocates nothing once warmed up; only
  // touched by the once-per-tree-mutation rebuild, never the per-frame path.
  mutable std::string lower_scratch_;
};

}  // namespace microide::workspace
