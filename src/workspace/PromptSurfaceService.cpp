#include "workspace/PromptSurfaceService.h"

#include <utility>

#include "util/SingleLineText.h"
#include "workspace/WorkspaceProjectState.h"

namespace microide::workspace {

PromptSurfaceService::PromptSurfaceService(ProjectWorkspaceState& state,
                                           PromptState& prompts,
                                           Operations operations)
    : state_(state),
      prompts_(prompts),
      operations_(std::move(operations)) {}

void PromptSurfaceService::DismissDirtyPrompt(bool restore_focus) {
  operations_.request_prompt_redraw();
  prompts_.dirty_visible = false;
  prompts_.dirty = DirtyPromptState{};
  if (restore_focus) {
    state_.surface.focus = prompts_.dirty_previous_focus;
  }
  operations_.request_prompt_redraw();
}

void PromptSurfaceService::DismissPromptSurface(bool restore_focus) {
  operations_.request_prompt_redraw();
  prompts_.surface_visible = false;
  prompts_.surface = PromptSurfaceState{};
  if (restore_focus) {
    state_.surface.focus = prompts_.surface_previous_focus;
  }
  operations_.request_prompt_redraw();
}

void PromptSurfaceService::OpenPromptSurface(PromptSurfaceState::Action action,
                                             PromptSurfaceState::Kind kind,
                                             const std::filesystem::path& path,
                                             std::string input) {
  operations_.request_prompt_redraw();
  prompts_.surface_visible = true;
  prompts_.surface_previous_focus = state_.surface.focus;
  prompts_.surface.kind = kind;
  prompts_.surface.action = action;
  prompts_.surface.path = path.lexically_normal();
  util::SetSingleLineText(&prompts_.surface.input, std::move(input));
  prompts_.surface.detail.clear();
  prompts_.surface.bridge_agent_id.clear();
  prompts_.surface.bridge_request_id.clear();
  prompts_.surface.tool_call_id.clear();
  prompts_.surface.tool_id.clear();
  prompts_.surface.capability_scope.clear();
  prompts_.surface.button_count = 2;
  prompts_.surface.selected_button = 0;
  state_.surface.focus = FocusTarget::Overlay;
  operations_.request_prompt_redraw();
}

void PromptSurfaceService::OpenExternalUrlPrompt(std::string url) {
  if (url.empty()) {
    return;
  }

  operations_.request_prompt_redraw();
  prompts_.surface_visible = true;
  prompts_.surface_previous_focus = state_.surface.focus;
  prompts_.surface.kind = PromptSurfaceState::Kind::Confirm;
  prompts_.surface.action = PromptSurfaceState::Action::OpenExternalUrl;
  prompts_.surface.path.clear();
  util::SetSingleLineText(&prompts_.surface.input, {});
  prompts_.surface.detail = std::move(url);
  prompts_.surface.bridge_agent_id.clear();
  prompts_.surface.bridge_request_id.clear();
  prompts_.surface.tool_call_id.clear();
  prompts_.surface.tool_id.clear();
  prompts_.surface.capability_scope.clear();
  prompts_.surface.button_count = 2;
  prompts_.surface.selected_button = 0;
  state_.surface.focus = FocusTarget::Overlay;
  operations_.request_prompt_redraw();
}

}  // namespace microide::workspace
