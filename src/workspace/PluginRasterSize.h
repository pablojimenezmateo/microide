#pragma once

#include "editor/PluginSurfaceStore.h"
#include "render/SurfaceTextureCache.h"
#include "workspace/WorkspaceContext.h"

namespace microide::workspace {

// A plugin raster is laid out at its DECODED size. The width and height a plugin
// declares beside encoded bytes are optional and unchecked, and until a decode the
// host had no other number, so an image published without them was an inset one
// pixel tall forever and a preview that could not scroll (TD-2026-07-16-39).

// Before a publish reaches the store: a raster whose bytes are already decoded
// (an identical republish, or the same image under another id) takes that size
// now, so the republish does not put the declared size back.
void ApplyKnownRasterSize(editor::SurfaceContent& content,
                          const render::SurfaceTextureCache& cache);

// On the cache's wake: every raster decoded since the last call gets its size in
// every project's surface store. Returns whether any decode arrived, which means
// a "Rendering…" placeholder somewhere can now paint its image.
bool ApplyDecodedRasterSizes(render::SurfaceTextureCache& cache, WorkspaceContext& context);

}  // namespace microide::workspace
