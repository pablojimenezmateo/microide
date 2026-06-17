#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace microide::editor {

// One source-line breakpoint. Lines are 0-based buffer line indices (matching
// FoldGutterMark.line_index and TextViewport line addressing); the DAP wire is
// 1-based, so the +1 conversion lives only in the setBreakpoints encoder.
//
// `condition` / `hit_condition` / `log_message` are persisted but unused until
// Phase 6 (conditional / hit-count / logpoints). The trailing fields are
// transient adapter results: never persisted, reset whenever the store is
// loaded or a session ends.
struct Breakpoint {
  std::size_t line = 0;
  bool enabled = true;
  std::optional<std::string> condition;
  std::optional<std::string> hit_condition;
  std::optional<std::string> log_message;

  // Transient (not persisted): populated from setBreakpoints responses.
  bool verified = false;
  int adapter_id = 0;
  std::string verify_message;

  bool operator==(const Breakpoint& other) const {
    return line == other.line && enabled == other.enabled && condition == other.condition &&
           hit_condition == other.hit_condition && log_message == other.log_message;
  }
};

// Adapter verification result mapped from a DAP `Breakpoint` (kept DAP-free so
// this editor-layer store carries no workspace/protocol dependency).
struct VerifiedBreakpoint {
  int id = 0;
  bool verified = false;
  int line = 0;  // adapter-reported 1-based line (0 when absent)
  std::string message;
};

// Per-project breakpoint state keyed by file path. Adapter-agnostic: the host
// snapshots it at launch, sends setBreakpoints, and reflects verification back
// through ApplyVerification. Mirrors the ownership/location of DiagnosticsStore
// (lives on ProjectWorkspaceState, survives session restarts).
class BreakpointStore {
 public:
  struct FileBreakpoints {
    std::filesystem::path path;
    std::vector<Breakpoint> breakpoints;  // sorted by line
  };

  // Toggle a breakpoint on `path:line`. Returns true when a breakpoint now
  // exists on that line (it was added), false when it was removed.
  bool Toggle(const std::filesystem::path& path, std::size_t line);
  void Set(const std::filesystem::path& path, std::size_t line, bool enabled = true);
  void Remove(const std::filesystem::path& path, std::size_t line);

  // Edit the Phase 6 modifier fields. Each finds-or-creates the breakpoint on
  // `path:line` (so setting a condition on a bare line materializes a real
  // breakpoint), assigns the field, and bumps the revision when it changes. A
  // nullopt clears the field (e.g. an empty condition string from the prompt).
  void SetCondition(const std::filesystem::path& path, std::size_t line,
                    std::optional<std::string> condition);
  void SetHitCondition(const std::filesystem::path& path, std::size_t line,
                       std::optional<std::string> hit_condition);
  void SetLogMessage(const std::filesystem::path& path, std::size_t line,
                     std::optional<std::string> log_message);
  void ClearFile(const std::filesystem::path& path);
  void Clear();

  bool HasBreakpoint(const std::filesystem::path& path, std::size_t line) const;
  // Breakpoints on `path`, sorted by line, or nullptr when none.
  const std::vector<Breakpoint>* FindByPath(const std::filesystem::path& path) const;

  // Every file with at least one breakpoint (for the launch snapshot).
  std::vector<FileBreakpoints> SnapshotAll() const;

  // Reflect adapter verification for one file. The DAP response array is
  // positional to the setBreakpoints request, so we match by index first and
  // fall back to line when an adapter reorders/moves breakpoints.
  void ApplyVerification(const std::filesystem::path& path,
                         const std::vector<VerifiedBreakpoint>& results);
  // Drop all transient verification state (e.g. when a session terminates).
  void ResetVerification();

  // Replace the entire store (used by persistence restore). Resets verification.
  void ReplaceAll(std::vector<FileBreakpoints> files);

  std::uint64_t revision() const { return revision_; }

 private:
  static std::string PathKey(const std::filesystem::path& path);
  std::vector<Breakpoint>* MutableForKey(const std::string& key);
  // Find the breakpoint on `path:line`, creating it (sorted) when absent.
  // Returns nullptr only for an empty path.
  Breakpoint* MutableBreakpoint(const std::filesystem::path& path, std::size_t line);
  void BumpRevision();

  struct FileEntry {
    std::filesystem::path path;
    std::vector<Breakpoint> breakpoints;  // sorted by line
  };
  std::unordered_map<std::string, FileEntry> by_path_;
  std::uint64_t revision_ = 0;
};

}  // namespace microide::editor
