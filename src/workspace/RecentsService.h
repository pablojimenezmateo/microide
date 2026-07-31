#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "workspace/WorkspacePersistenceFormat.h"

namespace microide::workspace {

class PersistenceService;

// Host-owned most-recently-used registry for projects and files. Holds the MRU in
// memory and persists it through PersistenceService after each change. Entries are
// newest-first and bounded by MaxProjects()/MaxFiles(). Recent files carry the
// project root they were opened under so the file finder can surface only the active
// project's recents.
//
// Saves are coalesced. "Opens are user-paced, so saves run inline" was true of a
// click or an Enter and false of everything else that opens files: session
// restore, a multi-select open, arrowing through the file finder, or any scripted
// / control-channel drive. Each save is a durable record write (temp + fsync +
// backup + rename) on the shell thread at ~1.35 ms, and the multi_tab_cycle
// scenario measured 61 of them -- 72 ms, 59% of that scenario's traced
// main-thread time -- for a convenience list. Rapid opens now fold into one
// write; FlushPendingSave() at shutdown lands whatever the window still holds.
class RecentsService {
 public:
  // Bind the persistence backend and load any previously saved state. Resolves its
  // own storage path under the user state directory; if that cannot be resolved the
  // service stays in-memory only (persistence is silently skipped).
  void Configure(const PersistenceService& persistence);

  void RecordProjectOpen(const std::filesystem::path& root);
  void RecordFileOpen(const std::filesystem::path& file,
                      const std::filesystem::path& project_root);

  // Write out a coalesced change that is still pending. Called at shutdown, which
  // is the only reliable point: the process leaves via quick_exit(), so a
  // destructor flush would not run. A no-op when nothing is pending.
  void FlushPendingSave() const;

  // How long a burst of opens folds into a single durable write. Short enough
  // that a crash loses at most this much of a list the user never edits, long
  // enough that "open 60 files" is one write instead of 60.
  static constexpr std::chrono::milliseconds SaveCoalesceWindow() {
    return std::chrono::milliseconds(750);
  }

  const std::vector<std::filesystem::path>& RecentProjects() const {
    return state_.recent_project_roots;
  }

  // Recent files opened under `project_root`, newest-first, capped at `limit`.
  std::vector<std::filesystem::path> RecentFilesFor(const std::filesystem::path& project_root,
                                                    std::size_t limit) const;

  // Recent projects/files filtered to those that still exist on disk, for the welcome
  // surface. The result is cached against a revision bumped whenever the MRU changes,
  // so paint and hover hit-testing never re-stat the recents every frame
  // (TD-2026-07-17A-014). A path that disappears while the MRU is unchanged stays
  // listed until the next MRU change re-validates — the perf-first cache contract.
  const std::vector<std::filesystem::path>& ExistingRecentProjects() const;
  const std::vector<std::filesystem::path>& ExistingRecentFilesFor(
      const std::filesystem::path& project_root, std::size_t limit) const;

  static constexpr std::size_t MaxProjects() { return 15; }
  static constexpr std::size_t MaxFiles() { return 60; }

 private:
  // Mark the state changed and write it if the coalescing window has elapsed.
  void Save() const;
  void WriteNow() const;

  const PersistenceService* persistence_ = nullptr;
  std::filesystem::path storage_path_;
  PersistedMruState state_;
  mutable bool save_pending_ = false;
  mutable std::chrono::steady_clock::time_point last_save_at_{};

  // Bumped on every MRU mutation (record/open, initial load); keys the validated
  // existing-path caches below.
  std::uint64_t revision_ = 0;
  mutable std::vector<std::filesystem::path> existing_projects_;
  mutable std::uint64_t existing_projects_revision_ = 0;
  mutable bool existing_projects_valid_ = false;
  mutable std::vector<std::filesystem::path> existing_files_;
  mutable std::filesystem::path existing_files_root_;
  mutable std::size_t existing_files_limit_ = 0;
  mutable std::uint64_t existing_files_revision_ = 0;
  mutable bool existing_files_valid_ = false;
};

}  // namespace microide::workspace
