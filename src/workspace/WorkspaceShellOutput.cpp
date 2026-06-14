#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <string>

#include "workspace/TabReorder.h"

namespace microide::workspace {

const std::vector<WorkspaceOutputChannels::ChannelInfo>& WorkspaceShell::OutputChannels() const {
  return output_channels_.Channels();
}

const std::vector<std::string>* WorkspaceShell::OutputChannelEntries(std::string_view id) const {
  return output_channels_.Entries(id);
}

void WorkspaceShell::EnsureOutputChannelTabOpen(std::string_view channel_id) {
  if (channel_id.empty()) {
    return;
  }
  auto& tabs = context_.current_project_state.panel.output.open_channel_ids;
  if (std::find(tabs.begin(), tabs.end(), channel_id) == tabs.end()) {
    tabs.emplace_back(channel_id);
  }
}

bool WorkspaceShell::MoveActiveOutputTabTo(std::size_t index) {
  auto& tabs = context_.current_project_state.panel.output.open_channel_ids;
  const std::string& active_id = context_.current_project_state.panel.output.channel_id;
  if (active_id.empty()) {
    return false;
  }
  // Dragging an output tab pins it: ensure the active channel is in the open
  // list so reordering has a stable target, then move it within the list.
  if (std::find(tabs.begin(), tabs.end(), active_id) == tabs.end()) {
    tabs.push_back(active_id);
  }
  const auto it = std::find(tabs.begin(), tabs.end(), active_id);
  std::size_t active_index = static_cast<std::size_t>(it - tabs.begin());
  return ReorderActive(tabs, active_index, index);
}

void WorkspaceShell::CloseOutputChannelTab(std::string_view channel_id) {
  auto& tabs = context_.current_project_state.panel.output.open_channel_ids;
  const auto it = std::find(tabs.begin(), tabs.end(), channel_id);
  if (it == tabs.end()) {
    return;
  }
  const std::size_t closed_index = static_cast<std::size_t>(std::distance(tabs.begin(), it));
  tabs.erase(it);

  if (context_.current_project_state.panel.content != PanelContentKind::Output ||
      context_.current_project_state.panel.output.channel_id != channel_id) {
    return;
  }

  if (!tabs.empty()) {
    const std::size_t next_index = std::min(closed_index, tabs.size() - 1);
    context_.current_project_state.panel.output.channel_id = tabs[next_index];
    return;
  }

  if (ActiveTerminalTab() != nullptr) {
    context_.current_project_state.panel.content = PanelContentKind::Terminal;
    return;
  }

  context_.current_project_state.panel.content = PanelContentKind::None;
  if (context_.current_project_state.surface.focus == FocusTarget::Panel) {
    context_.current_project_state.surface.focus = FocusTarget::Editor;
  }
}

}  // namespace microide::workspace
