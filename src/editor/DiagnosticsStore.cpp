#include "editor/DiagnosticsStore.h"

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

}  // namespace

std::string DiagnosticsStore::PathKey(const std::filesystem::path& path) {
  return path.empty() ? std::string{} : path.lexically_normal().generic_string();
}

void DiagnosticsStore::SortDiagnostics(std::vector<PublishedDiagnostic>* diagnostics) {
  if (diagnostics == nullptr) {
    return;
  }
  std::sort(diagnostics->begin(), diagnostics->end(),
            [](const PublishedDiagnostic& lhs, const PublishedDiagnostic& rhs) {
              if (lhs.path != rhs.path) {
                return lhs.path.generic_string() < rhs.path.generic_string();
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

void DiagnosticsStore::RebuildPath(std::string_view path_key) {
  if (path_key.empty()) {
    return;
  }

  FileDiagnostics merged;
  for (const auto& owner_entry : diagnostics_by_owner_) {
    const auto it = owner_entry.second.find(std::string(path_key));
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
    merged_by_path_.erase(std::string(path_key));
    return;
  }

  SortDiagnostics(&merged.diagnostics);
  merged_by_path_[std::string(path_key)] = std::move(merged);
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
  };
  SortDiagnostics(&next.diagnostics);

  const auto existing = owner_entries.find(path_key);
  if (existing != owner_entries.end() && existing->second == next) {
    return false;
  }

  owner_entries[path_key] = std::move(next);
  RebuildPath(path_key);
  return true;
}

bool DiagnosticsStore::ClearOwner(std::string_view owner) {
  const auto owner_it = diagnostics_by_owner_.find(std::string(owner));
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

void DiagnosticsStore::Clear() {
  diagnostics_by_owner_.clear();
  merged_by_path_.clear();
}

const std::vector<PublishedDiagnostic>* DiagnosticsStore::FindByPath(
    const std::filesystem::path& path) const {
  const auto it = merged_by_path_.find(PathKey(path));
  return it == merged_by_path_.end() ? nullptr : &it->second.diagnostics;
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
  const auto owner_it = diagnostics_by_owner_.find(std::string(owner));
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
