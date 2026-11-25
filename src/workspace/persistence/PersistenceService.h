#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "persistence/PersistedRecordWriteQueue.h"
#include "workspace/persistence/WorkspacePersistenceFormat.h"

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
  bool SaveProjectSession(const std::filesystem::path& target_path, PersistedProjectSessionState state) const;

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

  void DeleteState(const std::filesystem::path& target_path) const;

  void FlushPendingWrites() const { write_queue_.Flush(); }

  bool WritesSettled() const { return write_queue_.settled(); }

  struct WriteFailureChange {
    std::optional<std::string> message;
  };
  std::optional<WriteFailureChange> TakeWriteFailureChange() const;

 private:
  mutable std::unordered_map<std::string, std::vector<std::byte>> backup_recovery_baseline_;

  mutable persistence::PersistedRecordWriteQueue write_queue_;
  mutable std::uint64_t reported_failure_revision_ = 0;
};

}
