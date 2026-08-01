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

namespace microide::workspace {
namespace {

using BackupRecoveryBaselines = std::unordered_map<std::string, std::vector<std::byte>>;
using PersistedBodyMemo = std::unordered_map<std::string, std::vector<std::byte>>;

// Ceiling on a memoized body. Above it the write-skip check is dropped rather
// than holding a second copy of a large session in memory.
constexpr std::size_t kMaxMemoizedBodyBytes = 1024ull * 1024;

std::string GuardKey(const std::filesystem::path& path) {
  return path.lexically_normal().string();
}

void RememberBody(PersistedBodyMemo& memo, const std::string& key,
                  const std::vector<std::byte>& body) {
  if (body.size() > kMaxMemoizedBodyBytes) {
    memo.erase(key);
    return;
  }
  memo[key] = body;
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
                          PersistedBodyMemo& memo) {
  if (out == nullptr) {
    return false;
  }
  const std::string key = GuardKey(path);
  const auto record = persistence::PersistedRecordReader::ReadFile(path);
  if (!record.has_value()) {
    return false;
  }
  if (!decode_binary(record->body, out)) {
    return false;
  }
  // What the file holds right now. A later save of unchanged state encodes to
  // exactly this, and WriteGuardedRecord can then skip the durable rewrite.
  RememberBody(memo, key, record->body);
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

// Writes unless the path is guarded and `body` is identical to the recovery
// baseline (no user mutation since a corrupt-primary recovery), in which case the
// write is suppressed and reported as success so callers treat it as persisted.
bool WriteGuardedRecord(const std::filesystem::path& path, const std::vector<std::byte>& body,
                        std::uint32_t version, BackupRecoveryBaselines& baselines,
                        PersistedBodyMemo& memo) {
  const std::string key = GuardKey(path);
  if (auto it = baselines.find(key); it != baselines.end()) {
    if (it->second == body) {
      return true;
    }
    baselines.erase(it);
  }
  // Already on disk, byte for byte: skip the temp write + fsync + backup rotation
  // + rename. The existence check keeps this honest — if the file went away
  // underneath us the memo is stale and the record has to be rewritten.
  if (const auto memo_it = memo.find(key); memo_it != memo.end() && memo_it->second == body) {
    std::error_code error;
    if (std::filesystem::exists(path, error) && !error) {
      return true;
    }
    memo.erase(memo_it);
  }
  if (!persistence::PersistedRecordWriter::WriteFile(path, body, version)) {
    memo.erase(key);
    return false;
  }
  RememberBody(memo, key, body);
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
                                                       backup_recovery_baseline_, persisted_body_memo_);
}

bool PersistenceService::SaveUserConfig(const std::filesystem::path& target_path,
                                        const PersistedUserConfigState& state) const {
  if (target_path.empty()) {
    return false;
  }
  std::vector<std::byte> body;
  return EncodeUserConfigRecord(state, &body) &&
         WriteGuardedRecord(target_path, body, 1u, backup_recovery_baseline_, persisted_body_memo_);
}

bool PersistenceService::LoadProjectConfig(const std::filesystem::path& target_path,
                                           PersistedProjectConfigState* state) const {
  if (target_path.empty() || state == nullptr) {
    return false;
  }
  RemoveLegacyPersistenceArtifactsFor(target_path);
  return LoadStructuredRecord<PersistedProjectConfigState>(target_path, DecodeProjectConfigRecord,
                                                          EncodeProjectConfigRecord, state,
                                                          backup_recovery_baseline_, persisted_body_memo_);
}

bool PersistenceService::SaveProjectConfig(const std::filesystem::path& target_path,
                                           const PersistedProjectConfigState& state) const {
  if (target_path.empty()) {
    return false;
  }
  std::vector<std::byte> body;
  return EncodeProjectConfigRecord(state, &body) &&
         WriteGuardedRecord(target_path, body, 2u, backup_recovery_baseline_, persisted_body_memo_);
}

bool PersistenceService::LoadProjectSession(const std::filesystem::path& target_path,
                                            PersistedProjectSessionState* state) const {
  if (target_path.empty() || state == nullptr) {
    return false;
  }
  RemoveLegacyPersistenceArtifactsFor(target_path);
  return LoadStructuredRecord<PersistedProjectSessionState>(target_path, DecodeProjectSessionRecord,
                                                           EncodeProjectSessionRecord, state,
                                                           backup_recovery_baseline_, persisted_body_memo_);
}

bool PersistenceService::SaveProjectSession(const std::filesystem::path& target_path,
                                            const PersistedProjectSessionState& state) const {
  if (target_path.empty()) {
    return false;
  }
  std::vector<std::byte> body;
  return EncodeProjectSessionRecord(state, &body) &&
         WriteGuardedRecord(target_path, body, 3u, backup_recovery_baseline_, persisted_body_memo_);
}

bool PersistenceService::LoadWorkspaceSession(const std::filesystem::path& target_path,
                                              PersistedWorkspaceSessionState* state) const {
  if (target_path.empty() || state == nullptr) {
    return false;
  }
  RemoveLegacyPersistenceArtifactsFor(target_path);
  return LoadStructuredRecord<PersistedWorkspaceSessionState>(
      target_path, DecodeWorkspaceSessionRecord, EncodeWorkspaceSessionRecord, state,
      backup_recovery_baseline_, persisted_body_memo_);
}

bool PersistenceService::SaveWorkspaceSession(const std::filesystem::path& target_path,
                                              const PersistedWorkspaceSessionState& state) const {
  if (target_path.empty()) {
    return false;
  }
  std::vector<std::byte> body;
  return EncodeWorkspaceSessionRecord(state, &body) &&
         WriteGuardedRecord(target_path, body, 4u, backup_recovery_baseline_, persisted_body_memo_);
}

bool PersistenceService::LoadDebugState(const std::filesystem::path& target_path,
                                        PersistedDebugState* state) const {
  if (target_path.empty() || state == nullptr) {
    return false;
  }
  return LoadStructuredRecord<PersistedDebugState>(target_path, DecodeDebugStateRecord,
                                                    EncodeDebugStateRecord, state,
                                                    backup_recovery_baseline_, persisted_body_memo_);
}

bool PersistenceService::SaveDebugState(const std::filesystem::path& target_path,
                                        const PersistedDebugState& state) const {
  if (target_path.empty()) {
    return false;
  }
  std::vector<std::byte> body;
  return EncodeDebugStateRecord(state, &body) &&
         WriteGuardedRecord(target_path, body, 5u, backup_recovery_baseline_, persisted_body_memo_);
}

bool PersistenceService::LoadMruState(const std::filesystem::path& target_path,
                                      PersistedMruState* state) const {
  if (target_path.empty() || state == nullptr) {
    return false;
  }
  return LoadStructuredRecord<PersistedMruState>(target_path, DecodeMruRecord, EncodeMruRecord, state,
                                                  backup_recovery_baseline_, persisted_body_memo_);
}

bool PersistenceService::SaveMruState(const std::filesystem::path& target_path,
                                      const PersistedMruState& state) const {
  if (target_path.empty()) {
    return false;
  }
  std::vector<std::byte> body;
  return EncodeMruRecord(state, &body) &&
         WriteGuardedRecord(target_path, body, 6u, backup_recovery_baseline_, persisted_body_memo_);
}

void PersistenceService::DeleteState(const std::filesystem::path& target_path) const {
  if (target_path.empty()) {
    return;
  }
  const std::string key = GuardKey(target_path);
  backup_recovery_baseline_.erase(key);
  // The record is going away, so the memo of what it held must go with it or a
  // later save of that same body would be skipped against a file that no longer
  // exists. (The existence probe in WriteGuardedRecord also catches this; erasing
  // here keeps the memo from outliving its record in the first place.)
  persisted_body_memo_.erase(key);
  std::error_code error;
  std::filesystem::remove(target_path, error);
  // Also remove the backup, or the reader would fall back to it and resurrect the
  // state this call is meant to clear.
  std::filesystem::remove(persistence::PersistedRecordWriter::BackupPathFor(target_path), error);
}

}  // namespace microide::workspace
