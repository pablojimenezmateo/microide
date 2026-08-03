#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "persistence/PersistedRecordWriteQueue.h"
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

  // Block until every accepted Save has actually reached disk.
  //
  // Saves are applied on a background thread (see PersistedRecordWriteQueue), so
  // a Save returning true means the record was accepted, not that the bytes have
  // landed. Anything that needs the old synchronous guarantee -- shutting down,
  // or handing the file to something outside this process -- calls this. The
  // Load/Delete paths already flush internally.
  void FlushPendingWrites() const { write_queue_.Flush(); }

 private:
  // Paths whose in-memory state was recovered from a `.bak` because the primary
  // was present but unreadable/corrupt (not merely absent). Maps the normalized
  // path to the record body re-encoded from the recovered state. While a path is
  // guarded, a Save whose encoded body equals this baseline — i.e. no user
  // mutation since recovery — is suppressed, so the still-recoverable corrupt
  // primary is not clobbered with stale backup-derived state. A differing body
  // (a real mutation) writes normally and clears the guard.
  mutable std::unordered_map<std::string, std::vector<std::byte>> backup_recovery_baseline_;

  // Applies the durable record writes off the shell thread, and owns the
  // "already on disk byte for byte" memo that skips redundant ones.
  mutable persistence::PersistedRecordWriteQueue write_queue_;
};

}  // namespace microide::workspace
