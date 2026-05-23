#include "project/GitRepositoryMetadataTracker.h"

#include <system_error>

namespace microide::project {
namespace {

std::optional<std::uint64_t> FileModificationTick(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    return std::nullopt;
  }
  const auto tick = std::filesystem::last_write_time(path, error);
  if (error) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(tick.time_since_epoch().count());
}

}  // namespace

void GitRepositoryMetadataTracker::Reset() {
  project_root_.clear();
  baseline_.reset();
}

void GitRepositoryMetadataTracker::SetProjectRoot(const std::filesystem::path& project_root) {
  project_root_ = project_root.lexically_normal();
  baseline_ = ReadCurrentTicks();
}

std::vector<RepositoryChange> GitRepositoryMetadataTracker::SampleChanges() {
  const std::optional<MetadataTick> current = ReadCurrentTicks();
  if (!current.has_value() || !baseline_.has_value()) {
    baseline_ = current;
    return {};
  }

  std::vector<RepositoryChange> changes;
  if (current->head != baseline_->head) {
    changes.push_back(RepositoryChange{.kind = RepositoryChangeKind::HeadChanged});
  }
  if (current->index != baseline_->index) {
    changes.push_back(RepositoryChange{.kind = RepositoryChangeKind::IndexChanged});
  }
  baseline_ = current;
  return changes;
}

std::optional<GitRepositoryMetadataTracker::MetadataTick>
GitRepositoryMetadataTracker::ReadCurrentTicks() const {
  if (project_root_.empty()) {
    return std::nullopt;
  }

  const std::filesystem::path git_dir = project_root_ / ".git";
  std::error_code error;
  if (!std::filesystem::exists(git_dir, error)) {
    return std::nullopt;
  }

  MetadataTick tick;
  if (const auto head_tick = FileModificationTick(git_dir / "HEAD"); head_tick.has_value()) {
    tick.head = *head_tick;
  }
  if (const auto index_tick = FileModificationTick(git_dir / "index"); index_tick.has_value()) {
    tick.index = *index_tick;
  }
  return tick;
}

}  // namespace microide::project
