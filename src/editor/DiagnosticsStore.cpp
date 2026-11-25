#include "editor/DiagnosticsStore.h"

#include "util/StringUtil.h"

#include "editor/OwnerPathIndexOps.h"
#include "editor/PathKey.h"
#include "util/PathMatch.h"

#include <algorithm>
#include <utility>

namespace microide::editor {

bool TryParseDiagnosticSeverity(std::string_view token, DiagnosticSeverity* severity) {
  if (severity == nullptr) {
    return false;
  }
  const std::string value = util::ToLowerAscii(token);
  if (value == "error") {
    *severity = DiagnosticSeverity::Error;
    return true;
  }
  if (value == "warning" || value == "warn") {
    *severity = DiagnosticSeverity::Warning;
    return true;
  }
  if (value == "info" || value == "information") {
    *severity = DiagnosticSeverity::Info;
    return true;
  }
  if (value == "hint") {
    *severity = DiagnosticSeverity::Hint;
    return true;
  }
  return false;
}

DiagnosticSeverity ParseDiagnosticSeverity(std::string_view token) {
  DiagnosticSeverity severity = DiagnosticSeverity::Hint;
  (void)TryParseDiagnosticSeverity(token, &severity);
  return severity;
}

std::span<const PublishedDiagnostic> FilterDiagnosticsAtLeastSeverity(
    std::span<const PublishedDiagnostic> in, DiagnosticSeverity min_severity,
    std::vector<PublishedDiagnostic>& scratch) {
  if (min_severity == DiagnosticSeverity::Hint) {
    return in;
  }
  scratch.clear();
  for (const PublishedDiagnostic& diagnostic : in) {
    if (static_cast<int>(diagnostic.severity) <= static_cast<int>(min_severity)) {
      scratch.push_back(diagnostic);
    }
  }
  return scratch;
}

std::vector<PublishedDiagnostic> SelectContextDiagnostics(
    std::span<const PublishedDiagnostic> in, const SelectionRange& want, std::size_t max_count,
    bool* truncated) {
  if (truncated != nullptr) {
    *truncated = false;
  }
  const auto before = [](const TextPosition& a, const TextPosition& b) {
    return a.line < b.line || (a.line == b.line && a.column < b.column);
  };
  const auto normalize = [&](const SelectionRange& r) {
    return before(r.end, r.start) ? SelectionRange{r.end, r.start} : r;
  };
  const SelectionRange w = normalize(want);

  std::vector<PublishedDiagnostic> result;
  for (const PublishedDiagnostic& diagnostic : in) {
    const SelectionRange have = normalize(diagnostic.range);
    if (before(have.end, w.start) || before(w.end, have.start)) {
      continue;
    }
    if (result.size() >= max_count) {
      if (truncated != nullptr) {
        *truncated = true;
      }
      break;
    }
    result.push_back(diagnostic);
  }
  return result;
}

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

}

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
    merged.truncated = merged.truncated || it->second.truncated;
    merged.original_count += it->second.original_count;
  }

  if (merged.diagnostics.empty()) {
    const auto existing = merged_by_path_.find(path_key);
    if (existing != merged_by_path_.end()) {
      RemoveSummary(existing->second.summary);
      if (existing->second.truncated) {
        --truncated_file_count_;
      }
      merged_by_path_.erase(existing);
      revision_.Bump();
    }
    return;
  }

  SortDiagnostics(&merged.diagnostics);
  merged.summary = SummarizeDiagnostics(merged.diagnostics);
  constexpr std::size_t kMaxMergedDiagnosticsPerFile = 20000;
  if (merged.diagnostics.size() > kMaxMergedDiagnosticsPerFile) {
    merged.diagnostics.resize(kMaxMergedDiagnosticsPerFile);
    merged.truncated = true;
  }
  auto existing = merged_by_path_.find(path_key);
  if (existing == merged_by_path_.end()) {
    AddSummary(merged.summary);
    if (merged.truncated) {
      ++truncated_file_count_;
    }
    revision_.Bump();
    merged.changed_at = revision_.value();
    merged_by_path_.emplace(std::string(path_key), std::move(merged));
    return;
  }
  merged.changed_at = existing->second.changed_at;
  if (existing->second == merged) {
    return;
  }
  RemoveSummary(existing->second.summary);
  AddSummary(merged.summary);
  if (existing->second.truncated && !merged.truncated) {
    --truncated_file_count_;
  } else if (!existing->second.truncated && merged.truncated) {
    ++truncated_file_count_;
  }
  revision_.Bump();
  merged.changed_at = revision_.value();
  existing->second = std::move(merged);
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

  constexpr std::size_t kMaxDiagnosticsPerOwnerFile = 10000;
  const std::size_t original_count = diagnostics.size();
  bool truncated = false;
  if (diagnostics.size() > kMaxDiagnosticsPerOwnerFile) {
    diagnostics.resize(kMaxDiagnosticsPerOwnerFile);
    truncated = true;
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
      .truncated = truncated,
      .original_count = original_count,
  };
  SortDiagnostics(&next.diagnostics);
  next.summary = SummarizeDiagnostics(next.diagnostics);

  const auto existing = owner_entries.find(path_key);
  if (existing != owner_entries.end()) {
    if (existing->second == next) {
      return false;
    }
    existing->second = std::move(next);
  } else {
    owner_entries.emplace(path_key, std::move(next));
  }
  RebuildPath(path_key);
  return true;
}

