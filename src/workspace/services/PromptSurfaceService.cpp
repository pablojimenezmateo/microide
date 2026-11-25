#include "workspace/services/PromptSurfaceService.h"

#include <algorithm>
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
  return EnsureTabStableId(tabs[index]);
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

void PromptSurfaceService::ShowDirtyPrompt(DirtyPromptState prompt) {
  operations_.request_prompt_redraw();
  prompts_.dirty_visible = true;
  prompts_.dirty_previous_focus = state_.surface.focus;
  prompts_.dirty = std::move(prompt);
  state_.surface.focus = FocusTarget::Overlay;
  operations_.request_prompt_redraw();
}

void PromptSurfaceService::ShowDirtyPromptForTab(std::size_t index) {
  if (index >= state_.focused_group().open_tabs.size()) {
    return;
  }
  const std::uint64_t tab_id = EnsureFocusedTabStableId(index);
  std::vector<std::uint64_t> tab_ids =
      tab_id != 0 ? std::vector<std::uint64_t>{tab_id} : std::vector<std::uint64_t>{};
  ShowDirtyPrompt(DirtyPromptState{
      .kind = DirtyPromptState::Kind::CloseTab,
      .tab_index = index,
      .target_tabs = {index},
      .dirty_tabs = {index},
      .dirty_count = 1,
      .tab_id = tab_id,
      .target_tab_ids = tab_ids,
      .dirty_tab_ids = std::move(tab_ids),
  });
}

void PromptSurfaceService::ShowDirtyPromptForTabs(std::vector<std::size_t> target_tabs,
                                                  std::vector<std::size_t> dirty_tabs) {
  if (target_tabs.empty() || dirty_tabs.empty()) {
    return;
  }
  std::vector<std::uint64_t> target_tab_ids = StableIdsForFocusedTabs(target_tabs);
  std::vector<std::uint64_t> dirty_tab_ids = StableIdsForFocusedTabs(dirty_tabs);
  const std::uint64_t tab_id = target_tab_ids.empty() ? 0 : target_tab_ids.front();
  const std::size_t tab_index = target_tabs.front();
  const std::size_t dirty_count = dirty_tabs.size();
  ShowDirtyPrompt(DirtyPromptState{
      .kind = DirtyPromptState::Kind::CloseTabs,
      .tab_index = tab_index,
      .target_tabs = std::move(target_tabs),
      .dirty_tabs = std::move(dirty_tabs),
      .dirty_count = dirty_count,
      .tab_id = tab_id,
      .target_tab_ids = std::move(target_tab_ids),
      .dirty_tab_ids = std::move(dirty_tab_ids),
  });
}

void PromptSurfaceService::ShowDirtyPromptForProject(std::size_t index,
                                                     std::vector<std::size_t> dirty_tabs,
                                                     std::size_t dirty_count) {
  if (dirty_tabs.empty()) {
    return;
  }
  const std::size_t tab_index = dirty_tabs.front();
  std::vector<std::size_t> target_tabs = dirty_tabs;
  ShowDirtyPrompt(DirtyPromptState{
      .kind = DirtyPromptState::Kind::CloseProject,
      .tab_index = tab_index,
      .project_index = index,
      .target_tabs = std::move(target_tabs),
      .dirty_tabs = std::move(dirty_tabs),
      .dirty_count = dirty_count,
  });
}

void PromptSurfaceService::ShowDirtyPromptForQuit(std::size_t active_tab_index,
                                                  std::size_t active_project_index,
                                                  std::vector<std::size_t> dirty_tabs,
                                                  std::size_t dirty_count) {
  ShowDirtyPrompt(DirtyPromptState{
      .kind = DirtyPromptState::Kind::Quit,
      .tab_index = active_tab_index,
      .project_index = active_project_index,
      .dirty_tabs = std::move(dirty_tabs),
      .dirty_count = dirty_count,
  });
}

