#pragma once

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
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
    if (::pipe(fds) != 0) {
      return;
    }
    ::fcntl(fds[0], F_SETFL, O_NONBLOCK);
    ::fcntl(fds[1], F_SETFL, O_NONBLOCK);
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

 private:
  std::mutex mutex_;
  int read_fd_ = -1;
  int write_fd_ = -1;
};

}  // namespace microide::util
