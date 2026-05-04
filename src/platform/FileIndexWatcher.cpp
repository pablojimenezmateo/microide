#include "platform/FileIndexWatcher.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#include <fcntl.h>
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

bool IsGitMetadataRelativePath(const std::filesystem::path& relative_path) {
  const auto it = relative_path.begin();
  return it != relative_path.end() && *it == std::filesystem::path(".git");
}

bool ShouldSkipWatchedDirectory(const std::filesystem::path& directory) {
  return directory.filename() == ".git";
}

bool TryBuildTrackedRelativePath(const std::filesystem::path& absolute_path,
                                 const std::filesystem::path& root,
                                 std::filesystem::path& relative_path) {
  std::error_code rel_error;
  std::filesystem::path rel = std::filesystem::relative(absolute_path, root, rel_error);
  if (rel_error || rel.empty()) {
    return false;
  }
  rel = rel.lexically_normal();
  if (IsGitMetadataRelativePath(rel)) {
    return false;
  }
  relative_path = std::move(rel);
  return true;
}

// Build an initial IndexUpdateBatch by scanning root recursively.
IndexUpdateBatch BuildInitialBatch(const std::filesystem::path& root,
                                   const std::atomic<bool>* stop_requested = nullptr) {
  IndexUpdateBatch batch;
  batch.is_initial = true;

  std::error_code error;
  constexpr auto options = std::filesystem::directory_options::skip_permission_denied;
  for (std::filesystem::recursive_directory_iterator it(root, options, error), end;
       !error && it != end; it.increment(error)) {
    if (stop_requested != nullptr &&
        stop_requested->load(std::memory_order_acquire)) {
      break;
    }
    std::error_code status_error;
    const auto status = it->status(status_error);
    if (status_error) {
      continue;
    }
    if (std::filesystem::is_directory(status) && ShouldSkipWatchedDirectory(it->path())) {
      it.disable_recursion_pending();
      continue;
    }
    if (!std::filesystem::is_regular_file(status)) {
      continue;
    }
    const std::filesystem::path abs_path = it->path().lexically_normal();
    std::filesystem::path rel;
    if (!TryBuildTrackedRelativePath(abs_path, root, rel)) {
      continue;
    }

    std::error_code mtime_error;
    const auto mtime = std::filesystem::last_write_time(abs_path, mtime_error);
    std::error_code size_error;
    const auto size = std::filesystem::file_size(abs_path, size_error);

    IndexUpdateBatch::Change change;
    change.kind = IndexUpdateBatch::Kind::CreatedOrModified;
    change.entry.relative_path = rel;
    change.entry.mtime = mtime_error ? std::filesystem::file_time_type{} : mtime;
    change.entry.size = size_error ? 0 : size;
    batch.changes.push_back(std::move(change));
  }

  return batch;
}

}  // namespace

// ============================================================
// Platform-specific backends
// ============================================================

#if defined(__linux__)

struct FileIndexWatcher::Impl {
  std::atomic<bool> is_native{false};
  std::filesystem::path root;
  FileIndexWatcher::Callback callback;
  bool warned_fallback = false;

  // inotify backend
  int inotify_fd = -1;
  int control_pipe[2] = {-1, -1};
  std::map<int, std::filesystem::path> wd_to_path;  // watch descriptor -> abs path
  std::thread worker;
  bool native_active = false;

  // poll-fallback
  bool poll_mode = false;
  std::thread poll_worker;
  std::atomic<bool> stop_poll{false};
  std::thread initial_scan_worker;
  std::atomic<bool> stop_initial_scan{false};

  ~Impl() {
    StopNative();
    StopPoll();
  }

  void StopInitialScan() {
    stop_initial_scan.store(true, std::memory_order_release);
    if (initial_scan_worker.joinable()) {
      initial_scan_worker.join();
    }
    stop_initial_scan.store(false, std::memory_order_release);
  }

  void StopNative() {
    StopInitialScan();
    if (!native_active) {
      return;
    }
    native_active = false;
    // Signal worker
    if (control_pipe[1] >= 0) {
      const char byte = 0;
      while (write(control_pipe[1], &byte, 1) < 0 && errno == EINTR) {
      }
    }
    if (worker.joinable()) {
      worker.join();
    }
    CloseIfValid(control_pipe[0]);
    CloseIfValid(control_pipe[1]);
    control_pipe[0] = control_pipe[1] = -1;
    // Remove all watches and close inotify fd
    for (auto& [wd, path] : wd_to_path) {
      if (inotify_fd >= 0) {
        inotify_rm_watch(inotify_fd, wd);
      }
    }
    wd_to_path.clear();
    CloseIfValid(inotify_fd);
    inotify_fd = -1;
  }

