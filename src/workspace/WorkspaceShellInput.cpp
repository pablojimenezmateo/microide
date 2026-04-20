#include "workspace/WorkspaceShell.h"

#include "workspace/WorkspaceShellBootstrapper.h"

namespace microide::workspace {

WorkspaceShell::EventResult WorkspaceShell::HandleEvent(const SDL_Event& event) {
  return Bootstrapper(*this).BuildEventDispatcher().Handle(event);
}

}  // namespace microide::workspace
