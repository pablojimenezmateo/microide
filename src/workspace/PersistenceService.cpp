#include "workspace/PersistenceService.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "persistence/PersistedRecordReader.h"
#include "persistence/PersistedRecordWriter.h"
#include "util/PerformanceTrace.h"

namespace microide::workspace {
namespace {

using BackupRecoveryBaselines = std::unordered_map<std::string, std::vector<std::byte>>;
using WriteQueue = persistence::PersistedRecordWriteQueue;

std::string GuardKey(const std::filesystem::path& path) {
  return path.lexically_normal().string();
}

// True only when the state came from the `.bak` because the primary was PRESENT
// but unreadable/corrupt. A merely-absent primary (NotFound) is a normal
// first-run/fresh state and must not be guarded — creating it from the backup is
// the desired behavior.
bool RecoveredFromCorruptPrimary(const persistence::PersistedRecordReadResult& record) {
  using persistence::PersistedRecordReaderError;
  return record.used_backup && (record.primary_error == PersistedRecordReaderError::ReadFailed ||
                                record.primary_error == PersistedRecordReaderError::ParseFailed);
}

template <typename PersistedState, typename DecodeBinary, typename EncodeBinary>
bool LoadStructuredRecord(const std::filesystem::path& path,
                          DecodeBinary decode_binary,
                          EncodeBinary encode_binary,
                          PersistedState* out,
                          BackupRecoveryBaselines& baselines,
                          persistence::PersistedRecordWriteQueue& write_queue) {
  if (out == nullptr) {
    return false;
  }
  const std::string key = GuardKey(path);
  // A queued save for this path may not have landed yet; the file has to hold it
  // before it is read back. Scoped because it is the one place a caller can still
  // block on the writer -- a hot row here means something reads state back often
  // enough to serialize against its own saves.
  {
    util::PerformanceTrace::Scope scope("persistence::LoadFlushWait");
    write_queue.Flush();
  }
  const auto record = persistence::PersistedRecordReader::ReadFile(path);
  if (!record.has_value()) {
    return false;
  }
  if (!decode_binary(record->body, out)) {
    return false;
  }
  // What the file holds right now. A later save of unchanged state encodes to
  // exactly this, and the write queue can then skip the durable rewrite.
  write_queue.NoteOnDiskBody(path, record->body);
  if (RecoveredFromCorruptPrimary(*record)) {
    // Capture the baseline by re-encoding through the SAME encoder a Save uses,
    // so an unchanged state produces byte-identical output and is suppressed.
    std::vector<std::byte> baseline;
    if (encode_binary(*out, &baseline)) {
      baselines[key] = std::move(baseline);
    }
  } else {
    // A clean primary read (or a benign absent-primary fallback) supersedes any
    // stale guard left from an earlier corrupt-primary recovery.
    baselines.erase(key);
  }
  return true;
}

// Accepts the record unless the path is guarded and `body` is identical to the
// recovery baseline (no user mutation since a corrupt-primary recovery), in which
// case the write is suppressed and reported as success so callers treat it as
// persisted.
//
// "Accepted" rather than "written": the durable write happens on the queue's
// worker. Skipping an unchanged body now lives with the writer, which is what
// lets a failed write leave the memo alone and be retried.
bool WriteGuardedRecord(const std::filesystem::path& path, const std::vector<std::byte>& body,
                        std::uint32_t version, BackupRecoveryBaselines& baselines,
                        WriteQueue& write_queue) {
  const std::string key = GuardKey(path);
  if (auto it = baselines.find(key); it != baselines.end()) {
    if (it->second == body) {
      return true;
    }
    baselines.erase(it);
  }
  util::PerformanceTrace::Scope scope("persistence::QueueRecordWrite");
  write_queue.Queue(path, body, version);
  return true;
}

void RemoveLegacyArtifactIfStructuredExists(const std::filesystem::path& structured_path,
                                            std::string_view legacy_filename) {
  if (structured_path.empty() || legacy_filename.empty()) {
    return;
  }
  std::error_code error;
  if (!std::filesystem::exists(structured_path, error) || error) {
    return;
  }
  const std::filesystem::path legacy_path =
      structured_path.parent_path() / std::filesystem::path(legacy_filename);
  error.clear();
  if (!std::filesystem::exists(legacy_path, error) || error) {
    return;
  }
  error.clear();
  std::filesystem::remove(legacy_path, error);
}

void RemoveLegacyPersistenceArtifactsFor(const std::filesystem::path& target_path) {
  RemoveLegacyArtifactIfStructuredExists(target_path, "project.state.legacy");
  RemoveLegacyArtifactIfStructuredExists(target_path, "user.config.legacy");
  RemoveLegacyArtifactIfStructuredExists(target_path, "session.workspace.legacy");
  RemoveLegacyArtifactIfStructuredExists(target_path, "chat.conversations.legacy");
}

}  // namespace

bool PersistenceService::LoadUserConfig(const std::filesystem::path& target_path,
                                        PersistedUserConfigState* state) const {
  if (target_path.empty() || state == nullptr) {
    return false;
  }
  RemoveLegacyPersistenceArtifactsFor(target_path);
  return LoadStructuredRecord<PersistedUserConfigState>(target_path, DecodeUserConfigRecord,
                                                       EncodeUserConfigRecord, state,
                                                       backup_recovery_baseline_, write_queue_);
}

bool PersistenceService::SaveUserConfig(const std::filesystem::path& target_path,
                                        const PersistedUserConfigState& state) const {
  if (target_path.empty()) {
    return false;
  }
  std::vector<std::byte> body;
  return EncodeUserConfigRecord(state, &body) &&
         WriteGuardedRecord(target_path, body, 1u, backup_recovery_baseline_, write_queue_);
}

bool PersistenceService::LoadProjectConfig(const std::filesystem::path& target_path,
                                           PersistedProjectConfigState* state) const {
  if (target_path.empty() || state == nullptr) {
    return false;
  }
  RemoveLegacyPersistenceArtifactsFor(target_path);
  return LoadStructuredRecord<PersistedProjectConfigState>(target_path, DecodeProjectConfigRecord,
                                                          EncodeProjectConfigRecord, state,
                                                          backup_recovery_baseline_, write_queue_);
}

bool PersistenceService::SaveProjectConfig(const std::filesystem::path& target_path,
                                           const PersistedProjectConfigState& state) const {
  if (target_path.empty()) {
    return false;
  }
  std::vector<std::byte> body;
  return EncodeProjectConfigRecord(state, &body) &&
         WriteGuardedRecord(target_path, body, 2u, backup_recovery_baseline_, write_queue_);
}

bool PersistenceService::LoadProjectSession(const std::filesystem::path& target_path,
                                            PersistedProjectSessionState* state) const {
  if (target_path.empty() || state == nullptr) {
    return false;
  }
  RemoveLegacyPersistenceArtifactsFor(target_path);
  return LoadStructuredRecord<PersistedProjectSessionState>(target_path, DecodeProjectSessionRecord,
                                                           EncodeProjectSessionRecord, state,
                                                           backup_recovery_baseline_, write_queue_);
}

bool PersistenceService::SaveProjectSession(const std::filesystem::path& target_path,
                                            const PersistedProjectSessionState& state) const {
  if (target_path.empty()) {
    return false;
  }
  std::vector<std::byte> body;
  return EncodeProjectSessionRecord(state, &body) &&
         WriteGuardedRecord(target_path, body, 3u, backup_recovery_baseline_, write_queue_);
}

bool PersistenceService::LoadWorkspaceSession(const std::filesystem::path& target_path,
                                              PersistedWorkspaceSessionState* state) const {
  if (target_path.empty() || state == nullptr) {
    return false;
  }
  RemoveLegacyPersistenceArtifactsFor(target_path);
  return LoadStructuredRecord<PersistedWorkspaceSessionState>(
      target_path, DecodeWorkspaceSessionRecord, EncodeWorkspaceSessionRecord, state,
      backup_recovery_baseline_, write_queue_);
}

bool PersistenceService::SaveWorkspaceSession(const std::filesystem::path& target_path,
                                              const PersistedWorkspaceSessionState& state) const {
  if (target_path.empty()) {
    return false;
  }
  std::vector<std::byte> body;
  return EncodeWorkspaceSessionRecord(state, &body) &&
         WriteGuardedRecord(target_path, body, 4u, backup_recovery_baseline_, write_queue_);
}

bool PersistenceService::LoadDebugState(const std::filesystem::path& target_path,
                                        PersistedDebugState* state) const {
  if (target_path.empty() || state == nullptr) {
    return false;
  }
  return LoadStructuredRecord<PersistedDebugState>(target_path, DecodeDebugStateRecord,
                                                    EncodeDebugStateRecord, state,
                                                    backup_recovery_baseline_, write_queue_);
}

bool PersistenceService::SaveDebugState(const std::filesystem::path& target_path,
                                        const PersistedDebugState& state) const {
  if (target_path.empty()) {
    return false;
  }
  std::vector<std::byte> body;
  return EncodeDebugStateRecord(state, &body) &&
         WriteGuardedRecord(target_path, body, 5u, backup_recovery_baseline_, write_queue_);
}

bool PersistenceService::LoadMruState(const std::filesystem::path& target_path,
                                      PersistedMruState* state) const {
  if (target_path.empty() || state == nullptr) {
    return false;
  }
  return LoadStructuredRecord<PersistedMruState>(target_path, DecodeMruRecord, EncodeMruRecord, state,
                                                  backup_recovery_baseline_, write_queue_);
}

bool PersistenceService::SaveMruState(const std::filesystem::path& target_path,
                                      const PersistedMruState& state) const {
  if (target_path.empty()) {
    return false;
  }
  std::vector<std::byte> body;
  return EncodeMruRecord(state, &body) &&
         WriteGuardedRecord(target_path, body, 6u, backup_recovery_baseline_, write_queue_);
}

void PersistenceService::DeleteState(const std::filesystem::path& target_path) const {
  if (target_path.empty()) {
    return;
  }
  const std::string key = GuardKey(target_path);
  backup_recovery_baseline_.erase(key);
  // Ordered behind any write already accepted for this path, and superseding any
  // still queued, without waiting for either.
  write_queue_.QueueDelete(target_path);
}

}  // namespace microide::workspace
