#include "platform/HostPlatform.h"

#include <string>

#include <clocale>
#include <csignal>
#include <cstdlib>
#include <langinfo.h>
#include <string_view>
#include <unistd.h>

#if defined(__linux__)
#include <sys/prctl.h>
#endif

namespace microide::platform {

void IgnoreBrokenPipeSignal() {
  std::signal(SIGPIPE, SIG_IGN);
}

void AllowParentToTraceWhenAskedByQa() {
#if defined(__linux__)
  const char* asked = std::getenv("MICROIDE_QA_PARENT_MAY_TRACE");
  if (asked != nullptr && std::string_view(asked) == "1") {
    (void)prctl(PR_SET_PTRACER, static_cast<unsigned long>(getppid()), 0, 0, 0);  // Best effort: a stall's backtraces.
  }
#endif
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
