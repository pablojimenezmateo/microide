#pragma once

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "util/PosixPipe.h"
#endif

#include <mutex>

namespace microide::util {

// A self-pipe used to break a blocking poll() from another thread: the I/O
// thread adds read_fd() to its poll set, and any other thread calls Wake() to
// make that poll return immediately (without closing the fd the I/O thread is
// polling, which would be a data race and risk fd-number reuse).
//
// Open/Close/Wake take a brief internal mutex so they never race each other.
// Drain() intentionally takes no lock: it runs only on the I/O thread after
// poll() reports the read end readable, and read_fd_ is stable across that
// thread's lifetime (opened before it starts, closed after it joins). POSIX-only;
// every method is a no-op on other platforms.
class WakePipe {
 public:
  WakePipe() = default;
  ~WakePipe() { Close(); }
  WakePipe(const WakePipe&) = delete;
  WakePipe& operator=(const WakePipe&) = delete;

  void Open() {
#if defined(__unix__) || defined(__APPLE__)
    std::lock_guard lock(mutex_);
    if (read_fd_ >= 0) {
      return;
    }
    int fds[2] = {-1, -1};
    // Non-blocking so a wake never parks the producer and Drain() can empty the
    // pipe without blocking; close-on-exec so a concurrent fork+exec cannot pin
    // these ends open. MakeCloexecPipe sets both atomically where the platform
    // allows — the previous pipe()+F_SETFD here left a window in which a racing
    // fork inherited the self-pipe.
    if (!MakeCloexecPipe(fds, /*nonblocking=*/true)) {
      return;
    }
    read_fd_ = fds[0];
    write_fd_ = fds[1];
#endif
  }

  void Close() {
#if defined(__unix__) || defined(__APPLE__)
    std::lock_guard lock(mutex_);
    if (write_fd_ >= 0) {
      ::close(write_fd_);
      write_fd_ = -1;
    }
    if (read_fd_ >= 0) {
      ::close(read_fd_);
      read_fd_ = -1;
    }
#endif
  }

  // Break the I/O thread's poll(). Safe to call from any thread; a full pipe
  // already means a wake is pending, so a failed write is ignored.
  void Wake() {
#if defined(__unix__) || defined(__APPLE__)
    std::lock_guard lock(mutex_);
    if (write_fd_ < 0) {
      return;
    }
    const char byte = 1;
    ssize_t written = ::write(write_fd_, &byte, 1);
    (void)written;
#endif
  }

  // Consume pending wake bytes. Call only on the I/O thread (see class note).
  void Drain() {
#if defined(__unix__) || defined(__APPLE__)
    if (read_fd_ < 0) {
      return;
    }
    char scratch[64];
    while (::read(read_fd_, scratch, sizeof(scratch)) > 0) {
    }
#endif
  }

  // Read end to add to a poll() set; -1 when closed.
  int read_fd() const { return read_fd_; }

  // Wait until `watched_fd` has data/EOF or a Wake() arrives, whichever comes
  // first. Returns true when `watched_fd` should be read; false on timeout or
  // EINTR (the caller loops). A pending wake is consumed before returning.
  //
  // This is the stdio-transport poll shared by BOTH the LSP and DAP clients.
  // They each carried a verbatim copy, which is how they drift: the copies had
  // already diverged once and been re-synced by hand ("Mirrors the DAP fix").
  //
  // The caller MUST pass a freshly-fetched descriptor rather than a cached one:
  // a liveness probe or the shutdown reap can close the watched fd from another
  // thread and its NUMBER be reused, so polling a stale copy would watch an
  // unrelated descriptor. A closed fd arrives here as -1, and returning true
  // then lets the caller's read observe EOF and tear down.
  //
  // Call only on the I/O thread — it drains the wake pipe (see Drain()).
  bool PollReadableOrWake(int watched_fd, int timeout_ms) {
#if defined(__unix__) || defined(__APPLE__)
    if (watched_fd < 0) {
      return true;  // nothing pollable; let the caller's read report EOF
    }
    pollfd fds[2] = {};
    int nfds = 0;
    const int watched_index = nfds;
    fds[nfds].fd = watched_fd;
    fds[nfds].events = POLLIN | POLLHUP;
    ++nfds;
    int wake_index = -1;
    if (const int wake_read_fd = read_fd(); wake_read_fd >= 0) {
      wake_index = nfds;
      fds[nfds].fd = wake_read_fd;
      fds[nfds].events = POLLIN;
      ++nfds;
    }
    const int ready = ::poll(fds, nfds, timeout_ms);
    if (ready <= 0) {
      return false;
    }
    if (wake_index >= 0 && (fds[wake_index].revents & POLLIN) != 0) {
      Drain();
    }
    const short revents = fds[watched_index].revents;
    return (revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0;
#else
    (void)watched_fd;
    (void)timeout_ms;
    return true;
#endif
  }

 private:
  std::mutex mutex_;
  int read_fd_ = -1;
  int write_fd_ = -1;
};

}  // namespace microide::util
