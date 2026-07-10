#include "workspace/WorkspaceOutputChannels.h"

#include <filesystem>
#include <string_view>
#include <utility>

#include "util/Parse.h"
#include "workspace/WorkspaceOutputReference.h"

namespace microide::workspace {

namespace {
// Per-channel retained-entry ceiling. A long-running debug/LSP session or a
// hostile adapter would otherwise grow entries/parsed_entries without bound.
constexpr std::size_t kMaxChannelEntries = 100000;

// Per-line and per-channel byte ceilings. The entry cap alone still allows a
// stream of large-but-valid adapter/plugin output lines to retain many GiB over
// time. Keep the newest output, truncate individual pathological lines, and
// coalesce byte-budget trims the same way as entry-count trims.
constexpr std::size_t kMaxChannelLineBytes = 1u << 20;      // 1 MiB
constexpr std::size_t kMaxChannelRetainedBytes = 16u << 20;  // 16 MiB

void TruncateOutputLine(std::string& line) {
  if (line.size() <= kMaxChannelLineBytes) {
    return;
  }
  static constexpr std::string_view kSuffix = "... [truncated]";
  line.resize(kMaxChannelLineBytes - kSuffix.size());
  line.append(kSuffix);
}
}  // namespace

std::optional<OutputReference> ParseOutputReference(std::string_view text) {
  const std::size_t column_delimiter = text.rfind(':');
  if (column_delimiter == std::string_view::npos || column_delimiter == 0) {
    return std::nullopt;
  }
  const std::size_t line_delimiter = text.rfind(':', column_delimiter - 1);
  if (line_delimiter == std::string_view::npos || line_delimiter == 0) {
    return std::nullopt;
  }

  const std::string_view path_text = text.substr(0, line_delimiter);
  const std::string_view line_text =
      text.substr(line_delimiter + 1, column_delimiter - line_delimiter - 1);
  const std::string_view column_text = text.substr(column_delimiter + 1);
  const auto parsed_line = util::ParseSize(line_text);
  const auto parsed_column = util::ParseSize(column_text);
  // OutputReference documents both line and column as 1-based, so a zero in
  // either field is malformed and must be rejected (not silently accepted as
  // column 0).
  if (path_text.empty() || !parsed_line.has_value() || !parsed_column.has_value() ||
      *parsed_line == 0 || *parsed_column == 0) {
    return std::nullopt;
  }
  return OutputReference{
      .path = std::filesystem::path(path_text),
      .line = *parsed_line,
      .column = *parsed_column,
  };
}

namespace {

bool ParseOutputContextSnippet(std::string_view text,
                               std::string_view* prefix,
                               std::string_view* code) {
  if (prefix == nullptr || code == nullptr || text.size() < 6) {
    return false;
  }
  if (!(text.starts_with(" > ") || text.starts_with("   "))) {
    return false;
  }
  const std::size_t divider = text.find("| ");
  if (divider == std::string_view::npos) {
    return false;
  }
  *prefix = text.substr(0, divider + 2);
  *code = divider + 2 < text.size() ? text.substr(divider + 2) : std::string_view{};
  return true;
}

WorkspaceOutputChannels::ParsedEntry BuildParsedEntry(
    std::string_view line,
    std::optional<std::filesystem::path>* current_reference_path) {
  WorkspaceOutputChannels::ParsedEntry parsed;
  if (line.empty()) {
    if (current_reference_path != nullptr) {
      current_reference_path->reset();
    }
    return parsed;
  }

  if (const auto reference = ParseOutputReference(line); reference.has_value()) {
    parsed.kind = WorkspaceOutputChannels::ParsedEntry::Kind::ReferencePath;
    parsed.reference_path = reference->path;
    if (current_reference_path != nullptr) {
      *current_reference_path = parsed.reference_path;
    }
    return parsed;
  }

  std::string_view prefix;
  std::string_view code;
  if (current_reference_path != nullptr && current_reference_path->has_value() &&
      ParseOutputContextSnippet(line, &prefix, &code)) {
    parsed.kind = WorkspaceOutputChannels::ParsedEntry::Kind::ContextSnippet;
    parsed.reference_path = **current_reference_path;
    parsed.prefix = std::string(prefix);
    parsed.code = std::string(code);
  }
  return parsed;
}

}  // namespace

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
  auto& channel = channels_.find(id)->second;
  TruncateOutputLine(line);
  channel.retained_bytes += line.size();
  channel.parsed_entries.push_back(BuildParsedEntry(line, &channel.current_reference_path));
  channel.entries.push_back(std::move(line));

