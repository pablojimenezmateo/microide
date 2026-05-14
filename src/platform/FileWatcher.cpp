#include "platform/FileWatcher.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <thread>

#if defined(__linux__)
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <fcntl.h>
#include <sys/event.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace microide::platform {

namespace {

#if defined(__linux__) || defined(__APPLE__)
void CloseIfValid(int fd) {
  if (fd >= 0) {
    close(fd);
  }
}
#endif

PathType PathTypeFromStatus(const std::filesystem::file_status& status) {
  switch (status.type()) {
    case std::filesystem::file_type::none:
    case std::filesystem::file_type::not_found:
      return PathType::Missing;
    case std::filesystem::file_type::regular:
      return PathType::RegularFile;
    case std::filesystem::file_type::directory:
      return PathType::Directory;
    default:
      return PathType::Other;
  }
}

std::vector<std::filesystem::path> CollectRecursiveWatchPaths(
    const std::vector<std::filesystem::path>& roots,
    const TreeTraversalFilter& filter,
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
      const PathType type = PathTypeFromStatus(it->status(status_error));
      if (status_error) {
        continue;
      }
      const std::filesystem::path path = it->path().lexically_normal();
      if (filter && !filter(path, type)) {
        if (type == PathType::Directory) {
          it.disable_recursion_pending();
        }
        continue;
      }
      if (type == PathType::Directory) {
        watch_paths.push_back(path);
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

#if defined(_WIN32)

std::vector<std::filesystem::path> CollectWindowsWatchRoots(
    const std::vector<std::filesystem::path>& roots,
    bool* polling_required) {
  std::vector<std::filesystem::path> watch_roots;
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

    if (root_type == PathType::Directory) {
      watch_roots.push_back(root);
      continue;
    }

    const std::filesystem::path parent = root.parent_path();
    if (parent.empty() || ReadPathType(parent) != PathType::Directory) {
      requires_polling = true;
      continue;
    }
    watch_roots.push_back(parent.lexically_normal());
  }

  std::sort(watch_roots.begin(), watch_roots.end());
  watch_roots.erase(std::unique(watch_roots.begin(), watch_roots.end()), watch_roots.end());
  if (polling_required != nullptr) {
    *polling_required = requires_polling;
  }
  return watch_roots;
}

#endif

#if defined(__linux__)
constexpr std::uint32_t kLinuxWatchMask = IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE |
                                          IN_DELETE_SELF | IN_MOVE_SELF | IN_MOVED_FROM |
                                          IN_MOVED_TO;
#elif defined(__APPLE__)
constexpr std::uint32_t kMacWatchMask = NOTE_ATTRIB | NOTE_DELETE | NOTE_EXTEND | NOTE_LINK |
                                        NOTE_RENAME | NOTE_REVOKE | NOTE_WRITE;
#elif defined(_WIN32)
constexpr DWORD kWindowsWatchFilter = FILE_NOTIFY_CHANGE_DIR_NAME |
                                      FILE_NOTIFY_CHANGE_FILE_NAME |
                                      FILE_NOTIFY_CHANGE_LAST_WRITE |
                                      FILE_NOTIFY_CHANGE_ATTRIBUTES |
                                      FILE_NOTIFY_CHANGE_SIZE |
                                      FILE_NOTIFY_CHANGE_CREATION;
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
      if (inotify_add_watch(inotify_fd_, path.c_str(), kLinuxWatchMask) < 0) {
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

#elif defined(__APPLE__)

struct FileTreeWatcher::NativeBackend {
  explicit NativeBackend(std::function<void()> on_change)
      : on_change_(std::move(on_change)) {}

  ~NativeBackend() {
    RequestStop();
    if (worker_.joinable()) {
      worker_.join();
    }
    for (int fd : watch_fds_) {
      CloseIfValid(fd);
    }
    CloseIfValid(control_pipe_[0]);
    CloseIfValid(control_pipe_[1]);
    CloseIfValid(kqueue_fd_);
  }

  NativeBackend(const NativeBackend&) = delete;
  NativeBackend& operator=(const NativeBackend&) = delete;

  bool Start(const std::vector<std::filesystem::path>& watch_paths) {
    kqueue_fd_ = kqueue();
    if (kqueue_fd_ < 0) {
      return false;
    }
    if (pipe(control_pipe_) != 0) {
      return false;
    }

    struct kevent control_event{};
    EV_SET(&control_event, control_pipe_[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    if (kevent(kqueue_fd_, &control_event, 1, nullptr, 0, nullptr) != 0) {
      return false;
    }

    for (const auto& path : watch_paths) {
      const int fd = open(path.c_str(), O_EVTONLY);
      if (fd < 0) {
        return false;
      }
      watch_fds_.push_back(fd);

      struct kevent change_event{};
      EV_SET(&change_event, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, kMacWatchMask, 0, nullptr);
      if (kevent(kqueue_fd_, &change_event, 1, nullptr, 0, nullptr) != 0) {
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
    std::array<struct kevent, 8> events{};
    while (true) {
      const int event_count = kevent(kqueue_fd_, nullptr, 0, events.data(),
                                     static_cast<int>(events.size()), nullptr);
      if (event_count < 0) {
        if (errno == EINTR) {
          continue;
        }
        return;
      }

      bool changed = false;
      for (int index = 0; index < event_count; ++index) {
        const auto& event = events[static_cast<std::size_t>(index)];
        if (event.filter == EVFILT_READ &&
            static_cast<int>(event.ident) == control_pipe_[0]) {
          return;
        }
        if (event.filter == EVFILT_VNODE) {
          changed = true;
        }
      }

      if (changed && on_change_) {
        on_change_();
      }
    }
  }

  std::function<void()> on_change_;
  int kqueue_fd_ = -1;
  int control_pipe_[2] = {-1, -1};
  std::vector<int> watch_fds_;
  std::thread worker_;
};

#elif defined(_WIN32)

struct FileTreeWatcher::NativeBackend {
  explicit NativeBackend(std::function<void()> on_change)
      : on_change_(std::move(on_change)) {}

  ~NativeBackend() {
    RequestStop();
    if (worker_.joinable()) {
      worker_.join();
    }
    for (HANDLE handle : notification_handles_) {
      if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        FindCloseChangeNotification(handle);
      }
    }
    if (stop_event_ != nullptr) {
      CloseHandle(stop_event_);
    }
    if (ready_event_ != nullptr) {
      CloseHandle(ready_event_);
    }
  }

  NativeBackend(const NativeBackend&) = delete;
  NativeBackend& operator=(const NativeBackend&) = delete;

  bool Start(const std::vector<std::filesystem::path>& watch_roots) {
    if (watch_roots.size() + 1 > MAXIMUM_WAIT_OBJECTS) {
      return false;
    }

    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event_ == nullptr) {
      return false;
    }
    ready_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ready_event_ == nullptr) {
      return false;
    }

    for (const auto& root : watch_roots) {
      const HANDLE notification = FindFirstChangeNotificationW(
          root.wstring().c_str(), TRUE, kWindowsWatchFilter);
      if (notification == INVALID_HANDLE_VALUE) {
        return false;
      }
      notification_handles_.push_back(notification);
    }

    worker_ = std::thread([this]() { WorkerMain(); });
    const DWORD ready_result = WaitForSingleObject(ready_event_, 1000);
    if (ready_result != WAIT_OBJECT_0) {
      RequestStop();
      if (worker_.joinable()) {
        worker_.join();
      }
      return false;
    }
    return true;
  }

  void RequestStop() {
    if (stop_event_ != nullptr) {
      SetEvent(stop_event_);
    }
  }

 private:
  void WorkerMain() {
    std::vector<HANDLE> wait_handles;
    wait_handles.reserve(notification_handles_.size() + 1);
    wait_handles.push_back(stop_event_);
    wait_handles.insert(wait_handles.end(), notification_handles_.begin(),
                        notification_handles_.end());
    if (ready_event_ != nullptr) {
      SetEvent(ready_event_);
    }

    while (true) {
      const DWORD wait_result =
          WaitForMultipleObjects(static_cast<DWORD>(wait_handles.size()),
                                 wait_handles.data(), FALSE, INFINITE);
      if (wait_result == WAIT_OBJECT_0) {
        return;
      }
      if (wait_result == WAIT_FAILED || wait_result == WAIT_TIMEOUT) {
        return;
      }

      const std::size_t index = static_cast<std::size_t>(wait_result - WAIT_OBJECT_0 - 1);
      if (index >= notification_handles_.size()) {
        return;
      }
      if (!FindNextChangeNotification(notification_handles_[index])) {
        return;
      }
      if (on_change_) {
        on_change_();
      }
    }
  }

  std::function<void()> on_change_;
  HANDLE stop_event_ = nullptr;
  HANDLE ready_event_ = nullptr;
  std::vector<HANDLE> notification_handles_;
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

void FileTreeWatcher::SetDeferInitialSnapshot(bool defer) {
  std::scoped_lock lock(mutex_);
  defer_initial_snapshot_ = defer;
}

void FileTreeWatcher::SetEntryFilter(TreeTraversalFilter filter) {
  std::scoped_lock lock(mutex_);
  entry_filter_ = std::move(filter);
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

  TreeTraversalFilter filter;
  bool capture_snapshot = true;
  std::unique_ptr<NativeBackend> old_backend;
  {
    std::scoped_lock lock(mutex_);
    filter = entry_filter_;
    roots_ = normalized_roots;
    pending_change_ = false;
    old_backend = RefreshNativeBackendLocked();
    capture_snapshot =
        !defer_initial_snapshot_ || polling_required_ || !static_cast<bool>(wake_callback_);
    if (!capture_snapshot) {
      snapshot_.clear();
      snapshot_valid_ = false;
      ResetNextPollAt();
      return;
    }
  }

  const std::vector<TreeSnapshotEntry> snapshot =
      capture_snapshot ? CaptureTreeSnapshot(normalized_roots, filter)
                       : std::vector<TreeSnapshotEntry>{};

  std::scoped_lock lock(mutex_);
  if (roots_ != normalized_roots) {
    return;
  }
  snapshot_ = snapshot;
  snapshot_valid_ = true;
  pending_change_ = false;
  ResetNextPollAt();
}

void FileTreeWatcher::Clear() {
  std::unique_ptr<NativeBackend> old_backend;
  std::scoped_lock lock(mutex_);
  roots_.clear();
  snapshot_.clear();
  snapshot_valid_ = false;
  pending_change_ = false;
  polling_required_ = true;
  old_backend = std::move(native_backend_);
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
  TreeTraversalFilter filter;
  bool snapshot_valid = false;
  bool polling_required = true;
  bool pending_change = false;
  {
    std::scoped_lock lock(mutex_);
    if (roots_.empty()) {
      return false;
    }
    roots = roots_;
    previous_snapshot = snapshot_;
    filter = entry_filter_;
    snapshot_valid = snapshot_valid_;
    polling_required = polling_required_;
    pending_change = pending_change_;
    pending_change_ = false;
  }

  if (!snapshot_valid && !polling_required) {
    if (!pending_change) {
      return false;
    }
    const std::vector<TreeSnapshotEntry> current_snapshot = CaptureTreeSnapshot(roots, filter);
    std::unique_ptr<NativeBackend> old_backend;
    std::scoped_lock lock(mutex_);
    if (roots != roots_) {
      return false;
    }
    snapshot_ = current_snapshot;
    snapshot_valid_ = true;
    old_backend = RefreshNativeBackendLocked();
    ResetNextPollAt();
    return false;
  }

  const std::vector<TreeSnapshotEntry> current_snapshot = CaptureTreeSnapshot(roots, filter);
  const bool changed = current_snapshot != previous_snapshot;

  std::unique_ptr<NativeBackend> old_backend;
  std::scoped_lock lock(mutex_);
  if (roots != roots_) {
    return changed;
  }
  snapshot_ = current_snapshot;
  snapshot_valid_ = true;
  old_backend = RefreshNativeBackendLocked();
  ResetNextPollAt();
  return changed;
}

std::unique_ptr<FileTreeWatcher::NativeBackend> FileTreeWatcher::RefreshNativeBackendLocked() {
  std::unique_ptr<NativeBackend> old_backend = std::move(native_backend_);
  polling_required_ = true;

#if defined(__linux__) || defined(__APPLE__)
  bool requires_polling = false;
  const std::vector<std::filesystem::path> watch_paths =
      CollectRecursiveWatchPaths(roots_, entry_filter_, &requires_polling);
  if (watch_paths.empty()) {
    polling_required_ = requires_polling || !roots_.empty();
    return old_backend;
  }

  auto backend = std::make_unique<NativeBackend>([this]() { NotifyWake(); });
  if (!backend->Start(watch_paths)) {
    polling_required_ = true;
    return old_backend;
  }

  native_backend_ = std::move(backend);
  polling_required_ = requires_polling;
  return old_backend;
#elif defined(_WIN32)
  bool requires_polling = false;
  const std::vector<std::filesystem::path> watch_roots =
      CollectWindowsWatchRoots(roots_, &requires_polling);
  if (watch_roots.empty()) {
    polling_required_ = requires_polling || !roots_.empty();
    return old_backend;
  }

  auto backend = std::make_unique<NativeBackend>([this]() { NotifyWake(); });
  if (!backend->Start(watch_roots)) {
    polling_required_ = true;
    return old_backend;
  }

  native_backend_ = std::move(backend);
  polling_required_ = requires_polling;
  return old_backend;
#else
  polling_required_ = true;
  return old_backend;
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
