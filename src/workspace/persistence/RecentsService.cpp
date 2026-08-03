#include "workspace/persistence/RecentsService.h"

#include <algorithm>
#include <utility>

#include "platform/AppDirectories.h"
#include "workspace/persistence/PersistenceService.h"

namespace microide::workspace {

namespace {

// Move `value` to the front of `entries`, dropping any prior occurrence, and cap the
// list at `max`. Newest-first ordering with deduplication.
void PromoteToFront(std::vector<std::filesystem::path>& entries,
                    const std::filesystem::path& value,
                    std::size_t max) {
  entries.erase(std::remove(entries.begin(), entries.end(), value), entries.end());
  entries.insert(entries.begin(), value);
  if (entries.size() > max) {
    entries.resize(max);
  }
}

}  // namespace

void RecentsService::Configure(const PersistenceService& persistence) {
  persistence_ = &persistence;
  const std::filesystem::path state_root =
      platform::ResolveAppDirectory(platform::UserDirectoryKind::State, "microide");
  storage_path_ = state_root.empty() ? std::filesystem::path{} : state_root / "recents";
  state_ = PersistedMruState{};
  save_pending_ = false;
  ++revision_;
  if (!storage_path_.empty()) {
    persistence_->LoadMruState(storage_path_, &state_);
    // Defensively clamp anything an older/corrupt record might have over-grown.
    if (state_.recent_project_roots.size() > MaxProjects()) {
      state_.recent_project_roots.resize(MaxProjects());
    }
    if (state_.recent_files.size() > MaxFiles()) {
      state_.recent_files.resize(MaxFiles());
    }
  }
}

void RecentsService::RecordProjectOpen(const std::filesystem::path& root) {
  if (root.empty()) {
    return;
  }
  // Re-opening what is already newest changes nothing, so it must not cost a
  // durable write. Reopening the active project is the common case.
  if (!state_.recent_project_roots.empty() && state_.recent_project_roots.front() == root) {
    return;
  }
  PromoteToFront(state_.recent_project_roots, root, MaxProjects());
  ++revision_;
  Save();
}

void RecentsService::RecordFileOpen(const std::filesystem::path& file,
                                    const std::filesystem::path& project_root) {
  if (file.empty()) {
    return;
  }
  auto& files = state_.recent_files;
  if (!files.empty() && files.front().path == file && files.front().project_root == project_root) {
    return;
  }
  files.erase(std::remove_if(files.begin(), files.end(),
                             [&](const PersistedRecentFile& entry) { return entry.path == file; }),
              files.end());
  files.insert(files.begin(), PersistedRecentFile{.path = file, .project_root = project_root});
  if (files.size() > MaxFiles()) {
    files.resize(MaxFiles());
  }
  ++revision_;
  Save();
}

std::vector<std::filesystem::path> RecentsService::RecentFilesFor(
    const std::filesystem::path& project_root, std::size_t limit) const {
  std::vector<std::filesystem::path> result;
  if (limit == 0) {
    return result;
  }
  for (const PersistedRecentFile& entry : state_.recent_files) {
    if (entry.project_root != project_root) {
      continue;
    }
    result.push_back(entry.path);
    if (result.size() >= limit) {
      break;
    }
  }
  return result;
}

const std::vector<std::filesystem::path>& RecentsService::ExistingRecentProjects() const {
  if (!existing_projects_valid_ || existing_projects_revision_ != revision_) {
    existing_projects_.clear();
    for (const std::filesystem::path& root : state_.recent_project_roots) {
      if (root.empty()) {
        continue;
      }
      std::error_code ec;
      if (std::filesystem::exists(root, ec)) {
        existing_projects_.push_back(root);
      }
    }
    existing_projects_revision_ = revision_;
    existing_projects_valid_ = true;
  }
  return existing_projects_;
}

const std::vector<std::filesystem::path>& RecentsService::ExistingRecentFilesFor(
    const std::filesystem::path& project_root, std::size_t limit) const {
  if (!existing_files_valid_ || existing_files_revision_ != revision_ ||
      existing_files_root_ != project_root || existing_files_limit_ != limit) {
    existing_files_.clear();
    // Mirror RecentFilesFor's cap-then-filter: take up to `limit` newest entries for
    // this root, then drop any that no longer exist (so the count matches the prior
    // BuildRecentRows exists-filter over a capped RecentFilesFor list).
    std::size_t matched = 0;
    for (const PersistedRecentFile& entry : state_.recent_files) {
      if (entry.project_root != project_root) {
        continue;
      }
      if (matched >= limit) {
        break;
      }
      ++matched;
      std::error_code ec;
      if (std::filesystem::exists(entry.path, ec)) {
        existing_files_.push_back(entry.path);
      }
    }
    existing_files_revision_ = revision_;
    existing_files_root_ = project_root;
    existing_files_limit_ = limit;
    existing_files_valid_ = true;
  }
  return existing_files_;
}

void RecentsService::Save() const {
  save_pending_ = true;
  const auto now = std::chrono::steady_clock::now();
  if (now - last_save_at_ < SaveCoalesceWindow()) {
    return;
  }
  last_save_at_ = now;
  WriteNow();
}

void RecentsService::FlushPendingSave() const {
  if (!save_pending_) {
    return;
  }
  last_save_at_ = std::chrono::steady_clock::now();
  WriteNow();
}

void RecentsService::WriteNow() const {
  save_pending_ = false;
  if (persistence_ == nullptr || storage_path_.empty()) {
    return;
  }
  persistence_->SaveMruState(storage_path_, state_);
}

}  // namespace microide::workspace
