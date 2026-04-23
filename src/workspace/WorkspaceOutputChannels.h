#pragma once

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
  };
  const ParsedEntry* ParsedEntryAt(std::string_view id, std::size_t index) const;
  const editor::HighlightedLine* HighlightedContextSnippet(std::string_view id,
                                                           std::size_t index,
                                                           const std::filesystem::path& resolved_path) const;
  void Clear(std::string_view id);

 private:
  struct Channel {
    std::string label;
    std::vector<std::string> entries;
    std::vector<ParsedEntry> parsed_entries;
    std::optional<std::filesystem::path> current_reference_path;
  };

  void MarkDirty();

  std::map<std::string, Channel, std::less<>> channels_;
  mutable bool channel_infos_dirty_ = true;
  mutable std::vector<ChannelInfo> channel_infos_;
};

}  // namespace microide::workspace
