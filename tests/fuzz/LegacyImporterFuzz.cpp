#include "workspace/WorkspacePersistenceLegacyFormat.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr) {
    return 0;
  }
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  microide::workspace::PersistedUserConfigState user_config;
  microide::workspace::PersistedProjectConfigState project_config;
  microide::workspace::PersistedProjectSessionState project_session;
  microide::workspace::PersistedWorkspaceSessionState workspace_session;
  (void)microide::workspace::ParseUserConfigText(input, &user_config);
  (void)microide::workspace::ParseProjectConfigText(input, &project_config);
  (void)microide::workspace::ParseProjectSessionText(input, &project_session);
  (void)microide::workspace::ParseWorkspaceSessionText(input, &workspace_session);
  return 0;
}
