#include "plugin/PluginSurfaceInterop.h"

#if MICROIDE_HAS_LUA_PLUGINS

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "editor/PluginSurfaceStore.h"
#include "plugin/PluginLuaInterop.h"
#include "plugin/PluginPathInterop.h"
#include "render/PluginDisplayList.h"
#include "render/SurfaceTextureCache.h"

namespace microide::plugin::surface_interop {
namespace {

using path_interop::ResolveRuntimePath;
using lua_interop::ReadNumberField;
using lua_interop::ReadStringField;

constexpr lua_Integer kMaxHitRegions = 4096;

// Reads the `color` field of the table at `table_index` into `out`; missing keeps
// `out` unchanged. A present-but-malformed color is an error.
bool ReadColorField(lua_State* state, int table_index, SDL_Color* out, std::string* error) {
  return lua_interop::ReadOptionalColorField(state, table_index, "color", out, error);
}

SDL_FRect ReadRectFields(lua_State* state, int table_index) {
  return SDL_FRect{ReadNumberField(state, table_index, "x", 0.0f),
                   ReadNumberField(state, table_index, "y", 0.0f),
                   ReadNumberField(state, table_index, "w", 0.0f),
                   ReadNumberField(state, table_index, "h", 0.0f)};
}

// Parse one display-list op table (already on the stack at `entry`) into the list.
// Image ops are intentionally not exposed to plugins in v1 (rasters use the
// `raster` body instead), so the recognized ops are rect/line/polyline/text/clip.
bool ReadDisplayOp(lua_State* state, int entry, render::PluginDisplayList* list,
                   std::string* error) {
  std::string kind;
  if (!ReadStringField(state, entry, "op", &kind)) {
    if (error != nullptr) *error = "display_list op requires an 'op' string";
    return false;
  }
  render::DisplayOp op;
  if (kind == "rect") {
    op.op = render::DrawOp::Rect;
    op.rect = ReadRectFields(state, entry);
    if (!ReadColorField(state, entry, &op.color, error)) return false;
  } else if (kind == "line") {
    op.op = render::DrawOp::Line;
    op.rect = SDL_FRect{ReadNumberField(state, entry, "x1", 0.0f),
                        ReadNumberField(state, entry, "y1", 0.0f),
                        ReadNumberField(state, entry, "x2", 0.0f),
                        ReadNumberField(state, entry, "y2", 0.0f)};
    if (!ReadColorField(state, entry, &op.color, error)) return false;
  } else if (kind == "text") {
    op.op = render::DrawOp::Text;
    op.rect = ReadRectFields(state, entry);
    if (!ReadColorField(state, entry, &op.color, error)) return false;
    std::string text;
    ReadStringField(state, entry, "text", &text);
    if (list->text_arena.size() + text.size() > render::kMaxDisplayTextBytes) {
      if (error != nullptr) *error = "display_list text exceeds the maximum size";
      return false;
    }
    op.data_offset = static_cast<std::uint32_t>(list->text_arena.size());
    op.data_count = static_cast<std::uint32_t>(text.size());
    list->text_arena.append(text);
  } else if (kind == "polyline") {
    op.op = render::DrawOp::Polyline;
    if (!ReadColorField(state, entry, &op.color, error)) return false;
    lua_getfield(state, entry, "points");
    if (!lua_istable(state, -1)) {
      lua_pop(state, 1);
      if (error != nullptr) *error = "polyline requires a 'points' array";
      return false;
    }
    const int points_index = lua_absindex(state, -1);
    const lua_Integer point_count = static_cast<lua_Integer>(lua_rawlen(state, points_index));
    if (point_count < 2 ||
        list->point_arena.size() + static_cast<std::size_t>(point_count) >
            render::kMaxDisplayPoints) {
      lua_pop(state, 1);
      if (error != nullptr) *error = "polyline point count is out of range";
      return false;
    }
    op.data_offset = static_cast<std::uint32_t>(list->point_arena.size());
    op.data_count = static_cast<std::uint32_t>(point_count);
    for (lua_Integer i = 1; i <= point_count; ++i) {
      lua_rawgeti(state, points_index, i);
      const int p = lua_absindex(state, -1);
      list->point_arena.push_back(
          SDL_FPoint{ReadNumberField(state, p, "x", 0.0f), ReadNumberField(state, p, "y", 0.0f)});
      lua_pop(state, 1);
    }
    lua_pop(state, 1);  // points
  } else if (kind == "clip_push") {
    op.op = render::DrawOp::ClipPush;
    op.rect = ReadRectFields(state, entry);
  } else if (kind == "clip_pop") {
    op.op = render::DrawOp::ClipPop;
  } else {
    if (error != nullptr) *error = "unknown display_list op: " + kind;
    return false;
  }
  list->ops.push_back(op);
  return true;
}

bool ReadDisplayList(lua_State* state, int dl_index, render::PluginDisplayList* list,
                     std::string* error) {
  list->content_width = ReadNumberField(state, dl_index, "width", 0.0f);
  list->content_height = ReadNumberField(state, dl_index, "height", 0.0f);
  lua_getfield(state, dl_index, "ops");
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    if (error != nullptr) *error = "display_list requires an 'ops' array";
    return false;
  }
  const int ops_index = lua_absindex(state, -1);
  const lua_Integer op_count = static_cast<lua_Integer>(lua_rawlen(state, ops_index));
  if (op_count < 0 || static_cast<std::size_t>(op_count) > render::kMaxDisplayOps) {
    lua_pop(state, 1);
    if (error != nullptr) *error = "display_list exceeds the maximum op count";
    return false;
  }
  list->ops.reserve(static_cast<std::size_t>(op_count));
  bool ok = true;
  for (lua_Integer i = 1; i <= op_count && ok; ++i) {
    lua_rawgeti(state, ops_index, i);
    if (!lua_istable(state, -1)) {
      if (error != nullptr) *error = "display_list ops must be tables";
      ok = false;
    } else {
      ok = ReadDisplayOp(state, lua_absindex(state, -1), list, error);
    }
    lua_pop(state, 1);
  }
  lua_pop(state, 1);  // ops
  if (!ok) return false;
  if (!render::ValidateDisplayList(*list, error)) return false;
  list->content_hash = render::ComputeDisplayListHash(*list);
  return true;
}

std::uint64_t HashBytes(const std::string& bytes) {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (unsigned char c : bytes) {
    h ^= static_cast<std::uint64_t>(c);
    h *= 0x100000001b3ULL;
  }
  return h == 0 ? 1 : h;  // 0 is the cache's "no raster" sentinel
}

// Parse a `raster` body, fill the handle + intrinsic size, and dispatch the bytes
// to the host texture cache for off-thread decode.
bool ReadRaster(lua_State* state, int raster_index, editor::SurfaceContent* content,
                const PluginHost::Callbacks& callbacks, std::string* error) {
  std::string format;
  ReadStringField(state, raster_index, "format", &format);
  const bool is_png = format == "png" || format == "jpeg" || format == "jpg" || format.empty();
  const bool is_rgba = format == "rgba8";
  if (!is_png && !is_rgba) {
    if (error != nullptr) *error = "raster format must be 'png' or 'rgba8'";
    return false;
  }
  lua_getfield(state, raster_index, "bytes");
  if (!lua_isstring(state, -1)) {
    lua_pop(state, 1);
    if (error != nullptr) *error = "raster requires a 'bytes' string";
    return false;
  }
  std::size_t len = 0;
  const char* data = lua_tolstring(state, -1, &len);
  if (len == 0 || len > render::SurfaceTextureCache::kMaxEncodedBytes) {
    lua_pop(state, 1);
    if (error != nullptr) *error = "raster bytes are empty or too large";
    return false;
  }
  std::string bytes(data, len);
  lua_pop(state, 1);

  const int width = static_cast<int>(ReadNumberField(state, raster_index, "width", 0.0f));
  const int height = static_cast<int>(ReadNumberField(state, raster_index, "height", 0.0f));
  const std::uint64_t hash = HashBytes(bytes);

  editor::RasterHandle handle;
  handle.content_hash = hash;
  handle.width = width;
  handle.height = height;
  content->body = handle;
  content->intrinsic_width = static_cast<float>(width);
  content->intrinsic_height = static_cast<float>(height);

  if (callbacks.decode_raster) {
    std::vector<std::byte> raw(len);
    for (std::size_t i = 0; i < len; ++i) {
      raw[i] = static_cast<std::byte>(static_cast<unsigned char>(bytes[i]));
    }
    callbacks.decode_raster(hash, is_png ? 0 : 1, std::move(raw), width, height);
  }
  return true;
}

bool ReadHitRegions(lua_State* state, int spec_index, editor::SurfaceContent* content,
                    std::string* error) {
  lua_getfield(state, spec_index, "hit_regions");
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return true;
  }
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    if (error != nullptr) *error = "hit_regions must be an array";
    return false;
  }
  const int array_index = lua_absindex(state, -1);
  const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(state, array_index));
  if (count > kMaxHitRegions) {
    lua_pop(state, 1);
    if (error != nullptr) *error = "hit_regions exceeds the maximum count";
    return false;
  }
  content->hit_regions.reserve(static_cast<std::size_t>(count));
  bool ok = true;
  for (lua_Integer i = 1; i <= count && ok; ++i) {
    lua_rawgeti(state, array_index, i);
    if (!lua_istable(state, -1)) {
      if (error != nullptr) *error = "hit_regions entries must be tables";
      ok = false;
    } else {
      const int entry = lua_absindex(state, -1);
      editor::SurfaceHitRegion region;
      region.rect = ReadRectFields(state, entry);
      ReadStringField(state, entry, "command", &region.command);
      content->hit_regions.push_back(std::move(region));
    }
    lua_pop(state, 1);
  }
  lua_pop(state, 1);  // array
  return ok;
}

