#include "editor/PluginDecorationStore.h"

#include "editor/PathKey.h"
#include "util/PathMatch.h"

#include <algorithm>
#include <utility>

namespace microide::editor {

namespace {

using util::PathEqualsOrWithin;
using util::ReplacePathPrefix;

// Sort the four decoration vectors of `decorations` into the per-line render
// order the slice lookups rely on. Works on both PluginDecorationData (per-owner,
// sorted once at publish) and FileDecorations (the merged multi-owner view).
template <typename Decorations>
void SortDecorations(Decorations& decorations) {
  std::sort(decorations.text_styles.begin(), decorations.text_styles.end(),
            [](const TextStyleDecoration& a, const TextStyleDecoration& b) {
              if (a.line != b.line) return a.line < b.line;
              return a.start_column < b.start_column;
            });
  std::sort(decorations.gutter_marks.begin(), decorations.gutter_marks.end(),
            [](const GutterMarkDecoration& a, const GutterMarkDecoration& b) {
              if (a.line != b.line) return a.line < b.line;
              return a.priority > b.priority;  // highest priority first within a line
            });
  std::sort(decorations.inline_texts.begin(), decorations.inline_texts.end(),
            [](const InlineTextDecoration& a, const InlineTextDecoration& b) {
              if (a.line != b.line) return a.line < b.line;
              return a.anchor_column < b.anchor_column;
            });
  std::sort(decorations.code_lenses.begin(), decorations.code_lenses.end(),
            [](const CodeLensDecoration& a, const CodeLensDecoration& b) {
              return a.line < b.line;
            });
}

// Contiguous slice of a line-sorted vector whose elements have `.line == line`.
template <typename T, typename LineOf>
std::span<const T> SliceForLine(const std::vector<T>& sorted, std::uint32_t line, LineOf line_of) {
  const auto first = std::lower_bound(sorted.begin(), sorted.end(), line,
                                      [&](const T& a, std::uint32_t l) { return line_of(a) < l; });
  const auto last = std::upper_bound(first, sorted.end(), line,
                                     [&](std::uint32_t l, const T& a) { return l < line_of(a); });
  if (first == last) {
    return {};
  }
  return std::span<const T>(&*first, static_cast<std::size_t>(last - first));
}

}  // namespace

std::span<const TextStyleDecoration> FileDecorations::TextStylesForLine(std::uint32_t line) const {
  return SliceForLine(text_styles, line, [](const TextStyleDecoration& d) { return d.line; });
}

std::span<const GutterMarkDecoration> FileDecorations::GutterMarksForLine(
    std::uint32_t line) const {
  return SliceForLine(gutter_marks, line, [](const GutterMarkDecoration& d) { return d.line; });
}

std::span<const InlineTextDecoration> FileDecorations::InlineTextsForLine(
    std::uint32_t line) const {
  return SliceForLine(inline_texts, line, [](const InlineTextDecoration& d) { return d.line; });
}

std::span<const CodeLensDecoration> FileDecorations::CodeLensesForLine(std::uint32_t line) const {
  return SliceForLine(code_lenses, line, [](const CodeLensDecoration& d) { return d.line; });
}

std::string PluginDecorationStore::PathKey(const std::filesystem::path& path) {
  return NormalizedPathKey(path);
}

void PluginDecorationStore::RebuildPath(std::string_view path_key) {
  if (path_key.empty()) {
    return;
  }

  // Gather every owner contributing to this path. Per-owner data is already
  // sorted at publish time, so the common single-owner case rebuilds with one
  // copy and no sort; only genuine multi-owner overlap pays the merge + re-sort.
  const OwnerFileDecorations* sole = nullptr;
  std::vector<const PluginDecorationData*> contributors;
  for (const auto& owner_entry : by_owner_) {
    const auto it = owner_entry.second.find(path_key);
    if (it == owner_entry.second.end()) {
      continue;
    }
    contributors.push_back(&it->second.data);
    sole = &it->second;
  }

  if (contributors.empty()) {
    // Heterogeneous find avoids materializing a std::string key just to erase;
    // unordered_map has no transparent erase(key) overload before C++23.
    if (const auto it = merged_by_path_.find(path_key); it != merged_by_path_.end()) {
      merged_by_path_.erase(it);
    }
    return;
  }

  FileDecorations merged;
  if (contributors.size() == 1) {
    merged.path = sole->path;
    merged.text_styles = sole->data.text_styles;
    merged.gutter_marks = sole->data.gutter_marks;
    merged.inline_texts = sole->data.inline_texts;
    merged.code_lenses = sole->data.code_lenses;
  } else {
    std::size_t ts = 0, gm = 0, it = 0, cl = 0;
    for (const PluginDecorationData* data : contributors) {
      ts += data->text_styles.size();
      gm += data->gutter_marks.size();
      it += data->inline_texts.size();
      cl += data->code_lenses.size();
    }
    merged.text_styles.reserve(ts);
    merged.gutter_marks.reserve(gm);
    merged.inline_texts.reserve(it);
    merged.code_lenses.reserve(cl);
    for (const PluginDecorationData* data : contributors) {
      merged.text_styles.insert(merged.text_styles.end(), data->text_styles.begin(),
                                data->text_styles.end());
      merged.gutter_marks.insert(merged.gutter_marks.end(), data->gutter_marks.begin(),
                                 data->gutter_marks.end());
      merged.inline_texts.insert(merged.inline_texts.end(), data->inline_texts.begin(),
                                 data->inline_texts.end());
      merged.code_lenses.insert(merged.code_lenses.end(), data->code_lenses.begin(),
                                data->code_lenses.end());
    }
    merged.path = sole->path;
    SortDecorations(merged);

    // Per-owner input is capped (kMaxEntriesPerKind) at publish, but the merge sums
    // across every owner, so a pathological set of plugins could push a single
    // file's per-kind totals arbitrarily high and bloat render/slice work. Apply an
    // aggregate per-file cap. The vectors are already line-sorted above (gutter
    // marks additionally by descending priority within a line), so truncating to
    // the first N is deterministic and keeps the lowest-line decorations. resize()
    // down never reallocates, so this stays allocation-conscious.
    constexpr std::size_t kMaxMergedPerKind = 200000;
    if (merged.text_styles.size() > kMaxMergedPerKind) {
      merged.text_styles.resize(kMaxMergedPerKind);
    }
    if (merged.gutter_marks.size() > kMaxMergedPerKind) {
      merged.gutter_marks.resize(kMaxMergedPerKind);
    }
    if (merged.inline_texts.size() > kMaxMergedPerKind) {
      merged.inline_texts.resize(kMaxMergedPerKind);
    }
    if (merged.code_lenses.size() > kMaxMergedPerKind) {
      merged.code_lenses.resize(kMaxMergedPerKind);
    }
  }

  const auto existing = merged_by_path_.find(path_key);
  if (existing == merged_by_path_.end()) {
    // New path: the key string must be owned here, but emplace skips the extra
    // hash+lookup that operator[] would do on top of the find above.
    merged_by_path_.emplace(std::string(path_key), std::move(merged));
    return;
  }
  existing->second = std::move(merged);
}

bool PluginDecorationStore::ReplaceForOwnerFile(std::string_view owner,
                                                const std::filesystem::path& path,
                                                PluginDecorationData data) {
  const std::string owner_key(owner);
  const std::filesystem::path normalized_path = path.lexically_normal();
  const std::string path_key = PathKey(normalized_path);
  if (owner_key.empty() || path_key.empty()) {
    return false;
  }

  auto& owner_entries = by_owner_[owner_key];
  if (data.empty()) {
    const bool changed = owner_entries.erase(path_key) > 0;
    if (owner_entries.empty()) {
      by_owner_.erase(owner_key);
    }
    if (changed) {
      RebuildPath(path_key);
    }
    return changed;
  }

  // Sort once here, at publish, so RebuildPath's common single-owner case can copy
  // the merged view without re-sorting. It also makes the no-op check below
  // order-insensitive: republishing the same decorations in a different order is
  // correctly treated as no change.
  SortDecorations(data);
  OwnerFileDecorations next{.path = normalized_path, .data = std::move(data)};
  const auto existing = owner_entries.find(path_key);
  if (existing != owner_entries.end() && existing->second == next) {
    return false;
  }
  owner_entries[path_key] = std::move(next);
  RebuildPath(path_key);
  return true;
}

bool PluginDecorationStore::ClearOwner(std::string_view owner) {
  const auto owner_it = by_owner_.find(owner);
  if (owner_it == by_owner_.end()) {
    return false;
  }
  std::vector<std::string> path_keys;
  path_keys.reserve(owner_it->second.size());
  for (const auto& entry : owner_it->second) {
    path_keys.push_back(entry.first);
  }
  by_owner_.erase(owner_it);
  for (const auto& path_key : path_keys) {
    RebuildPath(path_key);
  }
  return !path_keys.empty();
}

bool PluginDecorationStore::ClearOwnerFile(std::string_view owner,
                                           const std::filesystem::path& path) {
  const std::string owner_key(owner);
  const std::string path_key = PathKey(path);
  if (owner_key.empty() || path_key.empty()) {
    return false;
  }
  const auto owner_it = by_owner_.find(owner_key);
  if (owner_it == by_owner_.end()) {
    return false;
  }
  if (owner_it->second.erase(path_key) == 0) {
    return false;
  }
  if (owner_it->second.empty()) {
    by_owner_.erase(owner_it);
  }
  RebuildPath(path_key);
  return true;
}

bool PluginDecorationStore::RetargetPathPrefix(const std::filesystem::path& old_prefix,
                                               const std::filesystem::path& new_prefix) {
  const std::filesystem::path normalized_old = old_prefix.lexically_normal();
  const std::filesystem::path normalized_new = new_prefix.lexically_normal();
  if (normalized_old.empty() || normalized_new.empty() || normalized_old == normalized_new) {
    return false;
  }

  bool changed = false;
  std::vector<std::string> affected_path_keys;
  for (auto owner_it = by_owner_.begin(); owner_it != by_owner_.end();) {
    auto& owner_entries = owner_it->second;
    std::vector<std::string> old_keys;
    std::vector<std::pair<std::string, OwnerFileDecorations>> replacements;

    for (const auto& [path_key, file] : owner_entries) {
      if (!PathEqualsOrWithin(file.path, normalized_old)) {
        continue;
      }
      OwnerFileDecorations moved = file;
      moved.path = ReplacePathPrefix(file.path, normalized_old, normalized_new);
      const std::string moved_key = PathKey(moved.path);
      if (moved_key.empty()) {
        continue;
      }
      old_keys.push_back(path_key);
      replacements.push_back({moved_key, std::move(moved)});
      affected_path_keys.push_back(path_key);
      affected_path_keys.push_back(moved_key);
      changed = true;
    }

    for (const auto& old_key : old_keys) {
      owner_entries.erase(old_key);
    }
    for (auto& [new_key, moved] : replacements) {
      owner_entries[new_key] = std::move(moved);
    }
    if (owner_entries.empty()) {
      owner_it = by_owner_.erase(owner_it);
    } else {
      ++owner_it;
    }
  }

  std::sort(affected_path_keys.begin(), affected_path_keys.end());
  affected_path_keys.erase(std::unique(affected_path_keys.begin(), affected_path_keys.end()),
                           affected_path_keys.end());
  for (const auto& path_key : affected_path_keys) {
    RebuildPath(path_key);
  }
  return changed;
}

bool PluginDecorationStore::ClearPathPrefix(const std::filesystem::path& path_prefix) {
  const std::filesystem::path normalized_prefix = path_prefix.lexically_normal();
  if (normalized_prefix.empty()) {
    return false;
  }

  bool changed = false;
  std::vector<std::string> affected_path_keys;
  for (auto owner_it = by_owner_.begin(); owner_it != by_owner_.end();) {
    auto& owner_entries = owner_it->second;
    for (auto path_it = owner_entries.begin(); path_it != owner_entries.end();) {
      if (!PathEqualsOrWithin(path_it->second.path, normalized_prefix)) {
        ++path_it;
        continue;
      }
      affected_path_keys.push_back(path_it->first);
      path_it = owner_entries.erase(path_it);
      changed = true;
    }
    if (owner_entries.empty()) {
      owner_it = by_owner_.erase(owner_it);
    } else {
      ++owner_it;
    }
  }

  std::sort(affected_path_keys.begin(), affected_path_keys.end());
  affected_path_keys.erase(std::unique(affected_path_keys.begin(), affected_path_keys.end()),
                           affected_path_keys.end());
  for (const auto& path_key : affected_path_keys) {
    RebuildPath(path_key);
  }
  return changed;
}

void PluginDecorationStore::Clear() {
  if (by_owner_.empty() && merged_by_path_.empty()) {
    return;
  }
  by_owner_.clear();
  merged_by_path_.clear();
}

const FileDecorations* PluginDecorationStore::FindByPathKey(std::string_view path_key) const {
  // Hot path: called per visible pane per frame with a precomputed key (see
  // NormalizedPathKey / TextViewport::path_key). Skip the lookup entirely when
  // no plugin has published any decorations; the heterogeneous find never
  // allocates because the key string already lives on the document.
  if (merged_by_path_.empty()) {
    return nullptr;
  }
  const auto it = merged_by_path_.find(path_key);
  return it == merged_by_path_.end() ? nullptr : &it->second;
}

const FileDecorations* PluginDecorationStore::FindByPath(const std::filesystem::path& path) const {
  if (merged_by_path_.empty()) {
    return nullptr;
  }
  return FindByPathKey(PathKey(path));
}

}  // namespace microide::editor
