#include "workspace/persistence/RoamingReview.h"

#include <algorithm>
#include <cstdint>
#include <utility>

#include "compare/BranchReviewStateService.h"
#include "util/JsonValue.h"
#include "util/Parse.h"
#include "util/PathMatch.h"
#include "workspace/persistence/RoamingSession.h"

namespace microide::workspace {
namespace {

constexpr std::int64_t kFormatVersion = 1;
constexpr std::size_t kMaxRefBytes = 512;
constexpr std::size_t kMaxNoteBytes = 16 * 1024;
using Limits = compare::BranchReviewStateService;

util::JsonValue Unsigned(std::uint64_t value) {
  return util::JsonValue(std::to_string(value));
}

std::optional<std::uint64_t> AsUnsigned(const util::JsonValue& value) {
  if (!value.IsString() || value.AsString().empty() || value.AsString().size() > 20) {
    return std::nullopt;
  }
  std::uint64_t result = 0;
  for (const char c : value.AsString()) {
    if (c < '0' || c > '9' || result > (UINT64_MAX - static_cast<std::uint64_t>(c - '0')) / 10) {
      return std::nullopt;
    }
    result = result * 10 + static_cast<std::uint64_t>(c - '0');
  }
  return result;
}

std::uint64_t Time(const util::JsonValue& value) {
  return value.IsInt() && value.AsInt() > 0 ? static_cast<std::uint64_t>(value.AsInt()) : 0;
}

bool IsRefText(const util::JsonValue& value) {
  return value.IsString() && value.AsString().size() <= kMaxRefBytes &&
         std::ranges::none_of(value.AsString(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

std::optional<std::string> PortablePath(const std::filesystem::path& path) {
  std::string text = path.generic_string();
  return util::IsPortableRelativePath(text, kMaxRoamingPathBytes) ? std::optional<std::string>(std::move(text))
                                                                  : std::nullopt;
}

util::JsonValue HunkJson(const PersistedBranchReviewHunkIdentity& hunk) {
  util::JsonArray lines;
  for (const int line : {hunk.old_start, hunk.old_count, hunk.new_start, hunk.new_count}) {
    lines.push_back(util::JsonValue(static_cast<std::int64_t>(line)));
  }
  util::JsonObject row;
  row["lines"] = util::JsonValue(std::move(lines));
  row["hash"] = Unsigned(hunk.content_hash);
  return util::JsonValue(std::move(row));
}

std::optional<PersistedBranchReviewHunkIdentity> HunkFromJson(const util::JsonValue& json, std::string_view path) {
  const util::JsonValue& lines = json["lines"];
  const std::optional<std::uint64_t> hash = AsUnsigned(json["hash"]);
  if (!lines.IsArray() || lines.AsArray().size() != 4 || !hash.has_value()) {
    return std::nullopt;
  }
  int values[4] = {};
  for (std::size_t i = 0; i < 4; ++i) {
    const util::JsonValue& line = lines.AsArray()[i];
    if (!line.IsInt() || line.AsInt() < 0 || line.AsInt() > 100'000'000) {
      return std::nullopt;
    }
    values[i] = static_cast<int>(line.AsInt());
  }
  return PersistedBranchReviewHunkIdentity{.path = std::filesystem::path(path),
                                           .old_start = values[0],
                                           .old_count = values[1],
                                           .new_start = values[2],
                                           .new_count = values[3],
                                           .content_hash = *hash};
}

util::JsonValue TargetJson(const PersistedBranchReviewTarget& target) {
  util::JsonObject row;
  row["base"] = util::JsonValue(target.base_commit);
  row["head"] = util::JsonValue(target.head_commit);
  row["merge_base"] = util::JsonValue(target.merge_base_commit);
  row["changed_at"] = util::JsonValue(static_cast<std::int64_t>(target.last_accessed_unix_ms));
  util::JsonArray files;
  for (const PersistedBranchReviewFileEntry& file : target.reviewed_files) {
    if (const std::optional<std::string> path = PortablePath(file.path)) {
      util::JsonObject entry;
      entry["path"] = util::JsonValue(*path);
      entry["diff"] = Unsigned(file.reviewed_diff_fingerprint);
      entry["at"] = util::JsonValue(static_cast<std::int64_t>(file.reviewed_at_unix_ms));
      files.push_back(util::JsonValue(std::move(entry)));
    }
  }
  util::JsonArray hunks;
  for (const PersistedBranchReviewHunkEntry& hunk : target.reviewed_hunks) {
    if (const std::optional<std::string> path = PortablePath(hunk.identity.path)) {
      util::JsonObject entry;
      entry["path"] = util::JsonValue(*path);
      entry["hunk"] = HunkJson(hunk.identity);
      entry["at"] = util::JsonValue(static_cast<std::int64_t>(hunk.reviewed_at_unix_ms));
      hunks.push_back(util::JsonValue(std::move(entry)));
    }
  }
  util::JsonArray notes;
  for (const PersistedBranchReviewNote& note : target.notes) {
    if (const std::optional<std::string> path = PortablePath(note.path)) {
      util::JsonObject entry;
      entry["path"] = util::JsonValue(*path);
      if (note.hunk_identity.has_value()) {
        entry["hunk"] = HunkJson(*note.hunk_identity);
      }
      entry["text"] = util::JsonValue(note.text);
      entry["at"] = util::JsonValue(static_cast<std::int64_t>(note.updated_at_unix_ms));
      notes.push_back(util::JsonValue(std::move(entry)));
    }
  }
  row["files"] = util::JsonValue(std::move(files));
  row["hunks"] = util::JsonValue(std::move(hunks));
  row["notes"] = util::JsonValue(std::move(notes));
  return util::JsonValue(std::move(row));
}

std::optional<PersistedBranchReviewTarget> TargetFromJson(const util::JsonValue& json,
                                                         const std::filesystem::path& root) {
  if (!json.IsObject() || !IsRefText(json["base"]) || !IsRefText(json["head"]) || !IsRefText(json["merge_base"]) ||
      json["base"].AsString().empty() || json["head"].AsString().empty()) {
    return std::nullopt;
  }
  PersistedBranchReviewTarget target;
  target.repository_root = root;
  target.base_commit = json["base"].AsString();
  target.head_commit = json["head"].AsString();
  target.merge_base_commit = json["merge_base"].AsString();
  target.last_accessed_unix_ms = Time(json["changed_at"]);
  const auto path_of = [](const util::JsonValue& entry) -> std::optional<std::string> {
    const util::JsonValue& path = entry["path"];
    return entry.IsObject() && path.IsString() && util::IsPortableRelativePath(path.AsString(), kMaxRoamingPathBytes)
               ? std::optional<std::string>(path.AsString())
               : std::nullopt;
  };
  if (const util::JsonValue& files = json["files"]; files.IsArray()) {
    for (const util::JsonValue& entry : files.AsArray()) {
      const std::optional<std::string> path = path_of(entry);
      const std::optional<std::uint64_t> diff = path.has_value() ? AsUnsigned(entry["diff"]) : std::nullopt;
      if (diff.has_value() && target.reviewed_files.size() < Limits::kMaxFileEntriesPerTarget) {
        target.reviewed_files.push_back(PersistedBranchReviewFileEntry{
            .path = *path, .reviewed_diff_fingerprint = *diff, .reviewed_at_unix_ms = Time(entry["at"])});
      }
    }
  }
  if (const util::JsonValue& hunks = json["hunks"]; hunks.IsArray()) {
    for (const util::JsonValue& entry : hunks.AsArray()) {
      const std::optional<std::string> path = path_of(entry);
      std::optional<PersistedBranchReviewHunkIdentity> hunk =
          path.has_value() ? HunkFromJson(entry["hunk"], *path) : std::nullopt;
      if (hunk.has_value() && target.reviewed_hunks.size() < Limits::kMaxHunkEntriesPerTarget) {
        target.reviewed_hunks.push_back(
            PersistedBranchReviewHunkEntry{.identity = std::move(*hunk), .reviewed_at_unix_ms = Time(entry["at"])});
      }
    }
  }
  if (const util::JsonValue& notes = json["notes"]; notes.IsArray()) {
    for (const util::JsonValue& entry : notes.AsArray()) {
      const std::optional<std::string> path = path_of(entry);
      const util::JsonValue& text = path.has_value() ? entry["text"] : entry;
      if (!path.has_value() || !text.IsString() || text.AsString().empty() ||
          text.AsString().size() > kMaxNoteBytes || target.notes.size() >= Limits::kMaxNotesPerTarget) {
        continue;
      }
      PersistedBranchReviewNote note;
      note.path = *path;
      note.text = text.AsString();
      note.updated_at_unix_ms = Time(entry["at"]);
      note.scope = "file";
      if (!entry["hunk"].IsNull()) {
        note.hunk_identity = HunkFromJson(entry["hunk"], *path);
        if (!note.hunk_identity.has_value()) {
          continue;
        }
        note.scope = "hunk";
      }
      target.notes.push_back(std::move(note));
    }
  }
  return target;
}

bool SameTarget(const PersistedBranchReviewTarget& left, const PersistedBranchReviewTarget& right) {
  return left.repository_root == right.repository_root && left.base_commit == right.base_commit &&
         left.head_commit == right.head_commit && left.merge_base_commit == right.merge_base_commit;
}

}

std::string EncodeRoamingReview(const PersistedBranchReviewState& state, const std::filesystem::path& root,
                                std::size_t max_bytes) {
  std::vector<const PersistedBranchReviewTarget*> mine;
  for (const PersistedBranchReviewTarget& target : state.targets) {
    if (target.repository_root == root) {
      mine.push_back(&target);
    }
  }
  if (mine.empty()) {
    return {};
  }
  std::ranges::stable_sort(mine, [](const PersistedBranchReviewTarget* left, const PersistedBranchReviewTarget* right) {
    if (left->last_accessed_unix_ms != right->last_accessed_unix_ms) {
      return left->last_accessed_unix_ms > right->last_accessed_unix_ms;
    }
    return std::tie(left->base_commit, left->head_commit, left->merge_base_commit) <
           std::tie(right->base_commit, right->head_commit, right->merge_base_commit);
  });
  std::string text = "{\"version\":" + std::to_string(kFormatVersion) + ",\"targets\":[";
  bool any = false;
  for (const PersistedBranchReviewTarget* target : mine) {
    const std::string row = util::SerializeJson(TargetJson(*target));
    if (text.size() + row.size() + 3 > max_bytes) {
      break;
    }
    text += any ? "," : "";
    text += row;
    any = true;
  }
  text += "]}";
  return any ? text : std::string();
}

std::optional<std::vector<PersistedBranchReviewTarget>> DecodeRoamingReview(std::string_view text,
                                                                           const std::filesystem::path& root) {
  const std::optional<util::JsonValue> parsed = util::ParseJson(text);
  if (!parsed.has_value() || !parsed->IsObject() || !(*parsed)["version"].IsInt() ||
      (*parsed)["version"].AsInt() != kFormatVersion || !(*parsed)["targets"].IsArray()) {
    return std::nullopt;
  }
  std::vector<PersistedBranchReviewTarget> targets;
  for (const util::JsonValue& row : (*parsed)["targets"].AsArray()) {
    if (targets.size() >= Limits::kMaxTargetsPerRepository) {
      break;
    }
    std::optional<PersistedBranchReviewTarget> target = TargetFromJson(row, root);
    if (target.has_value() &&
        std::ranges::none_of(targets, [&](const auto& seen) { return SameTarget(seen, *target); })) {
      targets.push_back(std::move(*target));
    }
  }
  return targets;
}

bool MergeRoamingReview(std::vector<PersistedBranchReviewTarget> theirs, PersistedBranchReviewState* state) {
  bool changed = false;
  for (PersistedBranchReviewTarget& their : theirs) {
    const auto mine = std::ranges::find_if(state->targets, [&](const auto& held) { return SameTarget(held, their); });
    if (mine == state->targets.end()) {
      state->targets.push_back(std::move(their));
      changed = true;
    } else if (their.last_accessed_unix_ms > mine->last_accessed_unix_ms) {
      their.snapshot_generation = mine->snapshot_generation;
      *mine = std::move(their);
      changed = true;
    }
  }
  return changed;
}

}
