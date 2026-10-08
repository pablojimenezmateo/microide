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

}  // namespace microide::tests

#endif
