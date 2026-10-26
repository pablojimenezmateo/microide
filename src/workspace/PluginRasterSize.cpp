#include "workspace/PluginRasterSize.h"

#include <variant>
#include <vector>

namespace microide::workspace {

namespace {

void ApplyToProject(ProjectWorkspaceState& project,
                    const std::vector<render::SurfaceTextureCache::RasterSize>& sizes) {
  if (project.plugin_presentation == nullptr) {
    return;
  }
  for (const render::SurfaceTextureCache::RasterSize& size : sizes) {
    (void) project.plugin_presentation->surfaces.ApplyRasterSize(size.hash, size.width,
                                                                 size.height);
  }
}

}  // namespace

void ApplyKnownRasterSize(editor::SurfaceContent& content,
                          const render::SurfaceTextureCache& cache) {
  const auto* raster = std::get_if<editor::RasterHandle>(&content.body);
  if (raster == nullptr) {
    return;
  }
  if (const auto size = cache.DecodedSize(raster->content_hash); size.has_value()) {
    (void) editor::ApplyRasterSizeTo(content, size->hash, size->width, size->height);
  }
}

bool ApplyDecodedRasterSizes(render::SurfaceTextureCache& cache, WorkspaceContext& context) {
  const std::vector<render::SurfaceTextureCache::RasterSize> sizes = cache.TakeDecodedSizes();
  if (sizes.empty()) {
    return false;
  }
  ApplyToProject(context.current_project_state, sizes);
  for (const auto& entry : context.project_catalog.entries) {
    if (entry != nullptr) {
      ApplyToProject(*entry, sizes);
    }
  }
  return true;
}

}  // namespace microide::workspace
