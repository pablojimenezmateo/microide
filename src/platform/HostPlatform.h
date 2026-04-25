#pragma once

#include <optional>
#include <string_view>

namespace microide::platform {

enum class HostPlatform {
  Linux,
  MacOS,
  Windows,
};

HostPlatform CurrentHostPlatform();
std::string_view HostPlatformName(HostPlatform platform);

#ifdef MICROIDE_TESTING
void SetHostPlatformOverrideForTesting(std::optional<HostPlatform> platform);
#endif

}  // namespace microide::platform
