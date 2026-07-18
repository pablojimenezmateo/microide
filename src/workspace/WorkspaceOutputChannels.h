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
  };
  const ParsedEntry* ParsedEntryAt(std::string_view id, std::size_t index) const;
  const editor::HighlightedLine* HighlightedContextSnippet(std::string_view id,
                                                           std::size_t index,
                                                           const std::filesystem::path& resolved_path) const;
  void Clear(std::string_view id);
  // Drop a channel entirely (entries + parsed cache). No-op if the id is unknown.
  // Used when a debug session is pruned so its console does not linger (Phase 10).
  void RemoveChannel(std::string_view id);

  // TD-2026-07-17A-116: each channel is per-channel byte/entry capped, but the channel
  // *set* was unbounded. Failed/crashed/launch-rejected debug sessions intentionally keep
  // a unique `debug.console.<id>` channel, so repeated failing launches could accumulate
  // many 16 MiB consoles and open output-tab ids. Cap the live channel count and evict the
  // least-recently-touched channel when a new one crosses the cap, preserving the most
  // recently active/failed consoles. Count of channels dropped this way (test/telemetry).
  static constexpr std::size_t kMaxOutputChannels = 64;
  std::size_t EvictedChannelCount() const { return evicted_channel_count_; }

 private:
  struct Channel {
    std::string label;
    std::vector<std::string> entries;
    std::vector<ParsedEntry> parsed_entries;
    std::size_t retained_bytes = 0;
    std::optional<std::filesystem::path> current_reference_path;
    // Monotonic last-touch stamp (bumped on EnsureChannel/AppendLine) driving LRU
    // eviction when the global channel count cap is crossed.
    std::uint64_t last_touch = 0;
  };

  void MarkDirty();
  // Bump a channel's LRU stamp and, when creating pushed the set over the cap, evict the
  // least-recently-touched OTHER channel.
  void TouchChannel(Channel& channel);
  void EvictLeastRecentlyUsedChannelIfNeeded(std::string_view keep_id);

  std::map<std::string, Channel, std::less<>> channels_;
  std::uint64_t touch_counter_ = 0;
  std::size_t evicted_channel_count_ = 0;
  mutable bool channel_infos_dirty_ = true;
  mutable std::vector<ChannelInfo> channel_infos_;
};

}  // namespace microide::workspace
