#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "workspace/persistence/WorkspacePersistenceFormat.h"

namespace microide::workspace {

class PersistenceService;

class RecentsService {
 public:
  void Configure(const PersistenceService& persistence);

  void RecordProjectOpen(const project::ProjectId& id);
  void ForgetProject(const project::ProjectId& id);
  void RecordFileOpen(const std::filesystem::path& file,
                      const std::filesystem::path& project_root);

  void FlushPendingSave() const;

  static constexpr std::chrono::milliseconds SaveCoalesceWindow() {
    return std::chrono::milliseconds(750);
  }

  const std::vector<project::ProjectId>& RecentProjects() const {
    return state_.recent_projects;
  }

  std::vector<std::filesystem::path> RecentFilesFor(const std::filesystem::path& project_root,
                                                    std::size_t limit) const;

  const std::vector<project::ProjectId>& ExistingRecentProjects() const;
  const std::vector<std::filesystem::path>& ExistingRecentFilesFor(
      const std::filesystem::path& project_root, std::size_t limit) const;

  std::uint64_t revision() const { return revision_; }
  std::uint64_t instance_id() const { return instance_id_; }

  static constexpr std::size_t MaxProjects() { return 15; }
  static constexpr std::size_t MaxFiles() { return 60; }

 private:
  void Save() const;
  void WriteNow() const;

  const PersistenceService* persistence_ = nullptr;
  std::filesystem::path storage_path_;
  PersistedMruState state_;
  mutable bool save_pending_ = false;
  mutable std::chrono::steady_clock::time_point last_save_at_{};

  static std::uint64_t NextInstanceId();
  const std::uint64_t instance_id_ = NextInstanceId();

  std::uint64_t revision_ = 0;
  mutable std::vector<project::ProjectId> existing_projects_;
  mutable std::uint64_t existing_projects_revision_ = 0;
  mutable bool existing_projects_valid_ = false;
  mutable std::vector<std::filesystem::path> existing_files_;
  mutable std::filesystem::path existing_files_root_;
  mutable std::size_t existing_files_limit_ = 0;
  mutable std::uint64_t existing_files_revision_ = 0;
  mutable bool existing_files_valid_ = false;
};

}
