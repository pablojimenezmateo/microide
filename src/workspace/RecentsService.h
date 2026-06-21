#pragma once

#include <cstddef>
#include <filesystem>
#include <vector>

#include "workspace/WorkspacePersistenceFormat.h"

namespace microide::workspace {

class PersistenceService;

// Host-owned most-recently-used registry for projects and files. Holds the MRU in
// memory and persists it through PersistenceService after each change. Entries are
// newest-first and bounded by MaxProjects()/MaxFiles(). Recent files carry the
// project root they were opened under so the file finder can surface only the active
// project's recents. Opens are user-paced (a click or Enter), so saves run inline;
// the persisted record is tiny.
class RecentsService {
 public:
  // Bind the persistence backend and load any previously saved state. Resolves its
  // own storage path under the user state directory; if that cannot be resolved the
  // service stays in-memory only (persistence is silently skipped).
  void Configure(const PersistenceService& persistence);

  void RecordProjectOpen(const std::filesystem::path& root);
  void RecordFileOpen(const std::filesystem::path& file,
                      const std::filesystem::path& project_root);

  const std::vector<std::filesystem::path>& RecentProjects() const {
    return state_.recent_project_roots;
  }

  // Recent files opened under `project_root`, newest-first, capped at `limit`.
  std::vector<std::filesystem::path> RecentFilesFor(const std::filesystem::path& project_root,
                                                    std::size_t limit) const;

  static constexpr std::size_t MaxProjects() { return 15; }
  static constexpr std::size_t MaxFiles() { return 60; }

 private:
  void Save() const;

  const PersistenceService* persistence_ = nullptr;
  std::filesystem::path storage_path_;
  PersistedMruState state_;
};

}  // namespace microide::workspace
