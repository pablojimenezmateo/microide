#pragma once

#if defined(__unix__) || defined(__APPLE__)

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

inline void CloseIfValid(int fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}

}  // namespace microide::util

#endif  // __unix__ || __APPLE__
