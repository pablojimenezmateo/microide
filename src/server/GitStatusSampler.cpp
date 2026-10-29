#include "server/GitStatusSampler.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <map>
#include <string_view>
#include <vector>

#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "util/PosixFd.h"
#include "util/ThreadName.h"

namespace microide::server {
namespace {

// A file the fingerprint compares changes by being written in place, created, or
// renamed over (git's lockfile protocol: `HEAD.lock` -> `HEAD`); its directory
// going away ends the watch (IN_IGNORED follows).
constexpr std::uint32_t kWatchMask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                                     IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;
// A commit writes the index, COMMIT_EDITMSG and the ref within milliseconds: one
// sample once the burst goes quiet, not one per file.
constexpr int kSettleMs = 20;
constexpr int kMaxSettleMs = 250;

// The watches the thread holds: wd -> directory, and which one is the project root
// (whose entries matter only when the entry is `.git`).
struct Watches {
  int inotify = -1;
  std::map<int, std::filesystem::path> by_wd;
  int root_wd = -1;

  void Update(const std::vector<std::filesystem::path>& wanted) {
    if (inotify < 0) {
      return;
    }
    for (auto it = by_wd.begin(); it != by_wd.end();) {
      if (std::find(wanted.begin(), wanted.end(), it->second) == wanted.end()) {
        (void)::inotify_rm_watch(inotify, it->first);
        it = by_wd.erase(it);
      } else {
        ++it;
      }
    }
    root_wd = -1;
    for (std::size_t i = 0; i < wanted.size(); ++i) {
      // The same wd again for a directory already watched. One that does not exist
      // (a ref directory before its first branch) is left to the next sample.
      const int wd = ::inotify_add_watch(inotify, wanted[i].c_str(), kWatchMask);
      if (wd < 0) {
        continue;
      }
      by_wd[wd] = wanted[i];
      if (i == 0) {
        root_wd = wd;  // WatchedDirectories lists the root first
      }
    }
  }

  // Reads every queued event. True when one of them can have changed what a sample
  // compares.
  bool Drain() {
    bool relevant = false;
    alignas(inotify_event) char buffer[4096];
    for (;;) {
      const ssize_t length = ::read(inotify, buffer, sizeof(buffer));
      if (length <= 0) {
        return relevant;  // EAGAIN: drained
      }
      for (ssize_t at = 0; at < length;) {
        const auto* event = reinterpret_cast<const inotify_event*>(buffer + at);
        at += static_cast<ssize_t>(sizeof(inotify_event) + event->len);
        if ((event->mask & (IN_Q_OVERFLOW | IN_IGNORED)) != 0) {
          if ((event->mask & IN_IGNORED) != 0) {
            by_wd.erase(event->wd);
          }
          relevant = true;
          continue;
        }
        if (event->wd != root_wd) {
          relevant = true;
          continue;
        }
        // In the root, only `.git` itself appearing, going or being replaced.
        const std::string_view name =
            event->len > 0 ? std::string_view(event->name) : std::string_view();
        relevant = relevant || name == ".git";
      }
    }
  }
};

// Waits on the stop pipe and, when it is open, the inotify descriptor. Returns the
// poll result; `stop_ready`/`events_ready` say which fired.
int WaitFor(int stop_fd, int inotify, int timeout_ms, bool* stop_ready, bool* events_ready) {
  pollfd fds[2] = {{.fd = stop_fd, .events = POLLIN, .revents = 0},
                   {.fd = inotify, .events = POLLIN, .revents = 0}};
  const int ready = ::poll(fds, inotify >= 0 ? 2 : 1, timeout_ms);
  *stop_ready = ready > 0 && fds[0].revents != 0;
  *events_ready = ready > 0 && inotify >= 0 && fds[1].revents != 0;
  return ready;
}

}  // namespace

GitStatusSampler::GitStatusSampler(std::filesystem::path root,
                                   std::chrono::milliseconds safety_interval,
                                   std::function<void()> changed)
    : changed_(std::move(changed)) {
  tracker_.SetProjectRoot(root);  // reads the baseline
  stop_.Open();
  thread_ = std::thread([this, safety_interval]() {
    util::NameCurrentThread("srv-git");
    Run(safety_interval);
  });
}

GitStatusSampler::~GitStatusSampler() {
  stopping_.store(true);
  stop_.Wake();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void GitStatusSampler::Sample() {
  if (!tracker_.SampleChanges().empty() && changed_) {
    changed_();
  }
}

void GitStatusSampler::Run(std::chrono::milliseconds safety_interval) {
  const util::UniqueFd inotify(::inotify_init1(IN_CLOEXEC | IN_NONBLOCK));
  Watches watches{.inotify = inotify.Get()};
  watches.Update(tracker_.WatchedDirectories());
  const int safety_ms = static_cast<int>(std::max<std::int64_t>(1, safety_interval.count()));
  while (!stopping_.load()) {
    bool stop_ready = false;
    bool events_ready = false;
    const int ready = WaitFor(stop_.read_fd(), watches.inotify, safety_ms, &stop_ready, &events_ready);
    if (stopping_.load()) {
      return;
    }
    if (stop_ready) {
      stop_.Drain();
    }
    if (ready < 0 && errno == EINTR) {
      continue;
    }
    // The safety interval (or a poll error, which must not spin) samples anyway.
    bool relevant = ready <= 0;
    if (events_ready) {
      relevant = watches.Drain() || relevant;
      // Let the burst finish, then sample once.
      for (int waited = 0; relevant && waited < kMaxSettleMs; waited += kSettleMs) {
        if (WaitFor(stop_.read_fd(), watches.inotify, kSettleMs, &stop_ready, &events_ready) <= 0 ||
            stop_ready) {
          break;
        }
        (void)watches.Drain();
      }
    }
    if (!relevant || stopping_.load()) {
      continue;
    }
    Sample();
    // A checkout moves the branch ref's directory; `.git` may have appeared.
    watches.Update(tracker_.WatchedDirectories());
  }
}

}  // namespace microide::server
