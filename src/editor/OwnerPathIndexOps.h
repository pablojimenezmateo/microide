#pragma once

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "editor/PathKey.h"
#include "util/PathMatch.h"

namespace microide::editor {

template <typename OwnerMap, typename Rebuild>
bool ClearOwnerEntries(OwnerMap& by_owner, std::string_view owner, Rebuild&& rebuild) {
  const auto owner_it = by_owner.find(owner);
  if (owner_it == by_owner.end()) {
    return false;
  }
  std::vector<std::string> path_keys;
  path_keys.reserve(owner_it->second.size());
  for (const auto& entry : owner_it->second) {
    path_keys.push_back(entry.first);
  }
  by_owner.erase(owner_it);
  for (const auto& path_key : path_keys) {
    rebuild(path_key);
  }
  return !path_keys.empty();
}

template <typename OwnerMap, typename Rebuild>
bool ClearOwnerFileEntry(OwnerMap& by_owner,
                         const std::string& owner_key,
                         const std::string& path_key,
                         Rebuild&& rebuild) {
  if (owner_key.empty() || path_key.empty()) {
    return false;
  }
  const auto owner_it = by_owner.find(owner_key);
  if (owner_it == by_owner.end()) {
    return false;
  }
  if (owner_it->second.erase(path_key) == 0) {
    return false;
  }
  if (owner_it->second.empty()) {
    by_owner.erase(owner_it);
  }
  rebuild(path_key);
  return true;
}

template <typename Rebuild>
void RebuildAffectedPathKeys(std::vector<std::string>& affected_path_keys, Rebuild&& rebuild) {
  std::sort(affected_path_keys.begin(), affected_path_keys.end());
  affected_path_keys.erase(std::unique(affected_path_keys.begin(), affected_path_keys.end()),
                           affected_path_keys.end());
  for (const auto& path_key : affected_path_keys) {
    rebuild(path_key);
  }
}

template <typename OwnerMap, typename Rebuild>
bool ClearEntriesUnderPrefix(OwnerMap& by_owner,
                             const std::filesystem::path& path_prefix,
                             Rebuild&& rebuild) {
  const std::filesystem::path normalized_prefix = path_prefix.lexically_normal();
  if (normalized_prefix.empty()) {
    return false;
  }
  bool changed = false;
  std::vector<std::string> affected_path_keys;
  for (auto owner_it = by_owner.begin(); owner_it != by_owner.end();) {
    auto& owner_entries = owner_it->second;
    for (auto path_it = owner_entries.begin(); path_it != owner_entries.end();) {
      if (!util::PathEqualsOrWithin(path_it->second.path, normalized_prefix)) {
        ++path_it;
        continue;
      }
      affected_path_keys.push_back(path_it->first);
      path_it = owner_entries.erase(path_it);
      changed = true;
    }
    if (owner_entries.empty()) {
      owner_it = by_owner.erase(owner_it);
    } else {
      ++owner_it;
    }
  }
  RebuildAffectedPathKeys(affected_path_keys, rebuild);
  return changed;
}

template <typename OwnerMap, typename Rewrite, typename Rebuild>
bool RetargetEntriesUnderPrefix(OwnerMap& by_owner,
                                const std::filesystem::path& old_prefix,
                                const std::filesystem::path& new_prefix,
                                Rewrite&& rewrite,
                                Rebuild&& rebuild) {
  const std::filesystem::path normalized_old = old_prefix.lexically_normal();
  const std::filesystem::path normalized_new = new_prefix.lexically_normal();
  if (normalized_old.empty() || normalized_new.empty() || normalized_old == normalized_new) {
    return false;
  }

  using Entry = typename OwnerMap::mapped_type::mapped_type;
  bool changed = false;
  std::vector<std::string> affected_path_keys;
  for (auto owner_it = by_owner.begin(); owner_it != by_owner.end();) {
    auto& owner_entries = owner_it->second;
    std::vector<std::string> old_keys;
    std::vector<std::pair<std::string, Entry>> replacements;

    for (const auto& [path_key, entry] : owner_entries) {
      if (!util::PathEqualsOrWithin(entry.path, normalized_old)) {
        continue;
      }
      Entry moved = entry;
      moved.path = util::ReplacePathPrefix(entry.path, normalized_old, normalized_new);
      rewrite(moved, normalized_old, normalized_new);
      std::string moved_key = NormalizedPathKey(moved.path);
      if (moved_key.empty()) {
        continue;
      }
      old_keys.push_back(path_key);
      affected_path_keys.push_back(path_key);
      affected_path_keys.push_back(moved_key);
      replacements.emplace_back(std::move(moved_key), std::move(moved));
      changed = true;
    }

    for (const auto& old_key : old_keys) {
      owner_entries.erase(old_key);
    }
    for (auto& [new_key, moved] : replacements) {
      owner_entries[new_key] = std::move(moved);
    }

    if (owner_entries.empty()) {
      owner_it = by_owner.erase(owner_it);
    } else {
      ++owner_it;
    }
  }
  RebuildAffectedPathKeys(affected_path_keys, rebuild);
  return changed;
}

}
