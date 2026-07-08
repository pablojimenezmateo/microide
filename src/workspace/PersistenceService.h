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

  bool LoadMruState(const std::filesystem::path& target_path,
                    PersistedMruState* state) const;
  bool SaveMruState(const std::filesystem::path& target_path,
                    const PersistedMruState& state) const;

  // Remove a persisted record entirely: both the primary file AND its `.bak`
  // sibling. Callers that clear state (e.g. debug state with nothing left to
  // persist) must use this rather than removing only the primary — the reader
  // falls back to the backup when the primary is missing, so a lone primary
  // remove would let stale state resurrect on the next restore.
  void DeleteState(const std::filesystem::path& target_path) const;
};

}  // namespace microide::workspace
