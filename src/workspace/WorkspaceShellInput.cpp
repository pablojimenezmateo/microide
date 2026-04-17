#include "workspace/WorkspaceShell.h"

#include "workspace/WorkspaceKeyInputCoordinator.h"
#include "workspace/WorkspaceTextInputCoordinator.h"

namespace microide::workspace {

namespace {

struct ScopeExit {
  std::function<void()> on_exit;

  ~ScopeExit() {
    if (on_exit) {
      on_exit();
    }
  }
};

}  // namespace

WorkspaceShell::EventResult WorkspaceShell::HandleEvent(const SDL_Event& event) {
  TextInputCoordinator text_input(*this);
  const ScopeExit sync_terminal_focus{[this]() { SyncTerminalFocusState(); }};
  const auto finish = [this](bool handled) {
    return EventResult{
        .handled = handled,
        .redraw = ConsumePendingRenderInvalidation(),
    };
  };
  if (project_open_dialog_event_type_ != 0 && event.type == project_open_dialog_event_type_) {
    ConsumePendingProjectOpenDialogResult();
    return finish(true);
  }
  if (project_search_runtime_.HandlesEvent(event.type)) {
    ConsumeProjectSearchUpdates();
    return finish(true);
  }
  if (git_blame_event_type_ != 0 && event.type == git_blame_event_type_) {
    RequestFocusedEditorRedraw();
    return finish(true);
  }
  if (terminal_event_type_ != 0 && event.type == terminal_event_type_) {
    ConsumeTerminalSessionUpdates();
    return finish(true);
  }

  text_input.SyncTextInputSurface(nullptr);

  switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      return finish(HandleMouseButtonDown(event));
    case SDL_EVENT_MOUSE_BUTTON_UP:
      return finish(HandleMouseButtonUp(event));
    case SDL_EVENT_MOUSE_MOTION:
      return finish(HandleMouseMotion(event));
    case SDL_EVENT_MOUSE_WHEEL:
      return finish(HandleMouseWheel(event));
    case SDL_EVENT_TEXT_EDITING:
      return finish(text_input.HandleTextEditing(event.edit));
    case SDL_EVENT_TEXT_INPUT:
      return finish(text_input.HandleTextInput(event.text));
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      surface_.window_has_input_focus = true;
      RequestWindowRedraw();
      return finish(true);
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      surface_.window_has_input_focus = false;
      RequestWindowRedraw();
      return finish(true);
    case SDL_EVENT_KEY_DOWN:
      break;
    default:
      return finish(false);
  }

  return finish(KeyInputCoordinator(*this).HandleKeyDown(event.key));
}

}  // namespace microide::workspace
