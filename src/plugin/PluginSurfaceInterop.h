#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "plugin/PluginHost.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin::surface_interop {

#if MICROIDE_HAS_LUA_PLUGINS

// Content-cache key for a plugin raster (RasterHandle::content_hash). Raw `rgba8`
// bytes are interpreted through the declared width/height, so two raw rasters with
// identical bytes but different geometry are DIFFERENT images and must not alias the
// same decoded texture — the dimensions and a raw/encoded discriminator are folded
// into the key. Encoded png/jpeg bytes fully determine the image, so their key is
// bytes-only (no fragmentation across differing declared display sizes). Never
// returns 0 (the cache's "no raster" sentinel).
std::uint64_t ComputeRasterContentHash(std::string_view bytes, bool is_rgba, int width,
                                       int height);

// Parse a `ctx.surface.set(id, spec)` payload and publish it for `plugin_id`.
// `spec_index` is the Lua stack index of the spec table. The spec carries exactly
// one body (`display_list` OR `raster`) plus optional `title`, `anchor`,
// `hit_regions`, and `preview`. On a raster body the encoded/raw bytes are also
// dispatched to the host texture cache via callbacks.decode_raster.
bool PublishSurface(lua_State* state,
                    std::string_view plugin_id,
                    const std::filesystem::path& current_project_root,
                    std::string_view surface_id,
                    int spec_index,
                    const PluginHost::Callbacks& callbacks,
                    std::string* error_message);

bool ClearSurface(std::string_view plugin_id,
                  std::string_view surface_id,
                  const PluginHost::Callbacks& callbacks,
                  std::string* error_message);
#endif

}  // namespace microide::plugin::surface_interop
