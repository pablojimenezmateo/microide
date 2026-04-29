#include "workspace/WorkspaceShell.h"

#include <algorithm>

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

void WorkspaceShell::ShowOutputChannel(std::string_view id) {
  const std::string channel_id =
      id.empty() ? (context_.current_project_state.panel.output.channel_id.empty()
                        ? std::string("plugins.log")
                        : context_.current_project_state.panel.output.channel_id)
                 : std::string(id);
  std::string channel_label = channel_id;
  for (const auto& channel : output_channels_.Channels()) {
    if (channel.id == channel_id) {
      channel_label = channel.label.empty() ? channel.id : channel.label;
      break;
    }
  }
  output_channels_.EnsureChannel(channel_id, channel_label);
  EnsureOutputChannelTabOpen(channel_id);
  context_.current_project_state.panel.content = PanelContentKind::Output;
  context_.current_project_state.panel.output.channel_id = channel_id;
  context_.current_project_state.surface.focus = FocusTarget::Panel;
  RequestBottomPanelRedraw();
}

}  // namespace microide::workspace
