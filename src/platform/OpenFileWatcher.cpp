#include "platform/OpenFileWatcher.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <system_error>
#include <utility>

#include "util/PosixFd.h"
#include "util/ThreadName.h"

namespace microide::platform {
namespace {

// A directory entry's content or existence: an in-place write (close-write), an
// atomic replace (moved-to), a create, a delete or move away, a mode change (an
// unreadable file reads differently); and the directory itself going.
constexpr std::uint32_t kMask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE |
                                IN_DELETE | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF |
                                IN_ONLYDIR;

}  // namespace

OpenFileWatcher::OpenFileWatcher(Callback callback) : callback_(std::move(callback)) {}

OpenFileWatcher::~OpenFileWatcher() {
  if (control_pipe_[1] >= 0) {
    const char stop = 1;
    while (write(control_pipe_[1], &stop, 1) < 0 && errno == EINTR) {
    }
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  util::CloseIfValid(control_pipe_[0]);
  util::CloseIfValid(control_pipe_[1]);
  util::CloseIfValid(inotify_fd_);
}

bool OpenFileWatcher::StartLocked() {
  if (inotify_fd_ >= 0) {
    return true;
  }
  inotify_fd_ = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
  if (inotify_fd_ < 0) {
    return false;
  }
  if (pipe2(control_pipe_, O_CLOEXEC) != 0) {
    util::CloseIfValid(inotify_fd_);
    inotify_fd_ = -1;
    control_pipe_[0] = control_pipe_[1] = -1;
    return false;
  }
  worker_ = std::thread([this]() {
    util::NameCurrentThread("open-file-watch");
    WorkerMain();
  });
  return true;
}

void OpenFileWatcher::SetFiles(std::uint64_t signature,
                               const std::vector<std::filesystem::path>& files) {
  std::lock_guard lock(mutex_);
  signature_.store(signature, std::memory_order_release);
  if (files.empty() && inotify_fd_ < 0) {
    return;
  }
  if (!StartLocked()) {
    return;
  }
  // Two spellings of one directory (a symlinked directory and its target, a
  // symlink loop) are one watch: inotify answers the same descriptor for the
  // same inode, so the entries are merged per descriptor, never replaced.
  std::unordered_map<int, std::vector<Entry>> next;
  const auto add = [&](const std::filesystem::path& file, const std::filesystem::path& reported) {
    const std::filesystem::path directory = file.parent_path();
    if (directory.empty() || !file.has_filename()) {
      return;
    }
    const int wd = inotify_add_watch(inotify_fd_, directory.c_str(), kMask);
    if (wd < 0) {
      return;  // gone, unreadable, or the watch limit: nothing to report on
    }
    std::vector<Entry>& entries = next[wd];
    const std::string name = file.filename().native();
    const bool known = std::any_of(entries.begin(), entries.end(), [&](const Entry& entry) {
      return entry.name == name && entry.reported == reported;
    });
    if (!known) {
      entries.push_back(Entry{.name = name, .reported = reported});
    }
  };
  for (const std::filesystem::path& file : files) {
    if (file.empty() || !file.is_absolute()) {
      continue;
    }
    add(file, file);
    std::error_code error;
    if (std::filesystem::is_symlink(file, error) && !error) {
      const std::filesystem::path target = std::filesystem::weakly_canonical(file, error);
      if (!error && target != file) {
        add(target, file);
      }
    }
  }
  for (const auto& [wd, entries] : entries_by_watch_) {
    if (next.find(wd) == next.end()) {
      inotify_rm_watch(inotify_fd_, wd);
    }
  }
  entries_by_watch_ = std::move(next);
}

void OpenFileWatcher::WorkerMain() {
  std::array<pollfd, 2> poll_fds{{
      pollfd{.fd = inotify_fd_, .events = POLLIN, .revents = 0},
      pollfd{.fd = control_pipe_[0], .events = POLLIN, .revents = 0},
  }};
  alignas(inotify_event) std::array<char, 16 * 1024> buffer{};
  while (true) {
    if (poll(poll_fds.data(), poll_fds.size(), -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }
    if ((poll_fds[1].revents & (POLLIN | POLLHUP)) != 0) {
      return;
    }
    if ((poll_fds[0].revents & POLLIN) == 0) {
      continue;
    }
    std::vector<std::filesystem::path> changed;
    const auto note = [&changed](const std::filesystem::path& path) {
      if (std::find(changed.begin(), changed.end(), path) == changed.end()) {
        changed.push_back(path);
      }
    };
    {
      std::lock_guard lock(mutex_);
      while (true) {
        const ssize_t length = read(inotify_fd_, buffer.data(), buffer.size());
        if (length <= 0) {
          break;  // EAGAIN: drained
        }
        for (ssize_t offset = 0; offset < length;) {
          const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
          offset += static_cast<ssize_t>(sizeof(inotify_event) + event->len);
          if ((event->mask & IN_Q_OVERFLOW) != 0) {
            // Events were dropped: any file may have changed.
            for (const auto& [wd, entries] : entries_by_watch_) {
              for (const Entry& entry : entries) {
                note(entry.reported);
              }
            }
            continue;
          }
          const auto found = entries_by_watch_.find(event->wd);
          if (found == entries_by_watch_.end()) {
            continue;
          }
          const bool directory_went = (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED)) != 0;
          if (directory_went) {
            // Every file in it is gone or elsewhere, and the watch with it; a
            // directory recreated under the same name (a branch switch, a stash
            // pop) is re-armed by the caller's next SetFiles.
            for (const Entry& entry : found->second) {
              note(entry.reported);
            }
            if ((event->mask & IN_IGNORED) != 0) {
              entries_by_watch_.erase(found);
            }
            signature_.store(0, std::memory_order_release);
            continue;
          }
          if (event->len == 0) {
            continue;
          }
          const std::string_view name(event->name, strnlen(event->name, event->len));
          for (const Entry& entry : found->second) {
            if (entry.name == name) {
              note(entry.reported);
            }
          }
        }
      }
    }
    if (!changed.empty() && callback_) {
      callback_(std::move(changed));
    }
  }
}

}  // namespace microide::platform
