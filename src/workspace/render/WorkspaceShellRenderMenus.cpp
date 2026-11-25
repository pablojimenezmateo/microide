#include "workspace/render/WorkspaceShellRenderPrimitives.h"

#include "workspace/render/MenuPopupPaint.h"

namespace microide::workspace {

void WorkspaceShell::RenderMenuPopups(SDL_Renderer* renderer, const WorkspaceLayout& ) const {
  PaintMenuPopups(renderer, frame_state_, theme_, text_renderer_);
}

}
