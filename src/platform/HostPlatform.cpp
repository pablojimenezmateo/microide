#include "platform/HostPlatform.h"

#if !defined(_WIN32)
#include <csignal>
#endif

namespace microide::platform {

namespace {

#ifdef MICROIDE_TESTING
std::optional<HostPlatform>& HostPlatformOverride() {
  static std::optional<HostPlatform> platform;
  return platform;
}
#endif

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
#ifdef MICROIDE_TESTING
  if (const std::optional<HostPlatform> override = HostPlatformOverride(); override.has_value()) {
    return *override;
  }
#endif
  return NativeHostPlatform();
}

std::string_view HostPlatformName(HostPlatform platform) {
  switch (platform) {
    case HostPlatform::Linux:
      return "linux";
    case HostPlatform::MacOS:
      return "macos";
    case HostPlatform::Windows:
      return "windows";
  }
  return "linux";
}

#ifdef MICROIDE_TESTING
void SetHostPlatformOverrideForTesting(std::optional<HostPlatform> platform) {
  HostPlatformOverride() = platform;
}
#endif

void IgnoreBrokenPipeSignal() {
#if !defined(_WIN32)
  std::signal(SIGPIPE, SIG_IGN);
#endif
}

}  // namespace microide::platform
