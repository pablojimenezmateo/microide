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

void PromptSurfaceService::ShowDirtyPromptForTab(std::size_t index) {
  if (index >= state_.open_tabs.size()) {
    return;
  }

  operations_.request_prompt_redraw();
  prompts_.dirty_visible = true;
  prompts_.dirty_previous_focus = state_.surface.focus;
  prompts_.dirty.kind = DirtyPromptState::Kind::CloseTab;
  prompts_.dirty.tab_index = index;
  prompts_.dirty.target_tabs = {index};
  prompts_.dirty.dirty_tabs = {index};
  prompts_.dirty.dirty_count = 1;
  prompts_.dirty.path.clear();
  prompts_.dirty.selected_action = 0;
  state_.surface.focus = FocusTarget::Overlay;
  operations_.request_prompt_redraw();
}

void PromptSurfaceService::ShowDirtyPromptForTabs(std::vector<std::size_t> target_tabs,
                                                  std::vector<std::size_t> dirty_tabs) {
  if (target_tabs.empty() || dirty_tabs.empty()) {
    return;
  }

  operations_.request_prompt_redraw();
  prompts_.dirty_visible = true;
  prompts_.dirty_previous_focus = state_.surface.focus;
  prompts_.dirty.kind = DirtyPromptState::Kind::CloseTabs;
  prompts_.dirty.tab_index = target_tabs.front();
  prompts_.dirty.target_tabs = std::move(target_tabs);
  prompts_.dirty.dirty_tabs = std::move(dirty_tabs);
  prompts_.dirty.dirty_count = prompts_.dirty.dirty_tabs.size();
  prompts_.dirty.path.clear();
  prompts_.dirty.selected_action = 0;
  state_.surface.focus = FocusTarget::Overlay;
  operations_.request_prompt_redraw();
}

void PromptSurfaceService::ShowDirtyPromptForProject(std::size_t index,
                                                     std::vector<std::size_t> dirty_tabs,
                                                     std::size_t dirty_count) {
  if (dirty_tabs.empty()) {
    return;
  }

  operations_.request_prompt_redraw();
  prompts_.dirty_visible = true;
  prompts_.dirty_previous_focus = state_.surface.focus;
  prompts_.dirty.kind = DirtyPromptState::Kind::CloseProject;
  prompts_.dirty.tab_index = dirty_tabs.front();
  prompts_.dirty.project_index = index;
  prompts_.dirty.target_tabs = dirty_tabs;
  prompts_.dirty.dirty_tabs = std::move(dirty_tabs);
  prompts_.dirty.dirty_count = dirty_count;
  prompts_.dirty.path.clear();
  prompts_.dirty.selected_action = 0;
  state_.surface.focus = FocusTarget::Overlay;
  operations_.request_prompt_redraw();
}

void PromptSurfaceService::ShowDirtyPromptForQuit(std::size_t active_tab_index,
                                                  std::size_t active_project_index,
                                                  std::vector<std::size_t> dirty_tabs,
                                                  std::size_t dirty_count) {
  operations_.request_prompt_redraw();
  prompts_.dirty_visible = true;
  prompts_.dirty_previous_focus = state_.surface.focus;
  prompts_.dirty.kind = DirtyPromptState::Kind::Quit;
  prompts_.dirty.tab_index = active_tab_index;
  prompts_.dirty.project_index = active_project_index;
  prompts_.dirty.dirty_tabs = std::move(dirty_tabs);
  prompts_.dirty.dirty_count = dirty_count;
  prompts_.dirty.path.clear();
  prompts_.dirty.selected_action = 0;
  state_.surface.focus = FocusTarget::Overlay;
  operations_.request_prompt_redraw();
}

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
