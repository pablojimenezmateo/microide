#include "platform/OpenFileWatcher.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <iterator>
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
  EntriesByWatch next;
  WaitingByWatch next_waiting;
  for (const std::filesystem::path& file : files) {
    if (file.empty() || !file.is_absolute()) {
      continue;
    }
    (void)ArmLocked(file, file, next, next_waiting);
    std::error_code error;
    if (std::filesystem::is_symlink(file, error) && !error) {
      const std::filesystem::path target = std::filesystem::weakly_canonical(file, error);
      if (!error && target != file) {
        (void)ArmLocked(target, file, next, next_waiting);
      }
    }
  }
  EntriesByWatch before_entries = std::exchange(entries_by_watch_, std::move(next));
  WaitingByWatch before_waiting = std::exchange(waiting_by_watch_, std::move(next_waiting));
  DropUnusedWatchesLocked(before_entries, before_waiting);
}

OpenFileWatcher::Armed OpenFileWatcher::ArmLocked(const std::filesystem::path& file,
                                                  const std::filesystem::path& reported,
                                                  EntriesByWatch& entries,
                                                  WaitingByWatch& waiting) {
  const std::filesystem::path directory = file.parent_path();
  if (directory.empty() || !file.has_filename()) {
    return Armed::Unwatchable;
  }
  // Bounded by the path's depth: each pass either watches the directory, waits
  // under an ancestor that holds, or found the directory back and goes again.
  for (std::size_t pass = 0, depth = static_cast<std::size_t>(
                                 std::distance(directory.begin(), directory.end()));
       pass <= depth; ++pass) {
    const int wd = inotify_add_watch(inotify_fd_, directory.c_str(), kMask);
    if (wd >= 0) {
      // Two spellings of one directory (a symlinked directory and its target, a
      // symlink loop) are one watch: inotify answers the same descriptor for the
      // same inode, so the entries are merged per descriptor, never replaced.
      std::vector<Entry>& listed = entries[wd];
      const std::string name = file.filename().native();
      const bool known = std::any_of(listed.begin(), listed.end(), [&](const Entry& entry) {
        return entry.name == name && entry.reported == reported;
      });
      if (!known) {
        listed.push_back(Entry{.name = name, .reported = reported});
      }
      return Armed::Watching;
    }
    if (errno != ENOENT && errno != ENOTDIR) {
      return Armed::Unwatchable;  // unreadable, or the watch limit: nothing to report on
    }
    // The directory is not there: wait for it from the nearest one that is.
    int ancestor_wd = -1;
    for (std::filesystem::path ancestor = directory.parent_path();
         !ancestor.empty() && ancestor_wd < 0; ancestor = ancestor.parent_path()) {
      ancestor_wd = inotify_add_watch(inotify_fd_, ancestor.c_str(), kMask);
      if (ancestor_wd < 0 && errno != ENOENT && errno != ENOTDIR) {
        return Armed::Unwatchable;
      }
      if (ancestor == ancestor.parent_path()) {
        break;
      }
    }
    if (ancestor_wd < 0) {
      return Armed::Unwatchable;
    }
    // The directory may have come back between the failed watch and this one,
    // with no event left to say so: only wait when it is still missing.
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
      waiting[ancestor_wd].push_back(Waiting{.file = file, .reported = reported});
      return Armed::Waiting;
    }
  }
  return Armed::Unwatchable;
}

void OpenFileWatcher::RearmWaitingLocked(std::vector<std::filesystem::path>& changed) {
  WaitingByWatch before_waiting = std::exchange(waiting_by_watch_, {});
  const EntriesByWatch before_entries = entries_by_watch_;
  for (const auto& [wd, waiting] : before_waiting) {
    for (const Waiting& file : waiting) {
      // Back, and watched from now on: whatever it holds was written before the
      // watch existed, so the document is told to look.
      if (ArmLocked(file.file, file.reported, entries_by_watch_, waiting_by_watch_) ==
              Armed::Watching &&
          std::find(changed.begin(), changed.end(), file.reported) == changed.end()) {
        changed.push_back(file.reported);
      }
    }
  }
  DropUnusedWatchesLocked(before_entries, before_waiting);
}

void OpenFileWatcher::DropUnusedWatchesLocked(const EntriesByWatch& before_entries,
                                              const WaitingByWatch& before_waiting) {
  const auto unused = [this](int wd) {
    return entries_by_watch_.find(wd) == entries_by_watch_.end() &&
           waiting_by_watch_.find(wd) == waiting_by_watch_.end();
  };
  for (const auto& [wd, entries] : before_entries) {
    if (unused(wd)) {
      inotify_rm_watch(inotify_fd_, wd);
    }
  }
  for (const auto& [wd, waiting] : before_waiting) {
    if (unused(wd) && before_entries.find(wd) == before_entries.end()) {
      inotify_rm_watch(inotify_fd_, wd);
    }
  }
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
      bool rearm_waiting = false;
      while (true) {
        const ssize_t length = read(inotify_fd_, buffer.data(), buffer.size());
        if (length <= 0) {
          break;  // EAGAIN: drained
        }
        for (ssize_t offset = 0; offset < length;) {
          const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
          offset += static_cast<ssize_t>(sizeof(inotify_event) + event->len);
          if ((event->mask & IN_Q_OVERFLOW) != 0) {
            rearm_waiting = true;
            // Events were dropped: any file may have changed.
            for (const auto& [wd, entries] : entries_by_watch_) {
              for (const Entry& entry : entries) {
                note(entry.reported);
              }
            }
            continue;
          }
          // Something appeared under a directory a missing one is waited for
          // from (or that directory itself went, so the wait moves up): look
          // again once the batch is read.
          if (waiting_by_watch_.find(event->wd) != waiting_by_watch_.end() &&
              (event->mask & (IN_CREATE | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF |
                              IN_IGNORED)) != 0) {
            rearm_waiting = true;
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
      if (rearm_waiting) {
        RearmWaitingLocked(changed);
      }
    }
    if (!changed.empty() && callback_) {
      callback_(std::move(changed));
    }
  }
}

}  // namespace microide::platform
