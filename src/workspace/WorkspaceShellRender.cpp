#include "workspace/WorkspaceShell.h"

#include "workspace/WorkspaceShellBootstrapper.h"

namespace microide::workspace {

WorkspaceRootView WorkspaceShell::MakeRootView() {
  return Bootstrapper(*this).BuildRootView();
}

void WorkspaceShell::Render(SDL_Renderer* renderer, int width, int height) {
  MakeRootView().Render(renderer, width, height);
}

void WorkspaceShell::RenderPrepared(SDL_Renderer* renderer, int width, int height) {
  MakeRootView().RenderPrepared(renderer, width, height);
}

}  // namespace microide::workspace