void PromptSurfaceService::ShowDirtyPathPrompt(DirtyPromptState::Kind kind,
                                               std::vector<std::size_t> dirty_tabs,
                                               std::size_t dirty_count,
                                               const std::filesystem::path& path) {
  ShowDirtyPrompt(DirtyPromptState{
      .kind = kind,
      .dirty_tabs = std::move(dirty_tabs),
      .dirty_count = dirty_count,
      .path = path.lexically_normal(),
  });
}

FocusTarget PromptSurfaceService::FocusAfterPrompt(FocusTarget previous) const {
  if (prompts_.dirty_visible || prompts_.surface_visible || state_.overlay.visible) {
    return FocusTarget::Overlay;
  }
  return previous == FocusTarget::Overlay ? FocusAfterOverlay(state_) : FocusBackTo(state_, previous);
}

void PromptSurfaceService::OnTabClosed(std::uint64_t stable_id) {
  if (!prompts_.dirty_visible || stable_id == 0) {
    return;
  }
  const DirtyPromptState& prompt = prompts_.dirty;
  if (prompt.kind != DirtyPromptState::Kind::CloseTab && prompt.kind != DirtyPromptState::Kind::CloseTabs) {
    return;
  }
  if (std::find(prompt.target_tab_ids.begin(), prompt.target_tab_ids.end(), stable_id) !=
      prompt.target_tab_ids.end()) {
    DismissDirtyPrompt(true);
  }
}

void PromptSurfaceService::DismissDirtyPrompt(bool restore_focus) {
  operations_.request_prompt_redraw();
  prompts_.dirty_visible = false;
  prompts_.dirty = DirtyPromptState{};
  if (restore_focus || state_.surface.focus == FocusTarget::Overlay) {
    state_.surface.focus = FocusAfterPrompt(prompts_.dirty_previous_focus);
  }
  operations_.request_prompt_redraw();
}

void PromptSurfaceService::DismissPromptSurface(bool restore_focus) {
  operations_.request_prompt_redraw();
  prompts_.surface_visible = false;
  prompts_.surface = PromptSurfaceState{};
  if (restore_focus || state_.surface.focus == FocusTarget::Overlay) {
    state_.surface.focus = FocusAfterPrompt(prompts_.surface_previous_focus);
  }
  operations_.request_prompt_redraw();
}

void PromptSurfaceService::CancelOpenPrompts() {
  if (prompts_.surface_visible) {
    DismissPromptSurface(true);
  }
  if (prompts_.dirty_visible) {
    DismissDirtyPrompt(true);
  }
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
  prompts_.surface.completions.clear();
  prompts_.surface.review_target = {};
  prompts_.surface.review_hunk.reset();
  prompts_.surface.review_replaced_hunk.reset();
  prompts_.surface.button_count = 2;
  prompts_.surface.selected_button = 0;
  state_.surface.focus = FocusTarget::Overlay;
  operations_.request_prompt_redraw();
}

void PromptSurfaceService::OpenReviewCommentPrompt(ReviewCommentPrompt prompt) {
  OpenPromptSurface(PromptSurfaceState::Action::ReviewComment, PromptSurfaceState::Kind::TextInput, prompt.path,
                    std::move(prompt.text));
  prompts_.surface.review_target = std::move(prompt.target);
  prompts_.surface.review_hunk = std::move(prompt.hunk);
  prompts_.surface.review_replaced_hunk = std::move(prompt.replaced_hunk);
  prompts_.surface.detail = std::move(prompt.detail);
}

void PromptSurfaceService::OpenConfirmCommandPrompt(std::string command_line, std::string question) {
  OpenPromptSurface(PromptSurfaceState::Action::ConfirmCommand, PromptSurfaceState::Kind::Confirm, {}, {});
  prompts_.surface.command_name = std::move(command_line);
  prompts_.surface.detail = std::move(question);
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

}
