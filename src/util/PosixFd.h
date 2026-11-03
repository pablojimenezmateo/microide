#pragma once

#include <cerrno>

#include <fcntl.h>
#include <unistd.h>

namespace microide::util {

// Owns one POSIX file descriptor and closes it on destruction. Move-only.
// Three TUs had grown their own copy of this (two named `Fd`, one `UniqueFd`),
// each with a slightly different surface.
class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) : fd_(fd) {}

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& other) noexcept : fd_(other.Release()) {}

  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      Reset(other.Release());
    }
    return *this;
  }

  ~UniqueFd() { Reset(); }

  int Get() const { return fd_; }
  bool IsValid() const { return fd_ >= 0; }

  int Release() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

  void Reset(int fd = -1) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};

// Adds O_NONBLOCK to `fd`'s status flags, keeping the others. Returns false when
// `fd` is invalid or fcntl fails.
inline bool SetNonBlocking(int fd) {
  if (fd < 0) {
    return false;
  }
  const int flags = ::fcntl(fd, F_GETFL, 0);
  return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// Opens /dev/null onto whichever of descriptors 0, 1 and 2 is closed. Call it first
// thing in main, before any descriptor is created: a process started with one of
// them closed (`microide <&-`, some launchers and service managers) otherwise
// hands that number to its first pipe or socket. A child's `dup2(pipe_end, 0)`
// is then a no-op that leaves the pipe close-on-exec, the following close()
// drops it, and the language server, adapter or tool starts with no stdin.
inline void EnsureStandardDescriptorsOpen() {
  int null_fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
  if (null_fd < 0) {
    return;
  }
  if (null_fd <= 2) {
    // It took a standard slot itself: move it above them, so each dup2 below
    // copies it (and clears close-on-exec) rather than being a no-op on itself.
    const int high = ::fcntl(null_fd, F_DUPFD_CLOEXEC, 3);
    ::close(null_fd);
    if (high < 0) {
      return;
    }
    null_fd = high;
  }
  for (int fd = 0; fd <= 2; ++fd) {
    if (::fcntl(fd, F_GETFD) == -1 && errno == EBADF) {
      (void)::dup2(null_fd, fd);
    }
  }
  ::close(null_fd);
}

inline void CloseIfValid(int fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}

}  // namespace microide::util

