#include "platform/ControlSocketClient.h"

#include <cerrno>
#include <cstring>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#endif

namespace microide::platform {

#if defined(__unix__) || defined(__APPLE__)

namespace {
// A control response/event line is small (query results, debug state). Bound the
// read buffer so a peer that streams bytes without ever sending a newline can't
// grow it without limit — over a local Unix socket that is GB/s, so an unbounded
// buffer would OOM the `control-send` process long before its --timeout elapses.
// Mirrors the server's kMaxRequestLineBytes intent for the client direction.
constexpr std::size_t kMaxResponseLineBytes = 16u << 20;  // 16 MiB
// Symmetric cap for the outbound direction: a single control request line is a small
// command/query; refuse to frame anything larger rather than pump megabytes into a
// possibly-stalled peer. Mirrors the server's request-line ceiling.
constexpr std::size_t kMaxRequestLineBytes = 16u << 20;  // 16 MiB
}  // namespace

ControlSocketClient::~ControlSocketClient() { Close(); }

bool ControlSocketClient::Connect(const std::filesystem::path& socket_path) {
  Close();
  const std::string path_string = socket_path.string();
  if (path_string.empty() || path_string.size() + 1 > sizeof(sockaddr_un::sun_path)) {
    return false;
  }
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path_string.c_str(), path_string.size() + 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return false;
  }
  // Make subsequent send()/recv() non-blocking so poll-driven deadlines are honored
  // even when the peer's receive buffer is full: a blocking send() would otherwise
  // stall inside the kernel past the caller's timeout. connect() above stays blocking.
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  fd_ = fd;
  return true;
}

bool ControlSocketClient::SendLine(const std::string& line, std::chrono::milliseconds timeout) {
  if (fd_ < 0) {
    return false;
  }
  // Cap the outbound size before framing so a runaway caller cannot try to stream an
  // unbounded line (the +1 accounts for the appended newline terminator).
  if (line.size() + 1 > kMaxRequestLineBytes) {
    return false;
  }
  std::string framed = line;
  framed.push_back('\n');
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::size_t offset = 0;
  while (offset < framed.size()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return false;  // overall write deadline exceeded (peer not draining)
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    pollfd p{fd_, POLLOUT, 0};
    const int ready = ::poll(&p, 1, static_cast<int>(remaining));
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (ready == 0) {
      return false;  // timed out waiting for the socket to become writable
    }
    const ssize_t written =
        ::send(fd_, framed.data() + offset, framed.size() - offset, MSG_NOSIGNAL);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue;  // transient: re-poll against the deadline
    }
    return false;
  }
  return true;
}

void ControlSocketClient::ShutdownWrite() {
  if (fd_ >= 0) {
    ::shutdown(fd_, SHUT_WR);
  }
}

std::optional<std::string> ControlSocketClient::ReadLine(std::chrono::milliseconds timeout) {
  if (fd_ < 0) {
    return std::nullopt;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    const std::size_t newline = read_buf_.find('\n');
    if (newline != std::string::npos) {
      std::string line = read_buf_.substr(0, newline);
      read_buf_.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      return line;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return std::nullopt;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    pollfd p{fd_, POLLIN, 0};
    const int ready = ::poll(&p, 1, static_cast<int>(remaining));
    if (ready <= 0) {
      if (ready < 0 && errno == EINTR) {
        continue;
      }
      return std::nullopt;  // timeout or poll error
    }
    char chunk[4096];
    const ssize_t count = ::recv(fd_, chunk, sizeof(chunk), 0);
    if (count > 0) {
      read_buf_.append(chunk, static_cast<std::size_t>(count));
      // No newline within the cap: a runaway / malicious peer. Abandon rather
      // than let the buffer grow toward OOM.
      if (read_buf_.size() > kMaxResponseLineBytes &&
          read_buf_.find('\n') == std::string::npos) {
        return std::nullopt;
      }
      continue;
    }
    if (count == 0) {
      // EOF: surface a final un-terminated line if one is buffered, else done.
      if (!read_buf_.empty()) {
        std::string line = std::move(read_buf_);
        read_buf_.clear();
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        return line;
      }
      return std::nullopt;
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
      continue;
    }
    return std::nullopt;
  }
}

void ControlSocketClient::Close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  read_buf_.clear();
}

#else  // non-POSIX: control channel unsupported.

ControlSocketClient::~ControlSocketClient() = default;
bool ControlSocketClient::Connect(const std::filesystem::path&) { return false; }
bool ControlSocketClient::SendLine(const std::string&, std::chrono::milliseconds) { return false; }
void ControlSocketClient::ShutdownWrite() {}
std::optional<std::string> ControlSocketClient::ReadLine(std::chrono::milliseconds) {
  return std::nullopt;
}
void ControlSocketClient::Close() {}

#endif

}  // namespace microide::platform
