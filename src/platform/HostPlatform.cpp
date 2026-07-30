#include "platform/HostPlatform.h"

#if !defined(_WIN32)
#include <csignal>
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

}  // namespace microide::platform
