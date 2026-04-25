#include "platform/FileWatcher.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <thread>

#if defined(__linux__)
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#endif

namespace microide::platform {

namespace {

#if defined(__linux__)

void CloseIfValid(int fd) {
  if (fd >= 0) {
    close(fd);
  }
}

std::vector<std::filesystem::path> CollectWatchPaths(
    const std::vector<std::filesystem::path>& roots,
    bool* polling_required) {
  std::vector<std::filesystem::path> watch_paths;
  bool requires_polling = false;

  for (const auto& root : roots) {
    if (root.empty()) {
      continue;
    }

    const PathType root_type = ReadPathType(root);
    if (root_type == PathType::Missing || root_type == PathType::Other) {
      requires_polling = true;
      continue;
    }

    watch_paths.push_back(root);
    if (root_type != PathType::Directory) {
      continue;
    }

    std::error_code error;
    constexpr auto options = std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(root, options, error), end;
         !error && it != end; it.increment(error)) {
      std::error_code status_error;
      if (it->is_directory(status_error) && !status_error) {
        watch_paths.push_back(it->path().lexically_normal());
      }
    }
    if (error) {
      requires_polling = true;
    }
  }

  std::sort(watch_paths.begin(), watch_paths.end());
  watch_paths.erase(std::unique(watch_paths.begin(), watch_paths.end()), watch_paths.end());
  if (polling_required != nullptr) {
    *polling_required = requires_polling;
  }
  return watch_paths;
}

constexpr std::uint32_t kWatchMask = IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE |
                                     IN_DELETE_SELF | IN_MOVE_SELF | IN_MOVED_FROM |
                                     IN_MOVED_TO;

#endif

}  // namespace

#if defined(__linux__)

struct FileTreeWatcher::NativeBackend {
  explicit NativeBackend(std::function<void()> on_change)
      : on_change_(std::move(on_change)) {}

  ~NativeBackend() {
    RequestStop();
    if (worker_.joinable()) {
      worker_.join();
    }
    CloseIfValid(control_pipe_[0]);
    CloseIfValid(control_pipe_[1]);
    CloseIfValid(inotify_fd_);
  }

  NativeBackend(const NativeBackend&) = delete;
  NativeBackend& operator=(const NativeBackend&) = delete;

  bool Start(const std::vector<std::filesystem::path>& watch_paths) {
    inotify_fd_ = inotify_init1(IN_CLOEXEC);
    if (inotify_fd_ < 0) {
      return false;
    }

    if (pipe(control_pipe_) != 0) {
      CloseIfValid(inotify_fd_);
      inotify_fd_ = -1;
      return false;
    }

    for (const auto& path : watch_paths) {
      if (inotify_add_watch(inotify_fd_, path.c_str(), kWatchMask) < 0) {
        return false;
      }
    }

    worker_ = std::thread([this]() { WorkerMain(); });
    return true;
  }

  void RequestStop() {
    if (control_pipe_[1] < 0) {
      return;
    }

    const std::array<char, 1> byte{{0}};
    while (write(control_pipe_[1], byte.data(), byte.size()) < 0) {
      if (errno != EINTR) {
        break;
      }
    }
  }

 private:
  void WorkerMain() {
    std::array<pollfd, 2> poll_fds{
        pollfd{.fd = inotify_fd_, .events = POLLIN, .revents = 0},
        pollfd{.fd = control_pipe_[0], .events = POLLIN, .revents = 0},
    };

    while (true) {
      const int poll_result = poll(poll_fds.data(), poll_fds.size(), -1);
      if (poll_result < 0) {
        if (errno == EINTR) {
          continue;
        }
        return;
      }

      if ((poll_fds[1].revents & (POLLIN | POLLHUP)) != 0) {
        return;
      }
      if ((poll_fds[0].revents & (POLLIN | POLLHUP)) == 0) {
        continue;
      }

      std::array<char, 4096> buffer{};
      bool changed = false;
      while (true) {
        const ssize_t bytes_read = read(inotify_fd_, buffer.data(), buffer.size());
        if (bytes_read > 0) {
          changed = true;
          if (bytes_read < static_cast<ssize_t>(buffer.size())) {
            break;
          }
          continue;
        }
        if (bytes_read < 0 && errno == EINTR) {
          continue;
        }
        break;
      }

      if (changed && on_change_) {
        on_change_();
      }
    }
  }

  std::function<void()> on_change_;
  int inotify_fd_ = -1;
  int control_pipe_[2] = {-1, -1};
  std::thread worker_;
};

