#include "workspace/render/WorkspaceShellRenderPrimitives.h"

#include "workspace/render/MenuPopupPaint.h"

namespace microide::workspace {

void WorkspaceShell::RenderMenuPopups(SDL_Renderer* renderer, const WorkspaceLayout& /*layout*/) const {
  // The open menus as frame prep resolved them (PreparedFrameState::menu_popups):
  // each one's card and rows, drawn from that and nothing else.
  PaintMenuPopups(renderer, frame_state_, theme_, text_renderer_);
}

}  // namespace microide::workspace
