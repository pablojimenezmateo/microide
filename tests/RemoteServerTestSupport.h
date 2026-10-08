#pragma once

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

#if defined(__unix__) || defined(__APPLE__)
#include <sys/types.h>
#include <unistd.h>

namespace microide::tests {

namespace server_fixture_detail {

inline constexpr const char* kOwnerFile = "owner.pid";

inline bool ProcessIsAlive(pid_t pid) {
  return pid > 0 && (::kill(pid, 0) == 0 || errno == EPERM);
}

// SIGKILL every process whose command line names something under `fixture_root`
// — the server started there (a plain socket dir or a fake host's home), and
// anything still holding one of its paths. Linux reads /proc; elsewhere the
// `stop` the caller already sent is all there is.
inline void KillProcessesUnder(const std::filesystem::path& fixture_root) {
#if defined(__linux__)
  const std::string needle = fixture_root.string() + "/";
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
    const std::string name = entry.path().filename().string();
    if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) {
      continue;
    }
    std::ifstream in(entry.path() / "cmdline", std::ios::binary);
    const std::string cmdline((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (cmdline.find(needle) != std::string::npos) {
      ::kill(static_cast<pid_t>(std::atoi(name.c_str())), SIGKILL);
    }
  }
#else
  (void)fixture_root;
#endif
}

// A fixture whose test process was KILLED (a ctest timeout, a sanitizer abort, a
// Ctrl-C) never ran its destructor, and its server owns a live host process or
// terminal — by design not idle — so it lived forever (TD-2026-10-08-332). Each
// fixture records its test's pid; the first fixture in every test process
// reaps the directories whose owner is gone, so a crashed run is cleaned by the
// next one. A directory with no owner file predates the record and is reaped
// once it is an hour old: no test runs that long.
inline void ReapOrphanedFixtures() {
  std::error_code ec;
  const std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
  if (ec) {
    return;
  }
  const auto now = std::filesystem::file_time_type::clock::now();
  for (const auto& entry : std::filesystem::directory_iterator(tmp, ec)) {
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

}  // namespace server_fixture_detail

// A socket directory for a real microide-server daemon. Short on purpose: the
// AF_UNIX path limit makes the usual long temp paths unusable for a socket. The
// destructor stops the daemon and removes the tree, so a failed test leaks
// neither.
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
    platform::RunSubprocess({MICROIDE_SERVER_BINARY, "stop", "--socket-dir", path_.string()},
                            platform::SubprocessOptions{.timeout_ms = 10000});
    // `stop` asks; a server wedged on a live child may not answer in time.
    server_fixture_detail::KillProcessesUnder(path_.parent_path());
    std::error_code ec;
    std::filesystem::remove_all(path_.parent_path(), ec);
  }
  ShortServerDir(const ShortServerDir&) = delete;
  ShortServerDir& operator=(const ShortServerDir&) = delete;

  // The socket directory (not yet created; the server creates it 0700).
  const std::filesystem::path& path() const { return path_; }
  // Scratch space beside it, removed with it.
  std::filesystem::path scratch() const { return path_.parent_path(); }

 private:
  std::filesystem::path path_;
};

inline void StartServer(const ShortServerDir& dir) {
  const auto started = platform::RunSubprocess(
      {MICROIDE_SERVER_BINARY, "start", "--socket-dir", dir.path().string()},
      platform::SubprocessOptions{.timeout_ms = 20000});
  Expect(started.success(), "the server starts: " + started.stderr_text);
}

// A "host" for the connection-lifecycle tests: tests/fixtures/remote/fake-ssh,
// which runs the remote command on this machine with HOME pointed at a private
// directory, plus the server that ends up running there — stopped when the test
// ends.
struct FakeSshHost {
  ShortServerDir dir;
  std::filesystem::path home = dir.scratch() / "h";
  std::filesystem::path shim = std::filesystem::path(MICROIDE_TEST_SOURCE_DIR) / "fixtures" /
                               "remote" / "fake-ssh";

  FakeSshHost() {
    std::filesystem::create_directories(home);
    // The server refuses a socket directory under anything group-writable; a
    // real home is not, whatever this machine's umask made the fixture.
    std::filesystem::permissions(home, std::filesystem::perms::owner_all);
  }
  ~FakeSshHost() {
    platform::RunSubprocess({MICROIDE_SERVER_BINARY, "stop", "--socket-dir",
                             (home / ".local/state/microide/server").string()},
                            platform::SubprocessOptions{.timeout_ms = 10000});
  }
  FakeSshHost(const FakeSshHost&) = delete;
  FakeSshHost& operator=(const FakeSshHost&) = delete;

  // The `remote.ssh_command` that reaches it, as argv.
  std::vector<std::string> SshArgv(bool needs_auth = false) const {
    std::vector<std::string> argv = {"env", "FAKE_SSH_HOME=" + home.string()};
    if (needs_auth) {
      argv.push_back("FAKE_SSH_NEEDS_AUTH=1");
    }
    argv.push_back(shim.string());
    return argv;
  }
};

}  // namespace microide::tests

#endif