#endif

FileTreeWatcher::FileTreeWatcher(std::chrono::milliseconds poll_interval)
    : poll_interval_(std::max(poll_interval, std::chrono::milliseconds::zero())) {}

FileTreeWatcher::~FileTreeWatcher() = default;

void FileTreeWatcher::SetPollInterval(std::chrono::milliseconds poll_interval) {
  std::scoped_lock lock(mutex_);
  poll_interval_ = std::max(poll_interval, std::chrono::milliseconds::zero());
  ResetNextPollAt();
}

void FileTreeWatcher::SetWakeCallback(WakeCallback callback) {
  std::scoped_lock lock(mutex_);
  wake_callback_ = std::move(callback);
}

void FileTreeWatcher::SetRoots(std::vector<std::filesystem::path> roots) {
  std::vector<std::filesystem::path> normalized_roots;
  normalized_roots.reserve(roots.size());
  for (auto& root : roots) {
    if (!root.empty()) {
      normalized_roots.push_back(root.lexically_normal());
    }
  }
  std::sort(normalized_roots.begin(), normalized_roots.end());
  normalized_roots.erase(std::unique(normalized_roots.begin(), normalized_roots.end()),
                         normalized_roots.end());
  const std::vector<TreeSnapshotEntry> snapshot = CaptureTreeSnapshot(normalized_roots);

  std::scoped_lock lock(mutex_);
  roots_ = std::move(normalized_roots);
  snapshot_ = snapshot;
  pending_change_ = false;
  RefreshNativeBackendLocked();
  ResetNextPollAt();
}

void FileTreeWatcher::Clear() {
  std::scoped_lock lock(mutex_);
  roots_.clear();
  snapshot_.clear();
  pending_change_ = false;
  polling_required_ = true;
  native_backend_.reset();
  next_poll_at_ = std::chrono::steady_clock::time_point::min();
}

std::optional<std::chrono::milliseconds> FileTreeWatcher::NextPollDelay() const {
  std::scoped_lock lock(mutex_);
  if (roots_.empty()) {
    return std::nullopt;
  }
  if (pending_change_) {
    if (wake_callback_ && !polling_required_) {
      return std::nullopt;
    }
    return std::chrono::milliseconds::zero();
  }
  if (poll_interval_ == std::chrono::milliseconds::zero()) {
    return std::chrono::milliseconds::zero();
  }
  if (!polling_required_) {
    return std::nullopt;
  }

  const auto now = std::chrono::steady_clock::now();
  if (now >= next_poll_at_) {
    return std::chrono::milliseconds::zero();
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(next_poll_at_ - now);
}

bool FileTreeWatcher::Poll() {
  std::vector<std::filesystem::path> roots;
  std::vector<TreeSnapshotEntry> previous_snapshot;
  {
    std::scoped_lock lock(mutex_);
    if (roots_.empty()) {
      return false;
    }
    roots = roots_;
    previous_snapshot = snapshot_;
    pending_change_ = false;
  }

  const std::vector<TreeSnapshotEntry> current_snapshot = CaptureTreeSnapshot(roots);
  const bool changed = current_snapshot != previous_snapshot;

  std::scoped_lock lock(mutex_);
  if (roots != roots_) {
    return changed;
  }
  snapshot_ = current_snapshot;
  RefreshNativeBackendLocked();
  ResetNextPollAt();
  return changed;
}

void FileTreeWatcher::RefreshNativeBackendLocked() {
#if defined(__linux__)
  bool requires_polling = false;
  const std::vector<std::filesystem::path> watch_paths = CollectWatchPaths(roots_, &requires_polling);
  native_backend_.reset();
  polling_required_ = true;
  if (watch_paths.empty()) {
    return;
  }

  auto backend = std::make_unique<NativeBackend>([this]() { NotifyWake(); });
  if (!backend->Start(watch_paths)) {
    return;
  }

  native_backend_ = std::move(backend);
  polling_required_ = requires_polling;
#else
  native_backend_.reset();
  polling_required_ = true;
#endif
}

void FileTreeWatcher::ResetNextPollAt() {
  next_poll_at_ = std::chrono::steady_clock::now() + poll_interval_;
}

void FileTreeWatcher::NotifyWake() {
  WakeCallback callback;
  {
    std::scoped_lock lock(mutex_);
    if (pending_change_) {
      return;
    }
    pending_change_ = true;
    callback = wake_callback_;
  }
  if (callback) {
    callback();
  }
}

}  // namespace microide::platform
