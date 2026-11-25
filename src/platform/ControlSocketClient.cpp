#include "platform/ControlSocketClient.h"
#include "platform/UnixSocket.h"

#include "platform/ControlSocketLimits.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "util/PosixFd.h"
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace microide::platform {

namespace {
constexpr std::size_t kMaxResponseLineBytes = 16u << 20;
constexpr std::size_t kMaxRequestLineBytes = kMaxControlRequestLineBytes;
}

ControlSocketClient::~ControlSocketClient() { Close(); }

bool ControlSocketClient::Connect(const std::filesystem::path& socket_path) {
  Close();
  const int fd = ConnectUnixSocket(socket_path);
  if (fd < 0) {
    return false;
  }
  util::SetNonBlocking(fd);
  fd_ = fd;
  return true;
}

bool ControlSocketClient::SendLine(const std::string& line, std::chrono::milliseconds timeout) {
  if (fd_ < 0) {
    return false;
  }
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
      return false;
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
      return false;
    }
    const ssize_t written =
        ::send(fd_, framed.data() + offset, framed.size() - offset, MSG_NOSIGNAL);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue;
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
      return std::nullopt;
    }
    char chunk[4096];
    const ssize_t count = ::recv(fd_, chunk, sizeof(chunk), 0);
    if (count > 0) {
      read_buf_.append(chunk, static_cast<std::size_t>(count));
      if (read_buf_.size() > kMaxResponseLineBytes &&
          read_buf_.find('\n') == std::string::npos) {
        return std::nullopt;
      }
      continue;
    }
    if (count == 0) {
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

}
