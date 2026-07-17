#include "editor/PluginDecorationStore.h"

#include "editor/PathKey.h"
#include "util/PathMatch.h"

#include <algorithm>
#include <utility>

namespace microide::editor {

namespace {

using util::PathEqualsOrWithin;
using util::ReplacePathPrefix;

// Per-kind render orderings (used both to sort per-owner data at publish and to
// k-way merge across owners). Each is a strict weak ordering keyed first by line.
inline bool TextStyleLess(const TextStyleDecoration& a, const TextStyleDecoration& b) {
  if (a.line != b.line) return a.line < b.line;
  return a.start_column < b.start_column;
}
inline bool GutterMarkLess(const GutterMarkDecoration& a, const GutterMarkDecoration& b) {
  if (a.line != b.line) return a.line < b.line;
  return a.priority > b.priority;  // highest priority first within a line
}
inline bool InlineTextLess(const InlineTextDecoration& a, const InlineTextDecoration& b) {
  if (a.line != b.line) return a.line < b.line;
  return a.anchor_column < b.anchor_column;
}
inline bool CodeLensLess(const CodeLensDecoration& a, const CodeLensDecoration& b) {
  return a.line < b.line;
}

// Sort the four decoration vectors of `decorations` into the per-line render
// order the slice lookups rely on. Works on both PluginDecorationData (per-owner,
// sorted once at publish) and FileDecorations (the merged multi-owner view).
template <typename Decorations>
void SortDecorations(Decorations& decorations) {
  std::sort(decorations.text_styles.begin(), decorations.text_styles.end(), TextStyleLess);
  std::sort(decorations.gutter_marks.begin(), decorations.gutter_marks.end(), GutterMarkLess);
  std::sort(decorations.inline_texts.begin(), decorations.inline_texts.end(), InlineTextLess);
  std::sort(decorations.code_lenses.begin(), decorations.code_lenses.end(), CodeLensLess);
}

// Bounded k-way merge of one already-sorted per-owner vector per contributor into
// a single sorted, capped result. Because every input is pre-sorted by `less`
// (SortDecorations at publish), the merge emits in the same total order a full
// concatenate+std::sort would — but reserves at most `cap` and stops after `cap`
// elements, so both allocation and work are bounded by the retained cap rather than
// the (potentially far larger) total contributed. `project` selects the per-kind
// vector from each contributor. (TD-2026-07-17-090.)
template <typename T, typename Proj, typename Less>
std::vector<T> CappedSortedMerge(const std::vector<const PluginDecorationData*>& contributors,
                                 Proj project, std::size_t cap, Less less) {
  std::vector<T> out;
  if (cap == 0) {
    return out;
  }
  std::size_t total = 0;
  for (const PluginDecorationData* data : contributors) {
    total += project(*data).size();
  }
  out.reserve(std::min(total, cap));

  struct Head {
    std::size_t owner;
    std::size_t index;
  };
  // Min-heap on the projected element: `greater` orders the heap so the smallest
  // element sits at the top (std::*_heap builds a max-heap under the comparator).
  const auto greater = [&](const Head& a, const Head& b) {
    return less(project(*contributors[b.owner])[b.index], project(*contributors[a.owner])[a.index]);
  };
  std::vector<Head> heap;
  heap.reserve(contributors.size());
  for (std::size_t i = 0; i < contributors.size(); ++i) {
    if (!project(*contributors[i]).empty()) {
      heap.push_back({i, 0});
    }
  }
  std::make_heap(heap.begin(), heap.end(), greater);
  while (!heap.empty() && out.size() < cap) {
    std::pop_heap(heap.begin(), heap.end(), greater);
    const Head top = heap.back();
    heap.pop_back();
    const std::vector<T>& src = project(*contributors[top.owner]);
    out.push_back(src[top.index]);
    if (top.index + 1 < src.size()) {
      heap.push_back({top.owner, top.index + 1});
      std::push_heap(heap.begin(), heap.end(), greater);
    }
  }
  return out;
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

  // Count the owners contributing to this path. Per-owner data is already sorted
  // at publish time, so the common single-owner case rebuilds with one copy and
  // no sort; only genuine multi-owner overlap pays the merge + re-sort. Counting
  // first keeps the dominant single-owner publish path free of the contributor
  // vector's heap allocation (the multi-owner branch materializes it lazily).
  const OwnerFileDecorations* sole = nullptr;
  std::size_t contributor_count = 0;
  for (const auto& owner_entry : by_owner_) {
    const auto it = owner_entry.second.find(path_key);
    if (it == owner_entry.second.end()) {
      continue;
    }
    ++contributor_count;
    sole = &it->second;
  }

  if (contributor_count == 0) {
    // Heterogeneous find avoids materializing a std::string key just to erase;
    // unordered_map has no transparent erase(key) overload before C++23.
    if (const auto it = merged_by_path_.find(path_key); it != merged_by_path_.end()) {
      merged_by_path_.erase(it);
    }
    return;
  }

  FileDecorations merged;
  if (contributor_count == 1) {
    merged.path = sole->path;
    merged.text_styles = sole->data.text_styles;
    merged.gutter_marks = sole->data.gutter_marks;
    merged.inline_texts = sole->data.inline_texts;
    merged.code_lenses = sole->data.code_lenses;
  } else {
    // Rare multi-owner overlap: gather the contributor pointers now. A reused
    // thread-local scratch keeps even repeated merges allocation-free after warmup.
    thread_local std::vector<const PluginDecorationData*> contributors;
    contributors.clear();
    contributors.reserve(contributor_count);
    for (const auto& owner_entry : by_owner_) {
      const auto it = owner_entry.second.find(path_key);
      if (it != owner_entry.second.end()) {
        contributors.push_back(&it->second.data);
      }
    }
    merged.path = sole->path;

    // Per-owner input is capped (kMaxEntriesPerKind) at publish, but a pathological
    // set of plugins targeting one file could still push the summed per-kind totals
    // arbitrarily high. Rather than concatenate every contribution, sort the whole
    // thing, then truncate — which does O(all-contributed) allocation + O(N log N)
    // work before the cap ever applies — do a bounded k-way merge of the already-
    // sorted per-owner vectors that reserves and emits at most kMaxMergedPerKind.
    // Result is byte-for-byte identical (lowest-line decorations kept in render
    // order); work and peak allocation are now bounded by the retained cap.
    constexpr std::size_t kMaxMergedPerKind = 200000;
    merged.text_styles = CappedSortedMerge<TextStyleDecoration>(
        contributors,
        [](const PluginDecorationData& d) -> const std::vector<TextStyleDecoration>& {
          return d.text_styles;
        },
        kMaxMergedPerKind, TextStyleLess);
    merged.gutter_marks = CappedSortedMerge<GutterMarkDecoration>(
        contributors,
        [](const PluginDecorationData& d) -> const std::vector<GutterMarkDecoration>& {
          return d.gutter_marks;
        },
        kMaxMergedPerKind, GutterMarkLess);
    merged.inline_texts = CappedSortedMerge<InlineTextDecoration>(
        contributors,
        [](const PluginDecorationData& d) -> const std::vector<InlineTextDecoration>& {
          return d.inline_texts;
        },
        kMaxMergedPerKind, InlineTextLess);
    merged.code_lenses = CappedSortedMerge<CodeLensDecoration>(
        contributors,
        [](const PluginDecorationData& d) -> const std::vector<CodeLensDecoration>& {
          return d.code_lenses;
        },
        kMaxMergedPerKind, CodeLensLess);
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