editor::SurfacePreviewSlot ReadPreview(lua_State* state, int spec_index) {
  std::string slot;
  lua_getfield(state, spec_index, "preview");
  if (lua_isstring(state, -1)) {
    slot = lua_tostring(state, -1);
  }
  lua_pop(state, 1);
  if (slot == "bottom") return editor::SurfacePreviewSlot::Bottom;
  if (slot == "side") return editor::SurfacePreviewSlot::Side;
  return editor::SurfacePreviewSlot::None;
}

bool ReadAnchor(lua_State* state, int spec_index,
                const std::filesystem::path& current_project_root,
                editor::SurfaceContent* content, std::string* error) {
  lua_getfield(state, spec_index, "anchor");
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return true;
  }
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    if (error != nullptr) *error = "anchor must be a table {path, line}";
    return false;
  }
  const int anchor_index = lua_absindex(state, -1);
  std::string raw_path;
  ReadStringField(state, anchor_index, "path", &raw_path);
  lua_getfield(state, anchor_index, "line");
  const lua_Integer line = lua_isinteger(state, -1) ? lua_tointeger(state, -1) : 0;
  lua_pop(state, 1);
  lua_pop(state, 1);  // anchor
  if (raw_path.empty() || line <= 0) {
    if (error != nullptr) *error = "anchor requires a non-empty path and a 1-based line";
    return false;
  }
  editor::SurfaceAnchor anchor;
  anchor.path = ResolveRuntimePath(current_project_root, std::filesystem::path(raw_path));
  anchor.line = static_cast<std::uint32_t>(line - 1);
  content->anchor = std::move(anchor);
  return true;
}

}  // namespace

