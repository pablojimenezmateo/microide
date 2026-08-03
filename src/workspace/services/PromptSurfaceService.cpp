#include "workspace/services/PromptSurfaceService.h"

#include <cstdint>
#include <utility>
#include <vector>

#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

PromptSurfaceService::PromptSurfaceService(ProjectWorkspaceState& state,
                                           PromptState& prompts,
                                           Operations operations)
    : state_(state),
      prompts_(prompts),
      operations_(std::move(operations)) {}

std::uint64_t PromptSurfaceService::EnsureFocusedTabStableId(std::size_t index) {
  auto& tabs = state_.focused_group().open_tabs;
  if (index >= tabs.size()) {
    return 0;
  }
  TabEntry& tab = tabs[index];
  if (tab.stable_id == 0) {
    tab.stable_id = state_.next_tab_stable_id++;
  }
  return tab.stable_id;
}

std::vector<std::uint64_t> PromptSurfaceService::StableIdsForFocusedTabs(
    const std::vector<std::size_t>& indices) {
  std::vector<std::uint64_t> ids;
  ids.reserve(indices.size());
  for (std::size_t index : indices) {
    const std::uint64_t id = EnsureFocusedTabStableId(index);
    if (id != 0) {
      ids.push_back(id);
    }
  }
  return ids;
}

void PromptSurfaceService::ShowDirtyPromptForTab(std::size_t index) {
  if (index >= state_.focused_group().open_tabs.size()) {
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
  prompts_.dirty.tab_id = EnsureFocusedTabStableId(index);
  prompts_.dirty.target_tab_ids =
      prompts_.dirty.tab_id != 0 ? std::vector<std::uint64_t>{prompts_.dirty.tab_id}
                                 : std::vector<std::uint64_t>{};
  prompts_.dirty.dirty_tab_ids = prompts_.dirty.target_tab_ids;
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
  prompts_.dirty.target_tab_ids = StableIdsForFocusedTabs(prompts_.dirty.target_tabs);
  prompts_.dirty.dirty_tab_ids = StableIdsForFocusedTabs(prompts_.dirty.dirty_tabs);
  prompts_.dirty.tab_id =
      prompts_.dirty.target_tab_ids.empty() ? 0 : prompts_.dirty.target_tab_ids.front();
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
  // CloseProject re-reads the live dirty set at confirm time (not stored indices/ids),
  // so clear any stale ids from a prior prompt.
  prompts_.dirty.tab_id = 0;
  prompts_.dirty.target_tab_ids.clear();
  prompts_.dirty.dirty_tab_ids.clear();
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
  // Quit re-reads the live dirty set per project at confirm time; clear stale ids.
  prompts_.dirty.tab_id = 0;
  prompts_.dirty.target_tab_ids.clear();
  prompts_.dirty.dirty_tab_ids.clear();
  prompts_.dirty.path.clear();
  prompts_.dirty.selected_action = 0;
  state_.surface.focus = FocusTarget::Overlay;
  operations_.request_prompt_redraw();
}

void PromptSurfaceService::ShowDirtyPathPrompt(DirtyPromptState::Kind kind,
                                               std::vector<std::size_t> dirty_tabs,
                                               std::size_t dirty_count,
                                               const std::filesystem::path& path) {
  operations_.request_prompt_redraw();
  prompts_.dirty_visible = true;
  prompts_.dirty_previous_focus = state_.surface.focus;
  prompts_.dirty.kind = kind;
  prompts_.dirty.dirty_tabs = std::move(dirty_tabs);
  prompts_.dirty.dirty_count = dirty_count;
  prompts_.dirty.path = path.lexically_normal();
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
  prompts_.surface.target_line = 0;
  prompts_.surface.input.SetText(std::move(input));
  prompts_.surface.detail.clear();
  prompts_.surface.provider_id.clear();
  prompts_.surface.request_id.clear();
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
  prompts_.surface.input.SetText({});
  prompts_.surface.detail = std::move(url);
  prompts_.surface.provider_id.clear();
  prompts_.surface.request_id.clear();
  prompts_.surface.tool_call_id.clear();
  prompts_.surface.tool_id.clear();
  prompts_.surface.capability_scope.clear();
  prompts_.surface.button_count = 2;
  prompts_.surface.selected_button = 0;
  state_.surface.focus = FocusTarget::Overlay;
  operations_.request_prompt_redraw();
}

}  // namespace microide::workspace
