#include "platform/UnixSocket.h"

#include <cerrno>
#include <cstring>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace microide::platform {

#if defined(__unix__) || defined(__APPLE__)

namespace {

bool FillAddress(const std::string& path, sockaddr_un* address) {
  if (path.empty() || path.size() > kMaxUnixSocketPathBytes ||
      path.size() + 1 > sizeof(address->sun_path)) {
    return false;
  }
  *address = sockaddr_un{};
  address->sun_family = AF_UNIX;
  std::memcpy(address->sun_path, path.c_str(), path.size() + 1);
  return true;
}

}  // namespace

bool ClearStaleSocketPath(const std::filesystem::path& path) {
  struct stat st{};
  if (::lstat(path.c_str(), &st) != 0) {
    return errno == ENOENT;  // nothing there → clear; other errors → refuse
  }
  if (!S_ISSOCK(st.st_mode) || st.st_uid != ::geteuid()) {
    return false;  // not our socket — do not delete it
  }
  return ::unlink(path.c_str()) == 0 || errno == ENOENT;
}

int ConnectUnixSocket(const std::filesystem::path& path) {
  sockaddr_un address{};
  if (!FillAddress(path.string(), &address)) {
    return -1;
  }
  // Close-on-exec atomically at creation: every process the editor or the server
  // spawns would otherwise inherit a live connected handle.
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

int ListenUnixSocket(const std::filesystem::path& path, std::string* error) {
  const std::string path_string = path.string();
  sockaddr_un address{};
  if (!FillAddress(path_string, &address)) {
    *error = "socket path is " + std::to_string(path_string.size()) + " bytes, over the " +
             std::to_string(kMaxUnixSocketPathBytes) + "-byte AF_UNIX limit: " + path_string;
    return -1;
  }
  if (!ClearStaleSocketPath(path)) {
    *error = "something other than a socket of ours is at " + path_string;
    return -1;
  }
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    *error = std::string("socket: ") + std::strerror(errno);
    return -1;
  }
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    *error = "could not bind " + path_string + ": " + std::strerror(errno);
    ::close(fd);
    return -1;
  }
  ::chmod(path_string.c_str(), S_IRUSR | S_IWUSR);
  if (::listen(fd, 16) != 0) {
    *error = "could not listen on " + path_string + ": " + std::strerror(errno);
    ::close(fd);
    ::unlink(path_string.c_str());
    return -1;
  }
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  return fd;
}

#else

bool ClearStaleSocketPath(const std::filesystem::path&) { return false; }
int ConnectUnixSocket(const std::filesystem::path&) { return -1; }
int ListenUnixSocket(const std::filesystem::path&, std::string* error) {
  *error = "unix sockets are not available on this platform";
  return -1;
}

#endif

}  // namespace microide::platform