bool PublishSurface(lua_State* state, std::string_view plugin_id,
                    const std::filesystem::path& current_project_root,
                    std::string_view surface_id, int spec_index,
                    const PluginHost::Callbacks& callbacks, std::string* error_message) {
  if (!callbacks.publish_surface) {
    if (error_message != nullptr) *error_message = "surface API unavailable";
    return false;
  }
  if (surface_id.empty()) {
    if (error_message != nullptr) *error_message = "surface id must not be empty";
    return false;
  }

  const int spec = lua_absindex(state, spec_index);
  editor::SurfaceContent content;
  ReadStringField(state, spec, "title", &content.title);

  lua_getfield(state, spec, "display_list");
  const bool has_dl = lua_istable(state, -1);
  lua_pop(state, 1);
  lua_getfield(state, spec, "raster");
  const bool has_raster = lua_istable(state, -1);
  lua_pop(state, 1);

  if (has_dl && has_raster) {
    if (error_message != nullptr) {
      *error_message = "surface spec must have only one of display_list / raster";
    }
    return false;
  }
  if (!has_dl && !has_raster) {
    if (error_message != nullptr) {
      *error_message = "surface spec requires a display_list or raster body";
    }
    return false;
  }

  if (has_dl) {
    lua_getfield(state, spec, "display_list");
    render::PluginDisplayList list;
    const bool ok = ReadDisplayList(state, lua_absindex(state, -1), &list, error_message);
    lua_pop(state, 1);
    if (!ok) return false;
    content.intrinsic_width = list.content_width;
    content.intrinsic_height = list.content_height;
    content.body = std::move(list);
  } else {
    lua_getfield(state, spec, "raster");
    const bool ok = ReadRaster(state, lua_absindex(state, -1), &content, callbacks, error_message);
    lua_pop(state, 1);
    if (!ok) return false;
  }

  if (!ReadAnchor(state, spec, current_project_root, &content, error_message) ||
      !ReadHitRegions(state, spec, &content, error_message)) {
    return false;
  }
  content.preview = ReadPreview(state, spec);

  callbacks.publish_surface(std::string(plugin_id), std::string(surface_id), std::move(content));
  if (error_message != nullptr) error_message->clear();
  return true;
}

bool ClearSurface(std::string_view plugin_id, std::string_view surface_id,
                  const PluginHost::Callbacks& callbacks, std::string* error_message) {
  if (!callbacks.clear_surface) {
    if (error_message != nullptr) *error_message = "surface API unavailable";
    return false;
  }
  callbacks.clear_surface(std::string(plugin_id), std::string(surface_id));
  if (error_message != nullptr) error_message->clear();
  return true;
}

}  // namespace microide::plugin::surface_interop

#endif
