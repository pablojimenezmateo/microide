#include "workspace/RecentsService.h"

#include <algorithm>
#include <utility>

#include "platform/AppDirectories.h"
#include "workspace/PersistenceService.h"

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
  PromoteToFront(state_.recent_project_roots, root, MaxProjects());
  Save();
}

void RecentsService::RecordFileOpen(const std::filesystem::path& file,
                                    const std::filesystem::path& project_root) {
  if (file.empty()) {
    return;
  }
  auto& files = state_.recent_files;
  files.erase(std::remove_if(files.begin(), files.end(),
                             [&](const PersistedRecentFile& entry) { return entry.path == file; }),
              files.end());
  files.insert(files.begin(), PersistedRecentFile{.path = file, .project_root = project_root});
  if (files.size() > MaxFiles()) {
    files.resize(MaxFiles());
  }
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

void RecentsService::Save() const {
  if (persistence_ == nullptr || storage_path_.empty()) {
    return;
  }
  persistence_->SaveMruState(storage_path_, state_);
}

}  // namespace microide::workspace
