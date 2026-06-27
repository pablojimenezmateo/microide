#include "editor/DiagnosticsStore.h"

#include "editor/PathKey.h"

#include <algorithm>
#include <utility>

namespace microide::editor {

namespace {

int SeverityRank(DiagnosticSeverity severity) {
  switch (severity) {
    case DiagnosticSeverity::Error:
      return 0;
    case DiagnosticSeverity::Warning:
      return 1;
    case DiagnosticSeverity::Info:
      return 2;
    case DiagnosticSeverity::Hint:
      return 3;
  }
  return 4;
}

std::vector<PublishedDiagnostic> CollectPublishedDiagnostics(std::string_view owner,
                                                             const std::filesystem::path& path,
                                                             std::vector<Diagnostic> diagnostics) {
  std::vector<PublishedDiagnostic> published;
  published.reserve(diagnostics.size());
  for (auto& diagnostic : diagnostics) {
    published.push_back(PublishedDiagnostic{
        .owner = std::string(owner),
        .path = path,
        .range = diagnostic.range,
        .severity = diagnostic.severity,
        .message = std::move(diagnostic.message),
    });
  }
  return published;
}

bool PathEqualsOrWithin(const std::filesystem::path& candidate,
                        const std::filesystem::path& root) {
  const std::filesystem::path normalized_candidate = candidate.lexically_normal();
  const std::filesystem::path normalized_root = root.lexically_normal();
  if (normalized_candidate.empty() || normalized_root.empty()) {
    return false;
  }
  if (normalized_candidate == normalized_root) {
    return true;
  }

  std::error_code error;
  const std::filesystem::path relative =
      std::filesystem::relative(normalized_candidate, normalized_root, error);
  if (error || relative.empty()) {
    return false;
  }
  const std::string relative_text = relative.generic_string();
  return relative_text != "." && relative_text != ".." && relative_text.rfind("../", 0) != 0;
}

std::filesystem::path ReplacePathPrefix(const std::filesystem::path& path,
                                        const std::filesystem::path& old_prefix,
                                        const std::filesystem::path& new_prefix) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  const std::filesystem::path normalized_old_prefix = old_prefix.lexically_normal();
  const std::filesystem::path normalized_new_prefix = new_prefix.lexically_normal();
  if (!PathEqualsOrWithin(normalized_path, normalized_old_prefix)) {
    return normalized_path;
  }
  if (normalized_path == normalized_old_prefix) {
    return normalized_new_prefix;
  }

  std::error_code error;
  const std::filesystem::path relative =
      std::filesystem::relative(normalized_path, normalized_old_prefix, error);
  if (error || relative.empty()) {
    return normalized_path;
  }
  return (normalized_new_prefix / relative).lexically_normal();
}

}  // namespace

std::string DiagnosticsStore::PathKey(const std::filesystem::path& path) {
  return NormalizedPathKey(path);
}

void DiagnosticsStore::SortDiagnostics(std::vector<PublishedDiagnostic>* diagnostics) {
  if (diagnostics == nullptr) {
    return;
  }
  std::sort(diagnostics->begin(), diagnostics->end(),
            [](const PublishedDiagnostic& lhs, const PublishedDiagnostic& rhs) {
              if (lhs.path != rhs.path) {
                // native() avoids the two per-comparison string allocations.
                return lhs.path.native() < rhs.path.native();
              }
              if (lhs.range.start.line != rhs.range.start.line) {
                return lhs.range.start.line < rhs.range.start.line;
              }
              if (lhs.range.start.column != rhs.range.start.column) {
                return lhs.range.start.column < rhs.range.start.column;
              }
              if (lhs.range.end.line != rhs.range.end.line) {
                return lhs.range.end.line < rhs.range.end.line;
              }
              if (lhs.range.end.column != rhs.range.end.column) {
                return lhs.range.end.column < rhs.range.end.column;
              }
              if (lhs.severity != rhs.severity) {
                return SeverityRank(lhs.severity) < SeverityRank(rhs.severity);
              }
              if (lhs.owner != rhs.owner) {
                return lhs.owner < rhs.owner;
              }
              return lhs.message < rhs.message;
            });
}

DiagnosticsStore::SeveritySummary DiagnosticsStore::SummarizeDiagnostics(
    const std::vector<PublishedDiagnostic>& diagnostics) {
  SeveritySummary summary;
  for (const PublishedDiagnostic& diagnostic : diagnostics) {
    switch (diagnostic.severity) {
      case DiagnosticSeverity::Error:
        ++summary.errors;
        break;
      case DiagnosticSeverity::Warning:
        ++summary.warnings;
        break;
      case DiagnosticSeverity::Info:
        ++summary.infos;
        break;
      case DiagnosticSeverity::Hint:
        ++summary.hints;
        break;
    }
  }
  return summary;
}

