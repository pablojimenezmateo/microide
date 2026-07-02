#pragma once

#include <filesystem>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "editor/TextViewport.h"
#include "util/TransparentStringHash.h"

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

// Parse a `diagnostics.min_severity` token ("error"/"warning"/"info"/"hint");
// unknown tokens fall back to Hint (show everything).
DiagnosticSeverity ParseDiagnosticSeverity(std::string_view token);

// Returns `in` unchanged when min_severity is Hint (the show-all default, zero
// cost). Otherwise copies diagnostics at or above min_severity into `scratch` and
// returns a span over it. Lives here (not a render TU) so the copy never counts as
// render-hot-path string materialization.
std::span<const PublishedDiagnostic> FilterDiagnosticsAtLeastSeverity(
    std::span<const PublishedDiagnostic> in, DiagnosticSeverity min_severity,
    std::vector<PublishedDiagnostic>& scratch);

class DiagnosticsStore {
 public:
  bool ReplaceForOwnerFile(std::string_view owner,
                           const std::filesystem::path& path,
                           std::vector<Diagnostic> diagnostics);
  bool ClearOwner(std::string_view owner);
  bool ClearOwnerFile(std::string_view owner, const std::filesystem::path& path);
  bool RetargetPathPrefix(const std::filesystem::path& old_prefix,
                          const std::filesystem::path& new_prefix);
  bool ClearPathPrefix(const std::filesystem::path& path_prefix);
  void Clear();
  const std::vector<PublishedDiagnostic>* FindByPath(const std::filesystem::path& path) const;
  // Hot-path variant taking a precomputed NormalizedPathKey (see
  // TextViewport::path_key). Allocation-free: the map uses heterogeneous lookup.
  const std::vector<PublishedDiagnostic>* FindByPathKey(std::string_view path_key) const;
  std::vector<PublishedDiagnostic> SnapshotAll() const;
  std::vector<PublishedDiagnostic> SnapshotForOwner(std::string_view owner) const;
  std::size_t ErrorCount() const { return error_count_; }
  std::size_t WarningCount() const { return warning_count_; }
  std::size_t InfoCount() const { return info_count_; }
  std::size_t HintCount() const { return hint_count_; }
  std::uint64_t revision() const { return revision_; }

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

    bool operator==(const FileDiagnostics&) const = default;
  };

  static std::string PathKey(const std::filesystem::path& path);
  static void SortDiagnostics(std::vector<PublishedDiagnostic>* diagnostics);
  static SeveritySummary SummarizeDiagnostics(
      const std::vector<PublishedDiagnostic>& diagnostics);
  void AddSummary(const SeveritySummary& summary);
  void RemoveSummary(const SeveritySummary& summary);
  void BumpRevision();
  void RebuildPath(std::string_view path_key);

  // Transparent hashing lets find() accept a string_view without allocating a
  // throwaway std::string key on every lookup.
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
  std::uint64_t revision_ = 0;
};

}  // namespace microide::editor
