#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "editor/TextViewport.h"

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
  std::vector<PublishedDiagnostic> SnapshotAll() const;
  std::vector<PublishedDiagnostic> SnapshotForOwner(std::string_view owner) const;

 private:
  struct FileDiagnostics {
    std::filesystem::path path;
    std::vector<PublishedDiagnostic> diagnostics;

    bool operator==(const FileDiagnostics&) const = default;
  };

  static std::string PathKey(const std::filesystem::path& path);
  static void SortDiagnostics(std::vector<PublishedDiagnostic>* diagnostics);
  void RebuildPath(std::string_view path_key);

  std::unordered_map<std::string, std::unordered_map<std::string, FileDiagnostics>>
      diagnostics_by_owner_;
  std::unordered_map<std::string, FileDiagnostics> merged_by_path_;
};

}  // namespace microide::editor