  void StopPoll() {
    stop_poll.store(true, std::memory_order_release);
    if (poll_worker.joinable()) {
      poll_worker.join();
    }
  }

  void StartInitialScan() {
    StopInitialScan();
    stop_initial_scan.store(false, std::memory_order_release);
    initial_scan_worker = std::thread([this]() {
      IndexUpdateBatch initial = BuildInitialBatch(root, &stop_initial_scan);
      if (!stop_initial_scan.load(std::memory_order_acquire) && callback) {
        callback(std::move(initial));
      }
    });
  }

  // Add inotify watches recursively for dir and all subdirectories.
  bool AddWatchRecursive(const std::filesystem::path& dir) {
    if (ShouldSkipWatchedDirectory(dir)) {
      return true;
    }
    constexpr std::uint32_t kMask = IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF |
                                     IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_ATTRIB;
    const int wd = inotify_add_watch(inotify_fd, dir.c_str(), kMask);
    if (wd < 0) {
      if (errno == ENOSPC) {
        return false;  // watch limit exhausted
      }
      return true;  // ignore other errors (permission denied etc.)
    }
    wd_to_path[wd] = dir;

    std::error_code error;
    constexpr auto opts = std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(dir, opts, error), end;
         !error && it != end; it.increment(error)) {
      std::error_code status_error;
      const auto status = it->status(status_error);
      if (status_error) {
        continue;
      }
      if (std::filesystem::is_directory(status) && ShouldSkipWatchedDirectory(it->path())) {
        it.disable_recursion_pending();
        continue;
      }
      if (!std::filesystem::is_directory(status)) {
        continue;
      }
      const std::filesystem::path subdir = it->path().lexically_normal();
      const int sub_wd = inotify_add_watch(inotify_fd, subdir.c_str(), kMask);
      if (sub_wd < 0) {
        if (errno == ENOSPC) {
          return false;
        }
        continue;
      }
      wd_to_path[sub_wd] = subdir;
    }
    return true;
  }

  bool StartNative() {
    inotify_fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (inotify_fd < 0) {
      return false;
    }

    if (pipe(control_pipe) != 0) {
      CloseIfValid(inotify_fd);
      inotify_fd = -1;
      return false;
    }

    if (!AddWatchRecursive(root)) {
      // Watch limit exhausted
      CloseIfValid(control_pipe[0]);
      CloseIfValid(control_pipe[1]);
      control_pipe[0] = control_pipe[1] = -1;
      for (auto& [wd, path] : wd_to_path) {
        inotify_rm_watch(inotify_fd, wd);
      }
      wd_to_path.clear();
      CloseIfValid(inotify_fd);
      inotify_fd = -1;
      return false;
    }

    native_active = true;
    worker = std::thread([this]() { WorkerMain(); });
    return true;
  }

  void WorkerMain() {
    std::array<pollfd, 2> poll_fds{{
        pollfd{.fd = inotify_fd, .events = POLLIN, .revents = 0},
        pollfd{.fd = control_pipe[0], .events = POLLIN, .revents = 0},
    }};

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

      // Read all available inotify events
      std::vector<IndexUpdateBatch::Change> changes;
      {
        alignas(alignof(struct inotify_event)) std::array<char, 4096> buf{};
        while (true) {
          const ssize_t bytes_read = read(inotify_fd, buf.data(), buf.size());
          if (bytes_read <= 0) {
            if (bytes_read < 0 && errno == EINTR) {
              continue;
            }
            break;
          }

          ssize_t offset = 0;
          while (offset < bytes_read) {
            const struct inotify_event* ev =
                reinterpret_cast<const struct inotify_event*>(buf.data() + offset);
            offset += static_cast<ssize_t>(sizeof(struct inotify_event) + ev->len);

            const bool is_dir = (ev->mask & IN_ISDIR) != 0;

            // Find the directory for this watch descriptor
            const auto it = wd_to_path.find(ev->wd);
            if (it == wd_to_path.end()) {
              continue;
            }
            const std::filesystem::path& dir = it->second;

            if (ev->len > 0 && ev->name[0] != '\0') {
              const std::string name(ev->name, ::strnlen(ev->name, ev->len));
              const std::filesystem::path abs_path = (dir / name).lexically_normal();

              if (is_dir && (ev->mask & (IN_CREATE | IN_MOVED_TO)) != 0) {
                // New directory: add watches for it recursively
                AddWatchRecursive(abs_path);
                // No file change to report
              } else if (!is_dir) {
                std::filesystem::path rel;
                if (TryBuildTrackedRelativePath(abs_path, root, rel)) {
                  IndexUpdateBatch::Change change;
                  if ((ev->mask & (IN_DELETE | IN_MOVED_FROM | IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
                    change.kind = IndexUpdateBatch::Kind::Deleted;
                    change.entry.relative_path = rel;
                  } else {
                    change.kind = IndexUpdateBatch::Kind::CreatedOrModified;
                    change.entry.relative_path = rel;
                    std::error_code mtime_error;
                    const auto mtime = std::filesystem::last_write_time(abs_path, mtime_error);
                    std::error_code size_error;
                    const auto size = std::filesystem::file_size(abs_path, size_error);
                    change.entry.mtime =
                        mtime_error ? std::filesystem::file_time_type{} : mtime;
                    change.entry.size = size_error ? 0 : size;
                  }
                  changes.push_back(std::move(change));
                }
              }
            } else if ((ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
              // The watched directory itself was removed/moved; remove from map
              if (inotify_fd >= 0) {
                inotify_rm_watch(inotify_fd, ev->wd);
              }
              wd_to_path.erase(it);
            }
          }

          if (bytes_read < static_cast<ssize_t>(buf.size())) {
            break;
          }
        }
      }

      if (!changes.empty() && callback) {
        IndexUpdateBatch batch;
        batch.is_initial = false;
        batch.changes = std::move(changes);
        callback(std::move(batch));
      }
    }
  }

  void StartPollFallback() {
    poll_mode = true;
    stop_poll.store(false, std::memory_order_release);
    // Take initial snapshot
    poll_worker = std::thread([this]() { PollWorkerMain(); });
  }

  void PollWorkerMain() {
    // Build initial snapshot: map relative_path -> mtime+size
    std::map<std::filesystem::path, std::pair<std::filesystem::file_time_type, std::uintmax_t>>
        snapshot;

    auto build_snapshot =
        [&]() -> std::map<std::filesystem::path,
                          std::pair<std::filesystem::file_time_type, std::uintmax_t>> {
      std::map<std::filesystem::path,
               std::pair<std::filesystem::file_time_type, std::uintmax_t>>
          result;
      std::error_code error;
      constexpr auto opts = std::filesystem::directory_options::skip_permission_denied;
      for (std::filesystem::recursive_directory_iterator it(root, opts, error), end;
           !error && it != end; it.increment(error)) {
        std::error_code status_error;
        const auto status = it->status(status_error);
        if (status_error) {
          continue;
        }
        if (std::filesystem::is_directory(status) && ShouldSkipWatchedDirectory(it->path())) {
          it.disable_recursion_pending();
          continue;
        }
        if (!std::filesystem::is_regular_file(status)) {
          continue;
        }
        const std::filesystem::path abs_path = it->path().lexically_normal();
        std::filesystem::path rel;
        if (!TryBuildTrackedRelativePath(abs_path, root, rel)) {
          continue;
        }
        std::error_code mtime_error;
        const auto mtime = std::filesystem::last_write_time(abs_path, mtime_error);
        std::error_code size_error;
        const auto sz = std::filesystem::file_size(abs_path, size_error);
        result[rel] = {mtime_error ? std::filesystem::file_time_type{} : mtime,
                       size_error ? 0 : sz};
      }
      return result;
    };

    snapshot = build_snapshot();

    while (!stop_poll.load(std::memory_order_acquire)) {
      // Sleep 750ms in small increments to react to stop quickly
      for (int i = 0; i < 15 && !stop_poll.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (stop_poll.load(std::memory_order_acquire)) {
        break;
      }

      const auto current = build_snapshot();

      std::vector<IndexUpdateBatch::Change> changes;

      // Check for created/modified
      for (const auto& [rel, mtime_size] : current) {
        const auto prev_it = snapshot.find(rel);
        if (prev_it == snapshot.end()) {
          // Created
          IndexUpdateBatch::Change change;
          change.kind = IndexUpdateBatch::Kind::CreatedOrModified;
          change.entry.relative_path = rel;
          change.entry.mtime = mtime_size.first;
          change.entry.size = mtime_size.second;
          changes.push_back(std::move(change));
        } else if (prev_it->second != mtime_size) {
          // Modified
          IndexUpdateBatch::Change change;
          change.kind = IndexUpdateBatch::Kind::CreatedOrModified;
          change.entry.relative_path = rel;
          change.entry.mtime = mtime_size.first;
          change.entry.size = mtime_size.second;
          changes.push_back(std::move(change));
        }
      }

      // Check for deleted
      for (const auto& [rel, mtime_size] : snapshot) {
        if (current.find(rel) == current.end()) {
          IndexUpdateBatch::Change change;
          change.kind = IndexUpdateBatch::Kind::Deleted;
          change.entry.relative_path = rel;
          changes.push_back(std::move(change));
        }
      }

      snapshot = current;

      if (!changes.empty() && callback) {
        IndexUpdateBatch batch;
        batch.is_initial = false;
        batch.changes = std::move(changes);
        callback(std::move(batch));
      }
    }
  }
};

#elif defined(__APPLE__)

struct FileIndexWatcher::Impl {
  std::atomic<bool> is_native{false};
  std::filesystem::path root;
  FileIndexWatcher::Callback callback;

  // FSEvents backend
  FSEventStreamRef stream = nullptr;
  CFRunLoopRef run_loop = nullptr;
  std::thread worker;
  bool native_active = false;

  // poll-fallback
  bool poll_mode = false;
  std::thread poll_worker;
  std::atomic<bool> stop_poll{false};
  bool warned_fallback = false;

  ~Impl() {
    StopNative();
    StopPoll();
  }

  void StopNative() {
    if (!native_active) {
      return;
    }
    native_active = false;
    if (run_loop != nullptr) {
      CFRunLoopStop(run_loop);
    }
    if (worker.joinable()) {
      worker.join();
    }
    run_loop = nullptr;
    if (stream != nullptr) {
      FSEventStreamStop(stream);
      FSEventStreamInvalidate(stream);
      FSEventStreamRelease(stream);
      stream = nullptr;
    }
  }

  void StopPoll() {
    stop_poll.store(true, std::memory_order_release);
    if (poll_worker.joinable()) {
      poll_worker.join();
    }
  }

  static void FsEventsCallback(ConstFSEventStreamRef /*stream_ref*/,
                                void* client_callback_info,
                                size_t num_events,
                                void* event_paths,
                                const FSEventStreamEventFlags* event_flags,
                                const FSEventStreamEventId* /*event_ids*/) {
    Impl* self = static_cast<Impl*>(client_callback_info);
    if (self == nullptr || !self->callback) {
      return;
    }

    char** paths = static_cast<char**>(event_paths);
    std::vector<IndexUpdateBatch::Change> changes;

    for (size_t i = 0; i < num_events; ++i) {
      const FSEventStreamEventFlags flags = event_flags[i];
      const std::filesystem::path abs_path =
          std::filesystem::path(paths[i]).lexically_normal();

      const bool is_dir =
          (flags & kFSEventStreamEventFlagItemIsDir) != 0;
      if (is_dir) {
        continue;
      }

      std::filesystem::path rel;
      if (!TryBuildTrackedRelativePath(abs_path, self->root, rel)) {
        continue;
      }

      IndexUpdateBatch::Change change;
      if ((flags & kFSEventStreamEventFlagItemRemoved) != 0) {
        change.kind = IndexUpdateBatch::Kind::Deleted;
        change.entry.relative_path = rel;
      } else {
        change.kind = IndexUpdateBatch::Kind::CreatedOrModified;
        change.entry.relative_path = rel;
        std::error_code mtime_error;
        const auto mtime = std::filesystem::last_write_time(abs_path, mtime_error);
        std::error_code size_error;
        const auto size = std::filesystem::file_size(abs_path, size_error);
        change.entry.mtime = mtime_error ? std::filesystem::file_time_type{} : mtime;
        change.entry.size = size_error ? 0 : size;
      }
      changes.push_back(std::move(change));
    }

    if (!changes.empty()) {
      IndexUpdateBatch batch;
      batch.is_initial = false;
      batch.changes = std::move(changes);
      self->callback(std::move(batch));
    }
  }

  bool StartNative() {
    CFStringRef path_str =
        CFStringCreateWithCString(nullptr, root.c_str(), kCFStringEncodingUTF8);
    if (path_str == nullptr) {
      return false;
    }
    CFArrayRef paths_to_watch =
        CFArrayCreate(nullptr, reinterpret_cast<const void**>(&path_str), 1,
                      &kCFTypeArrayCallBacks);
    CFRelease(path_str);
    if (paths_to_watch == nullptr) {
      return false;
    }

    FSEventStreamContext ctx{};
    ctx.info = this;

    stream = FSEventStreamCreate(
        nullptr, &FsEventsCallback, &ctx, paths_to_watch,
        kFSEventStreamEventIdSinceNow,
        0.05,  // 50ms latency
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
    CFRelease(paths_to_watch);

    if (stream == nullptr) {
      return false;
    }

    native_active = true;
    worker = std::thread([this]() {
      run_loop = CFRunLoopGetCurrent();
      FSEventStreamScheduleWithRunLoop(stream, run_loop, kCFRunLoopDefaultMode);
      FSEventStreamStart(stream);
      CFRunLoopRun();
      FSEventStreamStop(stream);
      FSEventStreamUnscheduleFromRunLoop(stream, run_loop, kCFRunLoopDefaultMode);
    });

    return true;
  }

  void StartPollFallback() {
    poll_mode = true;
    stop_poll.store(false, std::memory_order_release);
    poll_worker = std::thread([this]() { PollWorkerMain(); });
  }

  void PollWorkerMain() {
    std::map<std::filesystem::path, std::pair<std::filesystem::file_time_type, std::uintmax_t>>
        snapshot;

    auto build_snapshot =
        [&]() -> std::map<std::filesystem::path,
                          std::pair<std::filesystem::file_time_type, std::uintmax_t>> {
      std::map<std::filesystem::path,
               std::pair<std::filesystem::file_time_type, std::uintmax_t>>
          result;
      std::error_code error;
      constexpr auto opts = std::filesystem::directory_options::skip_permission_denied;
      for (std::filesystem::recursive_directory_iterator it(root, opts, error), end;
           !error && it != end; it.increment(error)) {
        std::error_code status_error;
        const auto status = it->status(status_error);
        if (status_error) {
          continue;
        }
        if (std::filesystem::is_directory(status) && ShouldSkipWatchedDirectory(it->path())) {
          it.disable_recursion_pending();
          continue;
        }
        if (!std::filesystem::is_regular_file(status)) {
          continue;
        }
        const std::filesystem::path abs_path = it->path().lexically_normal();
        std::filesystem::path rel;
        if (!TryBuildTrackedRelativePath(abs_path, root, rel)) {
          continue;
        }
        std::error_code mtime_error;
        const auto mtime = std::filesystem::last_write_time(abs_path, mtime_error);
        std::error_code size_error;
        const auto sz = std::filesystem::file_size(abs_path, size_error);
        result[rel] = {mtime_error ? std::filesystem::file_time_type{} : mtime,
                       size_error ? 0 : sz};
      }
      return result;
    };

    snapshot = build_snapshot();

    while (!stop_poll.load(std::memory_order_acquire)) {
      for (int i = 0; i < 15 && !stop_poll.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (stop_poll.load(std::memory_order_acquire)) {
        break;
      }

      const auto current = build_snapshot();
      std::vector<IndexUpdateBatch::Change> changes;

      for (const auto& [rel, mtime_size] : current) {
        const auto prev_it = snapshot.find(rel);
        if (prev_it == snapshot.end() || prev_it->second != mtime_size) {
          IndexUpdateBatch::Change change;
          change.kind = IndexUpdateBatch::Kind::CreatedOrModified;
          change.entry.relative_path = rel;
          change.entry.mtime = mtime_size.first;
          change.entry.size = mtime_size.second;
          changes.push_back(std::move(change));
        }
      }
      for (const auto& [rel, mtime_size] : snapshot) {
        if (current.find(rel) == current.end()) {
          IndexUpdateBatch::Change change;
          change.kind = IndexUpdateBatch::Kind::Deleted;
          change.entry.relative_path = rel;
          changes.push_back(std::move(change));
        }
      }

      snapshot = current;

      if (!changes.empty() && callback) {
        IndexUpdateBatch batch;
        batch.is_initial = false;
        batch.changes = std::move(changes);
        callback(std::move(batch));
      }
    }
  }
};

#elif defined(_WIN32)

struct FileIndexWatcher::Impl {
  std::atomic<bool> is_native{false};
  std::filesystem::path root;
  FileIndexWatcher::Callback callback;
  bool warned_fallback = false;

  HANDLE dir_handle = INVALID_HANDLE_VALUE;
  HANDLE stop_event = nullptr;
  std::thread worker;
  bool native_active = false;

  bool poll_mode = false;
  std::thread poll_worker;
  std::atomic<bool> stop_poll{false};

  ~Impl() {
    StopNative();
    StopPoll();
  }

  void StopNative() {
    if (!native_active) {
      return;
    }
    native_active = false;
    if (stop_event != nullptr) {
      SetEvent(stop_event);
    }
    if (worker.joinable()) {
      worker.join();
    }
    if (dir_handle != INVALID_HANDLE_VALUE) {
      CloseHandle(dir_handle);
      dir_handle = INVALID_HANDLE_VALUE;
    }
    if (stop_event != nullptr) {
      CloseHandle(stop_event);
      stop_event = nullptr;
    }
  }

  void StopPoll() {
    stop_poll.store(true, std::memory_order_release);
    if (poll_worker.joinable()) {
      poll_worker.join();
    }
  }

  bool StartNative() {
    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event == nullptr) {
      return false;
    }

    dir_handle = CreateFileW(
        root.wstring().c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (dir_handle == INVALID_HANDLE_VALUE) {
      CloseHandle(stop_event);
      stop_event = nullptr;
      return false;
    }

    native_active = true;
    worker = std::thread([this]() { WorkerMain(); });
    return true;
  }

  void WorkerMain() {
    constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                              FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;
    constexpr DWORD kBufSize = 65536;
    std::vector<BYTE> buffer(kBufSize);
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
      return;
    }

    auto issue_read = [&]() -> bool {
      ResetEvent(overlapped.hEvent);
      DWORD bytes = 0;
      return ReadDirectoryChangesW(dir_handle, buffer.data(), kBufSize, TRUE, kFilter, &bytes,
                                   &overlapped, nullptr) != 0 ||
             GetLastError() == ERROR_IO_PENDING;
    };

    if (!issue_read()) {
      CloseHandle(overlapped.hEvent);
      return;
    }

    HANDLE wait_handles[2] = {stop_event, overlapped.hEvent};

    while (true) {
      const DWORD wait_result =
          WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);

      if (wait_result == WAIT_OBJECT_0) {
        // Stop requested
        CancelIo(dir_handle);
        CloseHandle(overlapped.hEvent);
        return;
      }
      if (wait_result != WAIT_OBJECT_0 + 1) {
        CloseHandle(overlapped.hEvent);
        return;
      }

      DWORD bytes_transferred = 0;
      if (!GetOverlappedResult(dir_handle, &overlapped, &bytes_transferred, FALSE) ||
          bytes_transferred == 0) {
        if (!issue_read()) {
          CloseHandle(overlapped.hEvent);
          return;
        }
        continue;
      }

      std::vector<IndexUpdateBatch::Change> changes;
      const BYTE* ptr = buffer.data();
      while (true) {
        const FILE_NOTIFY_INFORMATION* info =
            reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(ptr);
        const std::wstring wname(info->FileName, info->FileNameLength / sizeof(WCHAR));
        const std::filesystem::path rel = std::filesystem::path(wname).lexically_normal();
        if (rel.empty() || IsGitMetadataRelativePath(rel)) {
          if (info->NextEntryOffset == 0) {
            break;
          }
          ptr += info->NextEntryOffset;
          continue;
        }

        IndexUpdateBatch::Change change;
        if (info->Action == FILE_ACTION_REMOVED || info->Action == FILE_ACTION_RENAMED_OLD_NAME) {
          change.kind = IndexUpdateBatch::Kind::Deleted;
          change.entry.relative_path = rel;
          changes.push_back(std::move(change));
        } else {
          const std::filesystem::path abs_path = (root / rel).lexically_normal();
          std::error_code status_error;
          const auto status = std::filesystem::status(abs_path, status_error);
          if (!status_error && std::filesystem::is_regular_file(status)) {
            change.kind = IndexUpdateBatch::Kind::CreatedOrModified;
            change.entry.relative_path = rel;
            std::error_code mtime_error;
            const auto mtime = std::filesystem::last_write_time(abs_path, mtime_error);
            std::error_code size_error;
            const auto sz = std::filesystem::file_size(abs_path, size_error);
            change.entry.mtime = mtime_error ? std::filesystem::file_time_type{} : mtime;
            change.entry.size = size_error ? 0 : sz;
            changes.push_back(std::move(change));
          }
        }

        if (info->NextEntryOffset == 0) {
          break;
        }
        ptr += info->NextEntryOffset;
      }

      if (!changes.empty() && callback) {
        IndexUpdateBatch batch;
        batch.is_initial = false;
        batch.changes = std::move(changes);
        callback(std::move(batch));
      }

      if (!issue_read()) {
        CloseHandle(overlapped.hEvent);
        return;
      }
    }
  }

  void StartPollFallback() {
    poll_mode = true;
    stop_poll.store(false, std::memory_order_release);
    poll_worker = std::thread([this]() { PollWorkerMain(); });
  }

  void PollWorkerMain() {
    std::map<std::filesystem::path, std::pair<std::filesystem::file_time_type, std::uintmax_t>>
        snapshot;

    auto build_snapshot =
        [&]() -> std::map<std::filesystem::path,
                          std::pair<std::filesystem::file_time_type, std::uintmax_t>> {
      std::map<std::filesystem::path,
               std::pair<std::filesystem::file_time_type, std::uintmax_t>>
          result;
      std::error_code error;
      constexpr auto opts = std::filesystem::directory_options::skip_permission_denied;
      for (std::filesystem::recursive_directory_iterator it(root, opts, error), end;
           !error && it != end; it.increment(error)) {
        std::error_code status_error;
        const auto status = it->status(status_error);
        if (status_error) {
          continue;
        }
        if (std::filesystem::is_directory(status) && ShouldSkipWatchedDirectory(it->path())) {
          it.disable_recursion_pending();
          continue;
        }
        if (!std::filesystem::is_regular_file(status)) {
          continue;
        }
        const std::filesystem::path abs_path = it->path().lexically_normal();
        std::filesystem::path rel;
        if (!TryBuildTrackedRelativePath(abs_path, root, rel)) {
          continue;
        }
        std::error_code mtime_error;
        const auto mtime = std::filesystem::last_write_time(abs_path, mtime_error);
        std::error_code size_error;
        const auto sz = std::filesystem::file_size(abs_path, size_error);
        result[rel] = {mtime_error ? std::filesystem::file_time_type{} : mtime,
                       size_error ? 0 : sz};
      }
      return result;
    };

    snapshot = build_snapshot();

    while (!stop_poll.load(std::memory_order_acquire)) {
      for (int i = 0; i < 15 && !stop_poll.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (stop_poll.load(std::memory_order_acquire)) {
        break;
      }

      const auto current = build_snapshot();
      std::vector<IndexUpdateBatch::Change> changes;

      for (const auto& [rel, mtime_size] : current) {
        const auto prev_it = snapshot.find(rel);
        if (prev_it == snapshot.end() || prev_it->second != mtime_size) {
          IndexUpdateBatch::Change change;
          change.kind = IndexUpdateBatch::Kind::CreatedOrModified;
          change.entry.relative_path = rel;
          change.entry.mtime = mtime_size.first;
          change.entry.size = mtime_size.second;
          changes.push_back(std::move(change));
        }
      }
      for (const auto& [rel, mtime_size] : snapshot) {
        if (current.find(rel) == current.end()) {
          IndexUpdateBatch::Change change;
          change.kind = IndexUpdateBatch::Kind::Deleted;
          change.entry.relative_path = rel;
          changes.push_back(std::move(change));
        }
      }

      snapshot = current;

      if (!changes.empty() && callback) {
        IndexUpdateBatch batch;
        batch.is_initial = false;
        batch.changes = std::move(changes);
        callback(std::move(batch));
      }
    }
  }
};

#else

// Unknown platform: poll-only fallback
struct FileIndexWatcher::Impl {
  std::atomic<bool> is_native{false};
  std::filesystem::path root;
  FileIndexWatcher::Callback callback;
  bool warned_fallback = false;
  bool poll_mode = false;
  std::thread poll_worker;
  std::atomic<bool> stop_poll{false};

  ~Impl() { StopPoll(); }

  void StopPoll() {
    stop_poll.store(true, std::memory_order_release);
    if (poll_worker.joinable()) {
      poll_worker.join();
    }
  }

  void StartPollFallback() {
    poll_mode = true;
    stop_poll.store(false, std::memory_order_release);
    poll_worker = std::thread([this]() { PollWorkerMain(); });
  }

  void PollWorkerMain() {
    std::map<std::filesystem::path, std::pair<std::filesystem::file_time_type, std::uintmax_t>>
        snapshot;

    auto build_snapshot =
        [&]() -> std::map<std::filesystem::path,
                          std::pair<std::filesystem::file_time_type, std::uintmax_t>> {
      std::map<std::filesystem::path,
               std::pair<std::filesystem::file_time_type, std::uintmax_t>>
          result;
      std::error_code error;
      constexpr auto opts = std::filesystem::directory_options::skip_permission_denied;
      for (std::filesystem::recursive_directory_iterator it(root, opts, error), end;
           !error && it != end; it.increment(error)) {
        std::error_code status_error;
        const auto status = it->status(status_error);
        if (status_error) {
          continue;
        }
        if (std::filesystem::is_directory(status) && ShouldSkipWatchedDirectory(it->path())) {
          it.disable_recursion_pending();
          continue;
        }
        if (!std::filesystem::is_regular_file(status)) {
          continue;
        }
        const std::filesystem::path abs_path = it->path().lexically_normal();
        std::filesystem::path rel;
        if (!TryBuildTrackedRelativePath(abs_path, root, rel)) {
          continue;
        }
        std::error_code mtime_error;
        const auto mtime = std::filesystem::last_write_time(abs_path, mtime_error);
        std::error_code size_error;
        const auto sz = std::filesystem::file_size(abs_path, size_error);
        result[rel] = {mtime_error ? std::filesystem::file_time_type{} : mtime,
                       size_error ? 0 : sz};
      }
      return result;
    };

    snapshot = build_snapshot();

    while (!stop_poll.load(std::memory_order_acquire)) {
      for (int i = 0; i < 15 && !stop_poll.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (stop_poll.load(std::memory_order_acquire)) {
        break;
      }

      const auto current = build_snapshot();
      std::vector<IndexUpdateBatch::Change> changes;

      for (const auto& [rel, mtime_size] : current) {
        const auto prev_it = snapshot.find(rel);
        if (prev_it == snapshot.end() || prev_it->second != mtime_size) {
          IndexUpdateBatch::Change change;
          change.kind = IndexUpdateBatch::Kind::CreatedOrModified;
          change.entry.relative_path = rel;
          change.entry.mtime = mtime_size.first;
          change.entry.size = mtime_size.second;
          changes.push_back(std::move(change));
        }
      }
      for (const auto& [rel, mtime_size] : snapshot) {
        if (current.find(rel) == current.end()) {
          IndexUpdateBatch::Change change;
          change.kind = IndexUpdateBatch::Kind::Deleted;
          change.entry.relative_path = rel;
          changes.push_back(std::move(change));
        }
      }

      snapshot = current;

      if (!changes.empty() && callback) {
        IndexUpdateBatch batch;
        batch.is_initial = false;
        batch.changes = std::move(changes);
        callback(std::move(batch));
      }
    }
  }
};

#endif

// ============================================================
// FileIndexWatcher public API
// ============================================================

FileIndexWatcher::FileIndexWatcher() : impl_(std::make_unique<Impl>()) {}

FileIndexWatcher::~FileIndexWatcher() {
  Unwatch();
}

void FileIndexWatcher::SetCallback(Callback callback) {
  impl_->callback = std::move(callback);
}

bool FileIndexWatcher::Watch(const std::filesystem::path& root_path) {
  Unwatch();

  std::error_code error;
  const auto absolute_root = std::filesystem::absolute(root_path, error);
  if (error || !std::filesystem::exists(absolute_root) ||
      !std::filesystem::is_directory(absolute_root)) {
    return false;
  }
  impl_->root = absolute_root.lexically_normal();

#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
  // Try native backend
  if (impl_->StartNative()) {
    impl_->is_native.store(true, std::memory_order_release);
#if defined(__linux__)
    impl_->StartInitialScan();
#else
    // Emit initial batch first (on the calling thread, synchronous, before starting watcher)
    if (impl_->callback) {
      IndexUpdateBatch initial = BuildInitialBatch(impl_->root);
      impl_->callback(std::move(initial));
    }
#endif
    return true;
  }

  if (!impl_->warned_fallback) {
    impl_->warned_fallback = true;
    SDL_Log("FileIndexWatcher: native file events unavailable, falling back to poll mode");
  }
#endif

#if defined(__linux__)
  impl_->StartInitialScan();
#else
  // Emit initial batch first (on the calling thread, synchronous, before starting watcher)
  if (impl_->callback) {
    IndexUpdateBatch initial = BuildInitialBatch(impl_->root);
    impl_->callback(std::move(initial));
  }
#endif
  impl_->StartPollFallback();
  return true;
}

void FileIndexWatcher::Unwatch() {
#if defined(__linux__)
  impl_->StopNative();
  impl_->StopPoll();
#elif defined(__APPLE__)
  impl_->StopNative();
  impl_->StopPoll();
#elif defined(_WIN32)
  impl_->StopNative();
  impl_->StopPoll();
#else
  impl_->StopPoll();
#endif
  impl_->is_native.store(false, std::memory_order_release);
  impl_->root.clear();
}

bool FileIndexWatcher::IsNative() const {
  return impl_->is_native.load(std::memory_order_acquire);
}

}  // namespace microide::platform
