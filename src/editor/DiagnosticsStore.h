#pragma once

#include <filesystem>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "editor/TextViewport.h"
#include "util/TransparentStringHash.h"
#include "util/Revision.h"

namespace microide::editor {

enum class DiagnosticSeverity {
  Error,
  Warning,
  Info,
  Hint,
};

struct Diagnostic {
  SelectionRange range;
  DiagnosticSeverity severity = DiagnosticSeverity::Error;
  std::string message;

  bool operator==(const Diagnostic& other) const {
    return range.start.line == other.range.start.line &&
           range.start.column == other.range.start.column &&
           range.end.line == other.range.end.line &&
           range.end.column == other.range.end.column && severity == other.severity &&
           message == other.message;
  }
};

struct PublishedDiagnostic {
  std::string owner;
  std::filesystem::path path;
  SelectionRange range;
  DiagnosticSeverity severity = DiagnosticSeverity::Error;
  std::string message;

  bool operator==(const PublishedDiagnostic& other) const {
    return owner == other.owner && path == other.path &&
           range.start.line == other.range.start.line &&
           range.start.column == other.range.start.column &&
           range.end.line == other.range.end.line &&
           range.end.column == other.range.end.column && severity == other.severity &&
           message == other.message;
  }
};

bool TryParseDiagnosticSeverity(std::string_view token, DiagnosticSeverity* severity);
DiagnosticSeverity ParseDiagnosticSeverity(std::string_view token);

std::span<const PublishedDiagnostic> FilterDiagnosticsAtLeastSeverity(
    std::span<const PublishedDiagnostic> in, DiagnosticSeverity min_severity,
    std::vector<PublishedDiagnostic>& scratch);

std::vector<PublishedDiagnostic> SelectContextDiagnostics(
    std::span<const PublishedDiagnostic> in, const SelectionRange& want, std::size_t max_count,
    bool* truncated = nullptr);

class DiagnosticsStore {
 public:
  bool ReplaceForOwnerFile(std::string_view owner,
                           const std::filesystem::path& path,
                           std::vector<Diagnostic> diagnostics);
  bool TransformOwnerFile(std::string_view owner, const std::filesystem::path& path,
                          const std::function<SelectionRange(SelectionRange)>& transform);
  bool ClearOwner(std::string_view owner);
  bool ClearOwnerFile(std::string_view owner, const std::filesystem::path& path);
  bool RetargetPathPrefix(const std::filesystem::path& old_prefix,
                          const std::filesystem::path& new_prefix);
  bool ClearPathPrefix(const std::filesystem::path& path_prefix);
  void Clear();
  const std::vector<PublishedDiagnostic>* FindByPath(const std::filesystem::path& path) const;
  const std::vector<PublishedDiagnostic>* FindByPathKey(std::string_view path_key) const;
  bool IsPathTruncated(const std::filesystem::path& path) const;
  bool IsPathKeyTruncated(std::string_view path_key) const;
  bool HasTruncatedFile() const { return truncated_file_count_ > 0; }
  std::vector<PublishedDiagnostic> SnapshotAll() const;
  std::vector<PublishedDiagnostic> SnapshotForOwner(std::string_view owner) const;
  std::size_t ErrorCount() const { return error_count_; }
  std::size_t WarningCount() const { return warning_count_; }
  std::size_t InfoCount() const { return info_count_; }
  std::size_t HintCount() const { return hint_count_; }
  std::uint64_t revision() const { return revision_.value(); }
  std::uint64_t PathRevisionByKey(std::string_view path_key) const;

 private:
  struct SeveritySummary {
    std::size_t errors = 0;
    std::size_t warnings = 0;
    std::size_t infos = 0;
    std::size_t hints = 0;

    bool operator==(const SeveritySummary&) const = default;
  };

  struct FileDiagnostics {
    std::filesystem::path path;
    std::vector<PublishedDiagnostic> diagnostics;
    SeveritySummary summary;
    bool truncated = false;
    std::size_t original_count = 0;
    std::uint64_t changed_at = 0;

    bool operator==(const FileDiagnostics&) const = default;
  };

  static std::string PathKey(const std::filesystem::path& path);
  static void SortDiagnostics(std::vector<PublishedDiagnostic>* diagnostics);
  static SeveritySummary SummarizeDiagnostics(
      const std::vector<PublishedDiagnostic>& diagnostics);
  void AddSummary(const SeveritySummary& summary);
  void RemoveSummary(const SeveritySummary& summary);
  void RebuildPath(std::string_view path_key);

  std::unordered_map<std::string,
                     std::unordered_map<std::string, FileDiagnostics,
                                        util::TransparentStringHash, std::equal_to<>>,
                     util::TransparentStringHash, std::equal_to<>>
      diagnostics_by_owner_;
  std::unordered_map<std::string, FileDiagnostics, util::TransparentStringHash,
                     std::equal_to<>>
      merged_by_path_;
  std::size_t error_count_ = 0;
  std::size_t warning_count_ = 0;
  std::size_t info_count_ = 0;
  std::size_t hint_count_ = 0;
  std::size_t truncated_file_count_ = 0;
  util::Revision revision_;
};

}
