#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/RuntimeSyntaxRegistry.h"

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
  struct ParsedEntry {
    enum class Kind {
      Plain,
      ReferencePath,
      ContextSnippet,
    };

    Kind kind = Kind::Plain;
    std::filesystem::path reference_path;
    std::string prefix;
    std::string code;
    mutable std::filesystem::path highlighted_path;
    mutable std::optional<editor::HighlightedLine> highlighted_code;
    mutable std::filesystem::path resolved_reference_path;
    mutable std::filesystem::path resolved_for_root;
    mutable bool resolved_reference_valid = false;
  };
  const ParsedEntry* ParsedEntryAt(std::string_view id, std::size_t index) const;
  std::size_t RetainedBytes(std::string_view id) const;
  const std::filesystem::path& ResolvedReferencePath(
      std::string_view id, std::size_t index, const std::filesystem::path& project_root) const;
  const editor::HighlightedLine* HighlightedContextSnippet(std::string_view id,
                                                           std::size_t index,
                                                           const std::filesystem::path& resolved_path) const;
  void Clear(std::string_view id);
  std::uint64_t FirstEntrySerial(std::string_view id) const;
  void RemoveChannel(std::string_view id);

  static constexpr std::size_t kMaxOutputChannels = 64;
  std::size_t EvictedChannelCount() const { return evicted_channel_count_; }

 private:
  struct Channel {
    std::string label;
    std::vector<std::string> entries;
    std::vector<ParsedEntry> parsed_entries;
    std::size_t retained_bytes = 0;
    std::optional<std::filesystem::path> current_reference_path;
    std::uint64_t first_serial = 0;
    std::uint64_t last_touch = 0;
  };

  void MarkDirty();
  void TouchChannel(Channel& channel);
  void EvictLeastRecentlyUsedChannelIfNeeded(std::string_view keep_id);

  std::map<std::string, Channel, std::less<>> channels_;
  std::uint64_t touch_counter_ = 0;
  std::uint64_t next_entry_serial_ = 1;
  std::size_t evicted_channel_count_ = 0;
  mutable bool channel_infos_dirty_ = true;
  mutable std::vector<ChannelInfo> channel_infos_;
};

}
