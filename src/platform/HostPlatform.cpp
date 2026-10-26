#include "platform/HostPlatform.h"

#include <string>

#include <clocale>
#include <csignal>
#include <langinfo.h>

namespace microide::platform {

void IgnoreBrokenPipeSignal() {
  std::signal(SIGPIPE, SIG_IGN);
}

std::string UseUtf8CharacterLocale() {
  auto is_utf8 = [] {
    const std::string_view codeset = nl_langinfo(CODESET);
    return codeset == "UTF-8" || codeset == "utf8";
  };
  if (std::setlocale(LC_CTYPE, "") == nullptr || !is_utf8()) {
    if (std::setlocale(LC_CTYPE, "C.UTF-8") == nullptr) {
      (void)std::setlocale(LC_CTYPE, "C");
    }
  }
  const char* now = std::setlocale(LC_CTYPE, nullptr);
  return now != nullptr ? now : "";
}

bool InterruptProcess(int pid) {
  return pid > 0 && kill(pid, SIGINT) == 0;
}

std::filesystem::path ProcessWorkingDirectory(int pid) {
  if (pid > 0) {
    std::error_code error;
    std::filesystem::path cwd =
        std::filesystem::read_symlink("/proc/" + std::to_string(pid) + "/cwd", error);
    if (!error) {
      return cwd;
    }
  }
  return {};
}

}  // namespace microide::platform
