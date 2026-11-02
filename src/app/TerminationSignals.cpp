#include "app/TerminationSignals.h"

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>

#include <fcntl.h>
#include <unistd.h>

namespace microide::app {
namespace {

constexpr std::array<int, 3> kSignals = {SIGTERM, SIGINT, SIGHUP};

// The self-pipe, read by the watcher: [0] read end, [1] write end. Written only by
// the handler (write(2) is async-signal-safe) and by the destructor's wake.
std::atomic<int> g_write_fd{-1};

void OnTerminationSignal(int signo) {
  const int saved_errno = errno;  // a handler must not change the interrupted code's errno
  const int fd = g_write_fd.load(std::memory_order_relaxed);
  if (fd >= 0) {
    const unsigned char byte = static_cast<unsigned char>(signo);
    (void)!::write(fd, &byte, 1);  // a full pipe drops it: one is already pending
  }
  errno = saved_errno;
}

}  // namespace

TerminationSignalWatcher::TerminationSignalWatcher(std::uint32_t event_type) {
  int fds[2] = {-1, -1};
  if (event_type == 0 || ::pipe2(fds, O_CLOEXEC) != 0) {
    return;
  }
  // The write end non-blocking: a handler must never block, even on a full pipe.
  (void)::fcntl(fds[1], F_SETFL, O_NONBLOCK);
  g_write_fd.store(fds[1], std::memory_order_relaxed);
  const int read_fd = fds[0];
  const int write_fd = fds[1];
  thread_ = std::thread([read_fd, write_fd, event_type] {
    for (;;) {
      unsigned char byte = 0;
      const ssize_t got = ::read(read_fd, &byte, 1);
      if (got < 0 && errno == EINTR) {
        continue;
      }
      if (got <= 0 || byte == 0) {
        break;  // closed, or the destructor's wake
      }
      SDL_Event event{};
      event.type = event_type;
      event.user.code = byte;
      (void)SDL_PushEvent(&event);
    }
    ::close(read_fd);
    ::close(write_fd);
  });
  struct sigaction action {};
  action.sa_handler = OnTerminationSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESTART;
  for (const int signo : kSignals) {
    (void)::sigaction(signo, &action, nullptr);
  }
  installed_ = true;
}

TerminationSignalWatcher::~TerminationSignalWatcher() {
  if (!installed_) {
    return;
  }
  struct sigaction action {};
  action.sa_handler = SIG_DFL;
  sigemptyset(&action.sa_mask);
  for (const int signo : kSignals) {
    (void)::sigaction(signo, &action, nullptr);
  }
  const int fd = g_write_fd.exchange(-1, std::memory_order_relaxed);
  const unsigned char wake = 0;
  (void)!::write(fd, &wake, 1);
  if (thread_.joinable()) {
    thread_.join();
  }
}

}  // namespace microide::app