void DiagnosticsStore::AddSummary(const SeveritySummary& summary) {
  error_count_ += summary.errors;
  warning_count_ += summary.warnings;
  info_count_ += summary.infos;
  hint_count_ += summary.hints;
}

void DiagnosticsStore::RemoveSummary(const SeveritySummary& summary) {
  error_count_ -= std::min(error_count_, summary.errors);
  warning_count_ -= std::min(warning_count_, summary.warnings);
  info_count_ -= std::min(info_count_, summary.infos);
  hint_count_ -= std::min(hint_count_, summary.hints);
}

void DiagnosticsStore::BumpRevision() {
  if (++revision_ == 0) {
    revision_ = 1;
  }
}

void DiagnosticsStore::RebuildPath(std::string_view path_key) {
  if (path_key.empty()) {
    return;
  }

  FileDiagnostics merged;
  for (const auto& owner_entry : diagnostics_by_owner_) {
    const auto it = owner_entry.second.find(path_key);
    if (it == owner_entry.second.end()) {
      continue;
    }
    if (merged.path.empty()) {
      merged.path = it->second.path;
    }
    merged.diagnostics.insert(merged.diagnostics.end(), it->second.diagnostics.begin(),
                              it->second.diagnostics.end());
  }

  if (merged.diagnostics.empty()) {
    const auto existing = merged_by_path_.find(path_key);
    if (existing != merged_by_path_.end()) {
      RemoveSummary(existing->second.summary);
      merged_by_path_.erase(existing);
      BumpRevision();
    }
    return;
  }

  SortDiagnostics(&merged.diagnostics);
  merged.summary = SummarizeDiagnostics(merged.diagnostics);
  auto existing = merged_by_path_.find(path_key);
  if (existing == merged_by_path_.end()) {
    AddSummary(merged.summary);
    merged_by_path_[std::string(path_key)] = std::move(merged);
    BumpRevision();
    return;
  }
  if (existing->second == merged) {
    return;
  }
  RemoveSummary(existing->second.summary);
  AddSummary(merged.summary);
  existing->second = std::move(merged);
  BumpRevision();
}

bool DiagnosticsStore::ReplaceForOwnerFile(std::string_view owner,
                                           const std::filesystem::path& path,
                                           std::vector<Diagnostic> diagnostics) {
  const std::string owner_key(owner);
  const std::filesystem::path normalized_path = path.lexically_normal();
  const std::string path_key = PathKey(normalized_path);
  if (owner_key.empty() || path_key.empty()) {
    return false;
  }

  bool changed = false;
  auto& owner_entries = diagnostics_by_owner_[owner_key];
  if (diagnostics.empty()) {
    changed = owner_entries.erase(path_key) > 0;
    if (owner_entries.empty()) {
      diagnostics_by_owner_.erase(owner_key);
    }
    if (changed) {
      RebuildPath(path_key);
    }
    return changed;
  }

  FileDiagnostics next{
      .path = normalized_path,
      .diagnostics = CollectPublishedDiagnostics(owner_key, normalized_path, std::move(diagnostics)),
      .summary = {},
  };
  SortDiagnostics(&next.diagnostics);
  next.summary = SummarizeDiagnostics(next.diagnostics);

  const auto existing = owner_entries.find(path_key);
  if (existing != owner_entries.end() && existing->second == next) {
    return false;
  }

  owner_entries[path_key] = std::move(next);
  RebuildPath(path_key);
  return true;
}

bool DiagnosticsStore::ClearOwner(std::string_view owner) {
  const auto owner_it = diagnostics_by_owner_.find(owner);
  if (owner_it == diagnostics_by_owner_.end()) {
    return false;
  }

  std::vector<std::string> path_keys;
  path_keys.reserve(owner_it->second.size());
  for (const auto& entry : owner_it->second) {
    path_keys.push_back(entry.first);
  }
  diagnostics_by_owner_.erase(owner_it);
  for (const auto& path_key : path_keys) {
    RebuildPath(path_key);
  }
  return !path_keys.empty();
}