bool DiagnosticsStore::TransformOwnerFile(
    std::string_view owner, const std::filesystem::path& path,
    const std::function<SelectionRange(SelectionRange)>& transform) {
  if (!transform || owner.empty() || path.empty() || diagnostics_by_owner_.empty()) {
    return false;
  }
  const auto owner_it = diagnostics_by_owner_.find(std::string(owner));
  if (owner_it == diagnostics_by_owner_.end() || owner_it->second.empty()) {
    return false;
  }
  const std::string path_key = PathKey(path);
  if (path_key.empty()) {
    return false;
  }
  const auto file_it = owner_it->second.find(path_key);
  if (file_it == owner_it->second.end() || file_it->second.diagnostics.empty()) {
    return false;
  }

  for (PublishedDiagnostic& diagnostic : file_it->second.diagnostics) {
    diagnostic.range = transform(diagnostic.range);
  }
  SortDiagnostics(&file_it->second.diagnostics);
  RebuildPath(path_key);
  return true;
}

bool DiagnosticsStore::ClearOwner(std::string_view owner) {
  return ClearOwnerEntries(diagnostics_by_owner_, owner,
                           [this](const std::string& path_key) { RebuildPath(path_key); });
}

bool DiagnosticsStore::ClearOwnerFile(std::string_view owner, const std::filesystem::path& path) {
  return ClearOwnerFileEntry(diagnostics_by_owner_, std::string(owner), PathKey(path),
                             [this](const std::string& path_key) { RebuildPath(path_key); });
}

bool DiagnosticsStore::RetargetPathPrefix(const std::filesystem::path& old_prefix,
                                          const std::filesystem::path& new_prefix) {
  return RetargetEntriesUnderPrefix(
      diagnostics_by_owner_, old_prefix, new_prefix,
      [](FileDiagnostics& moved, const std::filesystem::path& from,
         const std::filesystem::path& to) {
        for (auto& diagnostic : moved.diagnostics) {
          diagnostic.path = util::ReplacePathPrefix(diagnostic.path, from, to);
        }
      },
      [this](const std::string& path_key) { RebuildPath(path_key); });
}

bool DiagnosticsStore::ClearPathPrefix(const std::filesystem::path& path_prefix) {
  return ClearEntriesUnderPrefix(diagnostics_by_owner_, path_prefix,
                                 [this](const std::string& path_key) { RebuildPath(path_key); });
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
  truncated_file_count_ = 0;
  revision_.Bump();
}

std::uint64_t DiagnosticsStore::PathRevisionByKey(std::string_view path_key) const {
  if (merged_by_path_.empty()) {
    return 0;
  }
  const auto it = merged_by_path_.find(path_key);
  return it == merged_by_path_.end() ? 0 : it->second.changed_at;
}

const std::vector<PublishedDiagnostic>* DiagnosticsStore::FindByPathKey(
    std::string_view path_key) const {
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

bool DiagnosticsStore::IsPathKeyTruncated(std::string_view path_key) const {
  if (truncated_file_count_ == 0 || merged_by_path_.empty()) {
    return false;
  }
  const auto it = merged_by_path_.find(path_key);
  return it != merged_by_path_.end() && it->second.truncated;
}

bool DiagnosticsStore::IsPathTruncated(const std::filesystem::path& path) const {
  if (truncated_file_count_ == 0 || merged_by_path_.empty()) {
    return false;
  }
  return IsPathKeyTruncated(PathKey(path));
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

}
