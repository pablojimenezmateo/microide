#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "util/TransparentStringHash.h"

namespace microide::editor {

struct AppliedEdit;  // editor/EditTypes.h

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
  // 1-based line used to match this result back to a stored breakpoint (0 when
  // absent). For setBreakpoints responses the caller fills this with the line it
  // *requested* (the response is positional to the request), so verification lands
  // on the right line even if the user changed the breakpoint set while the
  // response was in flight. For an async `breakpoint` event it is the adapter line.
  int line = 0;
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
  // Flip the enabled state of an existing breakpoint on `path:line` (does NOT
  // create one). Returns true when a breakpoint was found and toggled. Used by the
  // Breakpoints panel so the user can disable/enable without leaving the panel.
  bool ToggleEnabled(const std::filesystem::path& path, std::size_t line);

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
  // Hot-path variant taking a precomputed NormalizedPathKey (see
  // TextViewport::path_key). Allocation-free: the map uses heterogeneous lookup.
  const std::vector<Breakpoint>* FindByPathKey(std::string_view path_key) const;

  // Every file with at least one breakpoint (for the launch snapshot).
  std::vector<FileBreakpoints> SnapshotAll() const;

  // Reflect adapter verification for one file. Each result is matched to a stored
  // breakpoint by its (requested) line — NOT by array index — so a setBreakpoints
  // response that arrives after the user has toggled another breakpoint in the same
  // file still verifies the correct lines instead of shifting onto its neighbours.
  // A result whose line no longer has a breakpoint (removed while in flight) is
  // dropped.
  void ApplyVerification(const std::filesystem::path& path,
                         const std::vector<VerifiedBreakpoint>& results);
  // Reflect a single asynchronous DAP `breakpoint` event (an adapter binding,
  // relocating, or invalidating a breakpoint after the initial response). Matches
  // by the adapter id assigned at setBreakpoints time, falling back to the reported
  // line. No-op when nothing matches. When `path` is empty, all files are searched
  // by id.
  void ApplyBreakpointEvent(const std::filesystem::path& path, const VerifiedBreakpoint& result);
  // Drop all transient verification state (e.g. when a session terminates).
  void ResetVerification();

  // Shift the breakpoint lines in `path` to follow a single applied edit (the
  // same AppliedEdit the LSP diagnostics shift consumes), so an insert/delete of
  // lines above a breakpoint keeps it anchored to its statement — matching VSCode.
  // Mirrors the diagnostics AdjustPositionForReplace: breakpoints at/above the
  // edit's first line stay; those below move by the net line delta; a breakpoint
  // inside the replaced span slides to the edit's last line. Collisions dedupe to
  // one breakpoint per line. Bumps the revision and returns true when anything
  // moved. `condition`/`enabled`/etc. ride along with the moved line.
  bool ShiftForAppliedEdit(const std::filesystem::path& path, const AppliedEdit& edit);

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
  // Transparent hashing lets FindByPathKey() accept a string_view without
  // allocating a throwaway std::string key on every lookup.
  std::unordered_map<std::string, FileEntry, util::TransparentStringHash, std::equal_to<>>
      by_path_;
  std::uint64_t revision_ = 0;
};

}  // namespace microide::editor