bool DiagnosticsStore::ClearOwnerFile(std::string_view owner, const std::filesystem::path& path) {
  const std::string owner_key(owner);
  const std::string path_key = PathKey(path);
  if (owner_key.empty() || path_key.empty()) {
    return false;
  }

  const auto owner_it = diagnostics_by_owner_.find(owner_key);
  if (owner_it == diagnostics_by_owner_.end()) {
    return false;
  }
  if (owner_it->second.erase(path_key) == 0) {
    return false;
  }
  if (owner_it->second.empty()) {
    diagnostics_by_owner_.erase(owner_it);
  }
  RebuildPath(path_key);
  return true;
}

bool DiagnosticsStore::RetargetPathPrefix(const std::filesystem::path& old_prefix,
                                          const std::filesystem::path& new_prefix) {
  const std::filesystem::path normalized_old = old_prefix.lexically_normal();
  const std::filesystem::path normalized_new = new_prefix.lexically_normal();
  if (normalized_old.empty() || normalized_new.empty() || normalized_old == normalized_new) {
    return false;
  }

  bool changed = false;
  std::vector<std::string> affected_path_keys;
  for (auto owner_it = diagnostics_by_owner_.begin(); owner_it != diagnostics_by_owner_.end();) {
    auto& owner_entries = owner_it->second;
    std::vector<std::string> old_keys;
    std::vector<std::pair<std::string, FileDiagnostics>> replacements;

    for (const auto& [path_key, file_diagnostics] : owner_entries) {
      if (!PathEqualsOrWithin(file_diagnostics.path, normalized_old)) {
        continue;
      }

      FileDiagnostics moved = file_diagnostics;
      moved.path = ReplacePathPrefix(file_diagnostics.path, normalized_old, normalized_new);
      for (auto& diagnostic : moved.diagnostics) {
        diagnostic.path = ReplacePathPrefix(diagnostic.path, normalized_old, normalized_new);
      }
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
      owner_it = diagnostics_by_owner_.erase(owner_it);
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

bool DiagnosticsStore::ClearPathPrefix(const std::filesystem::path& path_prefix) {
  const std::filesystem::path normalized_prefix = path_prefix.lexically_normal();
  if (normalized_prefix.empty()) {
    return false;
  }

  bool changed = false;
  std::vector<std::string> affected_path_keys;
  for (auto owner_it = diagnostics_by_owner_.begin(); owner_it != diagnostics_by_owner_.end();) {
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
      owner_it = diagnostics_by_owner_.erase(owner_it);
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

void DiagnosticsStore::Clear() {
  if (diagnostics_by_owner_.empty() && merged_by_path_.empty()) {
    return;
  }
  diagnostics_by_owner_.clear();
  merged_by_path_.clear();
  error_count_ = 0;
  warning_count_ = 0;
  info_count_ = 0;
  hint_count_ = 0;
  BumpRevision();
}

const std::vector<PublishedDiagnostic>* DiagnosticsStore::FindByPathKey(
    std::string_view path_key) const {
  // Hot path: called per visible pane per frame with a precomputed key. Skip
  // the lookup when no diagnostics have been published; the heterogeneous find
  // never allocates because the key string already lives on the document.
  if (merged_by_path_.empty()) {
    return nullptr;
  }
  const auto it = merged_by_path_.find(path_key);
  return it == merged_by_path_.end() ? nullptr : &it->second.diagnostics;
}

const std::vector<PublishedDiagnostic>* DiagnosticsStore::FindByPath(
    const std::filesystem::path& path) const {
  if (merged_by_path_.empty()) {
    return nullptr;
  }
  return FindByPathKey(PathKey(path));
}

std::vector<PublishedDiagnostic> DiagnosticsStore::SnapshotAll() const {
  std::vector<PublishedDiagnostic> diagnostics;
  for (const auto& entry : merged_by_path_) {
    diagnostics.insert(diagnostics.end(), entry.second.diagnostics.begin(),
                       entry.second.diagnostics.end());
  }
  SortDiagnostics(&diagnostics);
  return diagnostics;
}

std::vector<PublishedDiagnostic> DiagnosticsStore::SnapshotForOwner(std::string_view owner) const {
  const auto owner_it = diagnostics_by_owner_.find(owner);
  if (owner_it == diagnostics_by_owner_.end()) {
    return {};
  }

  std::vector<PublishedDiagnostic> diagnostics;
  for (const auto& entry : owner_it->second) {
    diagnostics.insert(diagnostics.end(), entry.second.diagnostics.begin(),
                       entry.second.diagnostics.end());
  }
  SortDiagnostics(&diagnostics);
  return diagnostics;
}

}  // namespace microide::editor
