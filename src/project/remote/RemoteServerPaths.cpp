#include "project/remote/RemoteServerPaths.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <vector>

#include "platform/UnixSocket.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#endif

namespace microide::project::remote {
namespace {

std::string_view TrimView(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

std::string OctalMode(unsigned mode) {
  char buffer[8];
  std::snprintf(buffer, sizeof(buffer), "%04o", mode & 07777u);
  return buffer;
}

}  // namespace

std::filesystem::path DefaultServerSocketDir(const std::filesystem::path& home) {
  std::error_code ec;
  std::filesystem::path canonical = std::filesystem::canonical(home, ec);
  if (ec) {
    canonical = home;
  }
  return canonical / ".local" / "state" / "microide" / "server";
}

std::filesystem::path ServerSocketPath(const std::filesystem::path& dir) {
  return dir / "server.sock";
}

std::filesystem::path ServerLogPath(const std::filesystem::path& dir) {
  return dir / "server.log";
}

bool EnsureServerSocketDir(const std::filesystem::path& dir, std::string* error) {
#if defined(__unix__) || defined(__APPLE__)
  if (!dir.is_absolute()) {
    *error = "the server socket directory must be an absolute path: " + dir.string();
    return false;
  }
  if (ServerSocketPath(dir).string().size() > platform::kMaxUnixSocketPathBytes) {
    *error = "the server socket path would be over the " +
             std::to_string(platform::kMaxUnixSocketPathBytes) +
             "-byte AF_UNIX limit: " + ServerSocketPath(dir).string();
    return false;
  }
  const uid_t uid = ::geteuid();
  // Walk down from "/", one component at a time, each opened relative to its
  // parent's descriptor: a name checked and then used by path could be swapped
  // for a symlink in between, a descriptor cannot.
  int parent = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (parent < 0) {
    *error = "cannot open /";
    return false;
  }
  std::filesystem::path walked = "/";
  std::vector<std::string> components;
  for (const auto& part : dir.relative_path()) {
    if (!part.empty() && part != ".") {
      components.push_back(part.string());
    }
  }
  bool ok = true;
  for (std::size_t i = 0; i < components.size() && ok; ++i) {
    const std::string& name = components[i];
    const bool last = i + 1 == components.size();
    walked /= name;
    if (name == "..") {
      *error = "the server socket directory may not contain '..': " + dir.string();
      ok = false;
      break;
    }
    struct stat st{};
    if (::fstatat(parent, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno != ENOENT || ::mkdirat(parent, name.c_str(), 0700) != 0 ||
          ::fstatat(parent, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        *error = "cannot create " + walked.string() + ": " + std::strerror(errno);
        ok = false;
        break;
      }
    }
    if (S_ISLNK(st.st_mode)) {
      *error = walked.string() + " is a symlink; the server socket directory must not "
               "go through one";
      ok = false;
      break;
    }
    if (!S_ISDIR(st.st_mode)) {
      *error = walked.string() + " is not a directory";
      ok = false;
      break;
    }
    if (last) {
      if (st.st_uid != uid || (st.st_mode & 07777u) != 0700u) {
        *error = walked.string() + " has mode " + OctalMode(st.st_mode) + " owned by uid " +
                 std::to_string(st.st_uid) + "; expected mode 0700 owned by uid " +
                 std::to_string(uid);
        ok = false;
        break;
      }
    } else {
      const bool sticky = (st.st_mode & S_ISVTX) != 0;
      const bool foreign_writable = (st.st_mode & (S_IWGRP | S_IWOTH)) != 0;
      if ((st.st_uid != uid && st.st_uid != 0) || (foreign_writable && !sticky)) {
        *error = walked.string() + " has mode " + OctalMode(st.st_mode) + " owned by uid " +
                 std::to_string(st.st_uid) +
                 "; a parent of the socket directory must be owned by you or root and not "
                 "writable by others";
        ok = false;
        break;
      }
    }
    const int next = ::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    ::close(parent);
    parent = next;
    if (parent < 0) {
      *error = "cannot open " + walked.string() + ": " + std::strerror(errno);
      return false;
    }
  }
  if (parent >= 0) {
    ::close(parent);
  }
  return ok;
#else
  (void)dir;
  *error = "the remote server is not available on this platform";
  return false;
#endif
}

std::string MakeDaemonEpoch() {
  unsigned char bytes[16] = {};
  bool filled = false;
#if defined(__linux__)
  filled = ::getrandom(bytes, sizeof(bytes), 0) == static_cast<ssize_t>(sizeof(bytes));
#endif
#if defined(__unix__) || defined(__APPLE__)
  if (!filled) {
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
      filled = ::read(fd, bytes, sizeof(bytes)) == static_cast<ssize_t>(sizeof(bytes));
      ::close(fd);
    }
  }
#endif
  if (!filled) {
    // Unique enough to tell two server processes apart, which is all it is for.
    const auto stamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    for (std::size_t i = 0; i < 8; ++i) {
      bytes[i] = static_cast<unsigned char>(stamp >> (8 * i));
    }
#if defined(__unix__) || defined(__APPLE__)
    const auto pid = static_cast<std::uint64_t>(::getpid());
    for (std::size_t i = 0; i < 8; ++i) {
      bytes[8 + i] = static_cast<unsigned char>(pid >> (8 * i));
    }
#endif
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string epoch;
  for (const unsigned char byte : bytes) {
    epoch.push_back(kHex[byte >> 4]);
    epoch.push_back(kHex[byte & 0xf]);
  }
  return epoch;
}

SessionSurvival ReadSessionSurvival(const std::filesystem::path& root, std::string_view user) {
  SessionSurvival survival;
  const auto apply = [&survival](const std::filesystem::path& file) {
    const std::optional<std::string> text = util::ReadTextFile(file);
    if (!text.has_value()) {
      return;
    }
    bool in_login = false;
    for (std::string_view line : util::SplitLineViews(*text)) {
      line = TrimView(line);
      if (line.empty() || line.front() == '#' || line.front() == ';') {
        continue;
      }
      if (line.front() == '[') {
        in_login = line == "[Login]";
        continue;
      }
      if (!in_login || !line.starts_with("KillUserProcesses")) {
        continue;
      }
      const std::size_t eq = line.find('=');
      if (eq == std::string_view::npos ||
          TrimView(line.substr(0, eq)) != "KillUserProcesses") {
        continue;
      }
      const std::string_view value = TrimView(line.substr(eq + 1));
      survival.kill_user_processes = value == "yes" || value == "true" || value == "1" ||
                                     value == "on";
    }
  };
  const std::filesystem::path etc = root / "etc" / "systemd";
  apply(etc / "logind.conf");
  // Drop-ins override the main file, in lexical order (systemd's rule); /run and
  // /usr/lib drop-ins are the distribution's and come first.
  std::vector<std::filesystem::path> drop_ins;
  for (const std::filesystem::path& dir :
       {root / "usr" / "lib" / "systemd" / "logind.conf.d", root / "run" / "systemd" / "logind.conf.d",
        etc / "logind.conf.d"}) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      if (entry.path().extension() == ".conf") {
        drop_ins.push_back(entry.path());
      }
    }
  }
  std::stable_sort(drop_ins.begin(), drop_ins.end(),
                   [](const auto& a, const auto& b) { return a.filename() < b.filename(); });
  for (const auto& file : drop_ins) {
    apply(file);
  }
  if (!user.empty() && user.find('/') == std::string_view::npos) {
    std::error_code ec;
    survival.linger = std::filesystem::exists(
        root / "var" / "lib" / "systemd" / "linger" / std::string(user), ec);
  }
  return survival;
}

}  // namespace microide::project::remote
