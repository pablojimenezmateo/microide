#pragma once

#include <filesystem>

#include "workspace/WorkspacePersistenceFormat.h"

namespace microide::workspace {

class PersistenceService {
 public:
  bool LoadUserConfig(const std::filesystem::path& target_path,
                      PersistedUserConfigState* state) const;
  bool SaveUserConfig(const std::filesystem::path& target_path,
                      const PersistedUserConfigState& state) const;

  bool LoadProjectConfig(const std::filesystem::path& target_path,
                         PersistedProjectConfigState* state) const;
  bool SaveProjectConfig(const std::filesystem::path& target_path,
                         const PersistedProjectConfigState& state) const;

  bool LoadProjectSession(const std::filesystem::path& target_path,
                          PersistedProjectSessionState* state) const;
  bool SaveProjectSession(const std::filesystem::path& target_path,
                          const PersistedProjectSessionState& state) const;

  bool LoadWorkspaceSession(const std::filesystem::path& target_path,
                            PersistedWorkspaceSessionState* state) const;
  bool SaveWorkspaceSession(const std::filesystem::path& target_path,
                            const PersistedWorkspaceSessionState& state) const;

  bool LoadDebugState(const std::filesystem::path& target_path,
                      PersistedDebugState* state) const;
  bool SaveDebugState(const std::filesystem::path& target_path,
                      const PersistedDebugState& state) const;
};

}  // namespace microide::workspace
