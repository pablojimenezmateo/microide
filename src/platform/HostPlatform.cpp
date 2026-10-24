#include "platform/HostPlatform.h"

#include <string>

#if !defined(_WIN32)
#include <clocale>
#include <csignal>
#include <langinfo.h>
#endif

namespace microide::platform {

namespace {

std::optional<HostPlatform>& HostPlatformOverride() {
  static std::optional<HostPlatform> platform;
  return platform;
}

HostPlatform NativeHostPlatform() {
#if defined(_WIN32)
  return HostPlatform::Windows;
#elif defined(__APPLE__)
  return HostPlatform::MacOS;
#else
  return HostPlatform::Linux;
#endif
}

}  // namespace

HostPlatform CurrentHostPlatform() {
  // Test override defaults to unset, so this is native in production builds.
  if (const std::optional<HostPlatform> override = HostPlatformOverride(); override.has_value()) {
    return *override;
  }
  return NativeHostPlatform();
}

bool HostPathsAreCaseInsensitive() {
  switch (CurrentHostPlatform()) {
    case HostPlatform::Windows:
    case HostPlatform::MacOS:
      return true;
    case HostPlatform::Linux:
      return false;
  }
  return false;
}

void SetHostPlatformOverrideForTesting(std::optional<HostPlatform> platform) {
  HostPlatformOverride() = platform;
}

void IgnoreBrokenPipeSignal() {
#if !defined(_WIN32)
  std::signal(SIGPIPE, SIG_IGN);
#endif
}

std::string UseUtf8CharacterLocale() {
#if defined(_WIN32)
  return {};
#else
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
#endif
}

std::filesystem::path ProcessWorkingDirectory(int pid) {
#if defined(__linux__)
  if (pid > 0) {
    std::error_code error;
    std::filesystem::path cwd =
        std::filesystem::read_symlink("/proc/" + std::to_string(pid) + "/cwd", error);
    if (!error) {
      return cwd;
    }
  }
#else
  (void)pid;
#endif
  return {};
}

}  // namespace microide::platform
