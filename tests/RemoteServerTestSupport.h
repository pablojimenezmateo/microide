#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "TestSupport.h"
#include "platform/Subprocess.h"

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>

namespace microide::tests {

// A socket directory for a real microide-server daemon. Short on purpose: the
// AF_UNIX path limit makes the usual long temp paths unusable for a socket. The
// destructor stops the daemon and removes the tree, so a failed test leaks
// neither.
class ShortServerDir {
 public:
  ShortServerDir() {
    std::string pattern = (std::filesystem::temp_directory_path() / "mip.XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    Expect(::mkdtemp(buffer.data()) != nullptr, "mkdtemp");
    path_ = std::filesystem::path(buffer.data()) / "s";
  }
  ~ShortServerDir() {
    platform::RunSubprocess({MICROIDE_SERVER_BINARY, "stop", "--socket-dir", path_.string()},
                            platform::SubprocessOptions{.timeout_ms = 10000});
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
