#pragma once

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "util/PosixPipe.h"

#include <mutex>

namespace microide::util {

class WakePipe {
 public:
  WakePipe() = default;
  ~WakePipe() { Close(); }
  WakePipe(const WakePipe&) = delete;
  WakePipe& operator=(const WakePipe&) = delete;

  void Open() {
    std::lock_guard lock(mutex_);
    if (read_fd_ >= 0) {
      return;
    }
    int fds[2] = {-1, -1};
    if (!MakeCloexecPipe(fds, true)) {
      return;
    }
    read_fd_ = fds[0];
    write_fd_ = fds[1];
  }

  void Close() {
    std::lock_guard lock(mutex_);
    if (write_fd_ >= 0) {
      ::close(write_fd_);
      write_fd_ = -1;
    }
    if (read_fd_ >= 0) {
      ::close(read_fd_);
      read_fd_ = -1;
    }
  }

  void Wake() {
    std::lock_guard lock(mutex_);
    if (write_fd_ < 0) {
      return;
    }
    const char byte = 1;
    ssize_t written = ::write(write_fd_, &byte, 1);
    (void)written;
  }

  void Drain() {
    if (read_fd_ < 0) {
      return;
    }
    char scratch[64];
    while (::read(read_fd_, scratch, sizeof(scratch)) > 0) {
    }
  }

  int read_fd() const { return read_fd_; }

  bool PollReadableOrWake(int watched_fd, int timeout_ms) {
    if (watched_fd < 0) {
      return true;
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
  }

 private:
  std::mutex mutex_;
  int read_fd_ = -1;
  int write_fd_ = -1;
};

}
