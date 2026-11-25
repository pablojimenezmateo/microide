#pragma once

#include <algorithm>
#include <cstdio>
#include <exception>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "TestSupport.h"
#include "platform/Subprocess.h"

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

namespace microide::tests {

namespace server_fixture_detail {

inline constexpr const char* kOwnerFile = "owner.pid";

inline bool ProcessIsAlive(pid_t pid) {
  return pid > 0 && (::kill(pid, 0) == 0 || errno == EPERM);
}

inline std::string ReadProcFile(const std::filesystem::path& path) {
  std::string text;
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return text;
  }
  char buffer[4096];
  for (ssize_t count; (count = ::read(fd, buffer, sizeof(buffer))) > 0;) {
    text.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(fd);
  return text;
}

inline void KillProcessesUnder(const std::filesystem::path& fixture_root) {
  const std::string needle = fixture_root.string() + "/";
  std::error_code ec;
  for (std::filesystem::directory_iterator it("/proc", ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) {
      continue;
    }
    const pid_t pid = static_cast<pid_t>(std::atoi(name.c_str()));
    if (pid == ::getpid()) {
      continue;
    }
    if (ReadProcFile(it->path() / "cmdline").find(needle) != std::string::npos ||
        ReadProcFile(it->path() / "environ").find(needle) != std::string::npos) {
      ::kill(pid, SIGKILL);
    }
  }
}

inline void ReapOrphanedFixtures() {
  std::error_code ec;
  const std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
  if (ec) {
    return;
  }
  const auto now = std::filesystem::file_time_type::clock::now();
  std::error_code walk;
  for (std::filesystem::directory_iterator it(tmp, walk), end; !walk && it != end; it.increment(walk)) {
    const std::filesystem::directory_entry& entry = *it;
    const std::string name = entry.path().filename().string();
    if (name.rfind("mip.", 0) != 0 || name.size() != 10 || !entry.is_directory(ec)) {
      continue;
    }
    bool orphaned = false;
    std::ifstream owner(entry.path() / kOwnerFile);
    if (pid_t pid = 0; owner >> pid) {
      orphaned = !ProcessIsAlive(pid);
    } else {
      const auto written = std::filesystem::last_write_time(entry.path(), ec);
      orphaned = !ec && now - written > std::chrono::hours(1);
    }
    if (!orphaned) {
      continue;
    }
    KillProcessesUnder(entry.path());
    std::filesystem::remove_all(entry.path(), ec);
  }
}

}

class ShortServerDir {
 public:
  ShortServerDir() {
    static std::once_flag reaped;
    std::call_once(reaped, server_fixture_detail::ReapOrphanedFixtures);
    std::string pattern = (std::filesystem::temp_directory_path() / "mip.XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    Expect(::mkdtemp(buffer.data()) != nullptr, "mkdtemp");
    path_ = std::filesystem::path(buffer.data()) / "s";
    std::ofstream(path_.parent_path() / server_fixture_detail::kOwnerFile) << ::getpid();
  }
  ~ShortServerDir() {
    if (std::uncaught_exceptions() > uncaught_at_start_) {
      DumpServerLogTail();
    }
    platform::RunSubprocess({MICROIDE_SERVER_BINARY, "stop", "--socket-dir", path_.string()},
                            platform::SubprocessOptions{.timeout_ms = 10000});
    server_fixture_detail::KillProcessesUnder(path_.parent_path());
    std::error_code ec;
    std::filesystem::remove_all(path_.parent_path(), ec);
  }
  ShortServerDir(const ShortServerDir&) = delete;
  ShortServerDir& operator=(const ShortServerDir&) = delete;

  const std::filesystem::path& path() const { return path_; }
  std::filesystem::path scratch() const { return path_.parent_path(); }

 private:
  void DumpServerLogTail() const {
    constexpr std::streamoff kTail = 32 * 1024;
    std::ifstream log(path_ / "server.log", std::ios::binary | std::ios::ate);
    if (!log) {
      std::fprintf(stderr, "[server log %s: none]\n", (path_ / "server.log").c_str());
      return;
    }
    const std::streamoff size = log.tellg();
    log.seekg(std::max<std::streamoff>(0, size - kTail));
    const std::string tail{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
    std::fprintf(stderr, "[server log %s, last %lld of %lld bytes]\n%s\n[end of server log]\n",
                 (path_ / "server.log").c_str(), static_cast<long long>(tail.size()),
                 static_cast<long long>(size), tail.c_str());
  }

  std::filesystem::path path_;
  int uncaught_at_start_ = std::uncaught_exceptions();
};

inline void StartServer(const ShortServerDir& dir, std::vector<std::string> extra_args = {}) {
  std::vector<std::string> argv = {MICROIDE_SERVER_BINARY, "start", "--socket-dir",
                                   dir.path().string()};
  argv.insert(argv.end(), extra_args.begin(), extra_args.end());
  const auto started =
      platform::RunSubprocess(argv, platform::SubprocessOptions{.timeout_ms = 20000});
  Expect(started.success(), "the server starts: " + started.stderr_text);
}

struct FakeSshHost {
  ShortServerDir dir;
  std::filesystem::path home = dir.scratch() / "h";
  std::filesystem::path login_dir;
  std::string master_delay;
  std::filesystem::path shim = std::filesystem::path(MICROIDE_TEST_SOURCE_DIR) / "fixtures" /
                               "remote" / "fake-ssh";

  FakeSshHost() {
    std::filesystem::create_directories(home);
    std::filesystem::permissions(home, std::filesystem::perms::owner_all);
  }
  ~FakeSshHost() {
    platform::RunSubprocess({MICROIDE_SERVER_BINARY, "stop", "--socket-dir",
                             (home / ".local/state/microide/server").string()},
                            platform::SubprocessOptions{.timeout_ms = 10000});
  }
  FakeSshHost(const FakeSshHost&) = delete;
  FakeSshHost& operator=(const FakeSshHost&) = delete;

  std::filesystem::path server_dir() const { return home / ".local/state/microide/server"; }

  std::vector<std::string> SshArgv(bool needs_auth = false) const {
    std::vector<std::string> argv = {"env", "FAKE_SSH_HOME=" + home.string()};
    if (!login_dir.empty()) {
      argv.push_back("FAKE_SSH_CWD=" + login_dir.string());
    }
    if (needs_auth) {
      argv.push_back("FAKE_SSH_NEEDS_AUTH=1");
    }
    if (!master_delay.empty()) {
      argv.push_back("FAKE_SSH_MASTER_DELAY=" + master_delay);
    }
    argv.push_back(shim.string());
    return argv;
  }
};

}

