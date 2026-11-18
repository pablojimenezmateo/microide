#include "workspace/persistence/RoamingSession.h"

#include <algorithm>
#include <utility>

#include "util/JsonValue.h"
#include "util/PathMatch.h"

namespace microide::workspace {
namespace {

constexpr std::int64_t kFormatVersion = 1;

// `path` as a root-relative '/'-separated path; empty when it is not under `root`.
std::string RelativeUnder(const std::filesystem::path& root, const std::filesystem::path& path) {
  if (path.empty() || root.empty() || path.is_relative()) {
    return {};
  }
  const std::filesystem::path relative = path.lexically_normal().lexically_relative(root.lexically_normal());
  if (relative.empty() || relative == std::filesystem::path(".") || util::RelativePathClimbsOut(relative)) {
    return {};
  }
  return relative.generic_string();
}

// A path another machine named: relative, normal, and under the root by
// construction -- no "..", no ".", no empty component, nothing absolute.
bool IsPortablePath(std::string_view path) {
  if (path.empty() || path.size() > kMaxRoamingPathBytes || path.front() == '/' ||
      path.find('\0') != std::string_view::npos) {
    return false;
  }
  while (!path.empty()) {
    const std::size_t slash = path.find('/');
    const std::string_view component = path.substr(0, slash);
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
    path.remove_prefix(slash == std::string_view::npos ? path.size() : slash + 1);
    if (slash != std::string_view::npos && path.empty()) {
      return false;  // a trailing slash
    }
  }
  return true;
}

util::JsonValue FilesJson(const RoamingSession& roaming) {
  util::JsonArray files;
  files.reserve(roaming.files.size());
  for (const RoamingSession::File& file : roaming.files) {
    util::JsonObject row;
    row["path"] = util::JsonValue(file.path);
    row["line"] = util::JsonValue(static_cast<std::int64_t>(file.line));
    row["column"] = util::JsonValue(static_cast<std::int64_t>(file.column));
    if (file.active) {
      row["active"] = util::JsonValue(true);
    }
    files.push_back(util::JsonValue(std::move(row)));
  }
  return util::JsonValue(std::move(files));
}

std::size_t Count(const util::JsonValue& value) {
  constexpr std::int64_t kMax = 100'000'000;  // a line or column past this is no position
  return value.IsInt() && value.AsInt() >= 0 && value.AsInt() <= kMax ? static_cast<std::size_t>(value.AsInt()) : 0;
}

}  // namespace

RoamingSession CaptureRoamingSession(const PersistedProjectSessionState& session,
                                     const std::filesystem::path& root, std::string client,
                                     std::uint64_t saved_at) {
  RoamingSession roaming;
  roaming.client = std::move(client);
  roaming.saved_at = saved_at;
  // The focused group first: its active tab is the one that was in front.
  std::vector<std::size_t> order;
  if (session.focused_group_index < session.groups.size()) {
    order.push_back(session.focused_group_index);
  }
  for (std::size_t i = 0; i < session.groups.size(); ++i) {
    if (i != session.focused_group_index) {
      order.push_back(i);
    }
  }
  for (const std::size_t group_index : order) {
    const PersistedEditorGroupState& group = session.groups[group_index];
    for (std::size_t i = 0; i < group.tabs.size() && roaming.files.size() < kMaxRoamingFiles; ++i) {
      const PersistedEditorTabState& tab = group.tabs[i];
      std::string relative = tab.kind == "editor" ? RelativeUnder(root, tab.path) : std::string();
      if (relative.empty() || relative.size() > kMaxRoamingPathBytes) {
        continue;
      }
      const bool active = group_index == session.focused_group_index && i == group.active_tab_index;
      // A file open in two groups is one file to open.
      const auto seen = std::find_if(roaming.files.begin(), roaming.files.end(),
                                     [&](const RoamingSession::File& file) { return file.path == relative; });
      if (seen != roaming.files.end()) {
        continue;
      }
      roaming.files.push_back(RoamingSession::File{.path = std::move(relative),
                                                   .line = tab.cursor_line,
                                                   .column = tab.cursor_column,
                                                   .active = active});
    }
  }
  return roaming;
}

std::string EncodeRoamingSession(const RoamingSession& roaming) {
  util::JsonObject root;
  root["version"] = util::JsonValue(kFormatVersion);
  root["client"] = util::JsonValue(roaming.client);
  root["saved_at"] = util::JsonValue(static_cast<std::int64_t>(roaming.saved_at));
  root["files"] = FilesJson(roaming);
  return util::SerializeJson(util::JsonValue(std::move(root)));
}

std::string RoamingSessionFingerprint(const RoamingSession& roaming) {
  return util::SerializeJson(FilesJson(roaming));
}

std::optional<RoamingSession> DecodeRoamingSession(std::string_view text) {
  const std::optional<util::JsonValue> parsed = util::ParseJson(text);
  if (!parsed.has_value() || !parsed->IsObject() || !(*parsed)["version"].IsInt() ||
      (*parsed)["version"].AsInt() != kFormatVersion) {
    return std::nullopt;
  }
  const util::JsonValue& client = (*parsed)["client"];
  const util::JsonValue& saved_at = (*parsed)["saved_at"];
  if (!client.IsString() || client.AsString().size() > kMaxRoamingClientBytes || !saved_at.IsInt() ||
      saved_at.AsInt() < 0) {
    return std::nullopt;
  }
  RoamingSession roaming;
  roaming.client = client.AsString();
  roaming.saved_at = static_cast<std::uint64_t>(saved_at.AsInt());
  bool has_active = false;
  if (const util::JsonValue& files = (*parsed)["files"]; files.IsArray()) {
    for (const util::JsonValue& row : files.AsArray()) {
      if (roaming.files.size() >= kMaxRoamingFiles) {
        break;
      }
      const util::JsonValue& path = row["path"];
      if (!row.IsObject() || !path.IsString() || !IsPortablePath(path.AsString())) {
        continue;  // a row that names nothing under the root is dropped, not trusted
      }
      const bool active = !has_active && row["active"].IsBool() && row["active"].AsBool();
      has_active = has_active || active;
      roaming.files.push_back(RoamingSession::File{.path = path.AsString(),
                                                   .line = Count(row["line"]),
                                                   .column = Count(row["column"]),
                                                   .active = active});
    }
  }
  return roaming;
}

}  // namespace microide::workspace
