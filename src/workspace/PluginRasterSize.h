#pragma once

#include "editor/PluginSurfaceStore.h"
#include "render/SurfaceTextureCache.h"
#include "workspace/WorkspaceContext.h"

namespace microide::workspace {

void ApplyKnownRasterSize(editor::SurfaceContent& content,
                          const render::SurfaceTextureCache& cache);

bool ApplyDecodedRasterSizes(render::SurfaceTextureCache& cache, WorkspaceContext& context);

}
