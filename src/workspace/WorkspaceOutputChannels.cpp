#include "workspace/WorkspaceOutputChannels.h"

#include <utility>

namespace microide::workspace {

void WorkspaceOutputChannels::EnsureChannel(std::string_view id, std::string_view label) {
  if (id.empty()) {
    return;
  }

  Channel channel;
  channel.label = std::string(label);
  auto [it, inserted] = channels_.try_emplace(std::string(id), std::move(channel));
  if (!inserted && it->second.label != label) {
    it->second.label = std::string(label);
  }
  MarkDirty();
}

void WorkspaceOutputChannels::AppendLine(std::string_view id, std::string_view label, std::string line) {
  if (id.empty()) {
    return;
  }

  EnsureChannel(id, label);
  channels_.find(id)->second.entries.push_back(std::move(line));
}

const std::vector<WorkspaceOutputChannels::ChannelInfo>& WorkspaceOutputChannels::Channels() const {
  if (!channel_infos_dirty_) {
    return channel_infos_;
  }

  channel_infos_.clear();
  channel_infos_.reserve(channels_.size());
  for (const auto& [id, channel] : channels_) {
    channel_infos_.push_back(ChannelInfo{
        .id = id,
        .label = channel.label,
    });
  }
  channel_infos_dirty_ = false;
  return channel_infos_;
}

const std::vector<std::string>* WorkspaceOutputChannels::Entries(std::string_view id) const {
  const auto it = channels_.find(id);
  return it == channels_.end() ? nullptr : &it->second.entries;
}

void WorkspaceOutputChannels::Clear(std::string_view id) {
  const auto it = channels_.find(id);
  if (it == channels_.end()) {
    return;
  }
  it->second.entries.clear();
}

void WorkspaceOutputChannels::MarkDirty() {
  channel_infos_dirty_ = true;
}

}  // namespace microide::workspace