  // Bound retained history: a chatty or hostile LSP / debug adapter / plugin can
  // stream output forever, and each entry retains a heap string plus a parsed
  // cache. Drop oldest, coalesced (only trim once 25 % over the cap, then down to
  // the cap) so the common append stays O(1) rather than erase-front per line.
  // entries and parsed_entries are kept in lockstep — renderers index them by the
  // same position, so both are trimmed together.
  const std::size_t entry_high_watermark = kMaxChannelEntries + kMaxChannelEntries / 4;
  const std::size_t byte_high_watermark =
      kMaxChannelRetainedBytes + kMaxChannelRetainedBytes / 4;
  if (channel.entries.size() > entry_high_watermark ||
      channel.retained_bytes > byte_high_watermark) {
    std::size_t drop = channel.entries.size() > kMaxChannelEntries
                           ? channel.entries.size() - kMaxChannelEntries
                           : 0;
    std::size_t projected_bytes = channel.retained_bytes;
    for (std::size_t i = 0; i < drop; ++i) {
      projected_bytes -= channel.entries[i].size();
    }
    while (drop < channel.entries.size() &&
           projected_bytes > kMaxChannelRetainedBytes) {
      projected_bytes -= channel.entries[drop].size();
      ++drop;
    }
    for (std::size_t i = 0; i < drop; ++i) {
      channel.retained_bytes -= channel.entries[i].size();
    }
    channel.entries.erase(channel.entries.begin(),
                          channel.entries.begin() + static_cast<std::ptrdiff_t>(drop));
    channel.parsed_entries.erase(channel.parsed_entries.begin(),
                                 channel.parsed_entries.begin() + static_cast<std::ptrdiff_t>(drop));
  }
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

const WorkspaceOutputChannels::ParsedEntry* WorkspaceOutputChannels::ParsedEntryAt(
    std::string_view id,
    std::size_t index) const {
  const auto it = channels_.find(id);
  if (it == channels_.end() || index >= it->second.parsed_entries.size()) {
    return nullptr;
  }
  return &it->second.parsed_entries[index];
}

const editor::HighlightedLine* WorkspaceOutputChannels::HighlightedContextSnippet(
    std::string_view id,
    std::size_t index,
    const std::filesystem::path& resolved_path) const {
  const auto it = channels_.find(id);
  if (it == channels_.end() || index >= it->second.parsed_entries.size()) {
    return nullptr;
  }
  ParsedEntry& parsed = const_cast<ParsedEntry&>(it->second.parsed_entries[index]);
  if (parsed.kind != ParsedEntry::Kind::ContextSnippet) {
    return nullptr;
  }
  if (!parsed.highlighted_code.has_value() || parsed.highlighted_path != resolved_path) {
    parsed.highlighted_path = resolved_path;
    parsed.highlighted_code =
        editor::runtime_syntax::HighlightLine(parsed.code, resolved_path, {}, parsed.code);
  }
  return &*parsed.highlighted_code;
}

void WorkspaceOutputChannels::Clear(std::string_view id) {
  const auto it = channels_.find(id);
  if (it == channels_.end()) {
    return;
  }
  it->second.entries.clear();
  it->second.parsed_entries.clear();
  it->second.retained_bytes = 0;
  it->second.current_reference_path.reset();
}

void WorkspaceOutputChannels::RemoveChannel(std::string_view id) {
  const auto it = channels_.find(id);
  if (it == channels_.end()) {
    return;
  }
  channels_.erase(it);
  MarkDirty();
}

void WorkspaceOutputChannels::MarkDirty() {
  channel_infos_dirty_ = true;
}

}  // namespace microide::workspace
