#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace microide::workspace {

class WorkspaceOutputChannels {
 public:
  struct ChannelInfo {
    std::string id;
    std::string label;
  };

  void EnsureChannel(std::string_view id, std::string_view label);
  void AppendLine(std::string_view id, std::string_view label, std::string line);
  const std::vector<ChannelInfo>& Channels() const;
  const std::vector<std::string>* Entries(std::string_view id) const;
  void Clear(std::string_view id);

 private:
  struct Channel {
    std::string label;
    std::vector<std::string> entries;
  };

  void MarkDirty();

  std::map<std::string, Channel, std::less<>> channels_;
  mutable bool channel_infos_dirty_ = true;
  mutable std::vector<ChannelInfo> channel_infos_;
};

}  // namespace microide::workspace
