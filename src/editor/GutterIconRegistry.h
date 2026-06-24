#pragma once

#include <SDL3/SDL.h>

#include <optional>
#include <string_view>

#include "editor/PluginDecorationStore.h"

namespace microide::editor {

// Host-owned mapping from a plugin's declared gutter-icon name to a built-in
// shape, plus the crisp host-drawn rendering of that shape. Keeping the name
// lookup here (at publish time) means the per-row render path only ever handles
// the interned `GutterIconShape` enum — no strings in the hot loop. The built-in
// vocabulary mirrors the existing breakpoint/diagnostic marker shapes; raster
// icon themes extend this later (Phase D) without touching the render seam.
class GutterIconRegistry {
 public:
  // Resolve a case-insensitive icon name (e.g. "bookmark", "dot", "check") to a
  // built-in shape. Unknown names return nullopt so the parser can reject them.
  static std::optional<GutterIconShape> ResolveShape(std::string_view name);

  // Bounding square for the marker on one gutter row (shared with breakpoints so
  // plugin marks line up with the host's own gutter glyphs).
  static SDL_FRect MarkerRect(float gutter_x, float y, float gutter_width, float line_height);

  // Paint `shape` in `color` within the marker rect for this row.
  static void Draw(SDL_Renderer* renderer, GutterIconShape shape, SDL_Color color, float gutter_x,
                   float y, float gutter_width, float line_height);
};

}  // namespace microide::editor
