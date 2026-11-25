#include "workspace/persistence/PersistenceService.h"

#include <memory>

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
#include "util/Log.h"
#include "util/PerformanceTrace.h"

namespace microide::workspace {
namespace {

using BackupRecoveryBaselines = std::unordered_map<std::string, std::vector<std::byte>>;
using WriteQueue = persistence::PersistedRecordWriteQueue;

std::string GuardKey(const std::filesystem::path& path) {
  return path.lexically_normal().string();
}

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
  {
    util::PerformanceTrace::Scope scope("persistence::LoadFlushWait");
    write_queue.Flush();
  }
  const auto record = persistence::PersistedRecordReader::ReadFile(path);
  if (!record.has_value()) {
    return false;
  }
  if (record->used_backup) {
    util::Log("persistence: " + path.string() + " unreadable (error " +
              std::to_string(static_cast<int>(record->primary_error)) + "), read its backup");
  }
  if (!decode_binary(record->body, out)) {
    util::Log("persistence: " + path.string() + (record->used_backup ? " (backup)" : "") +
              " read but did not decode");
    return false;
  }
  write_queue.NoteOnDiskBody(path, record->body);
  if (RecoveredFromCorruptPrimary(*record)) {
    std::vector<std::byte> baseline;
    if (encode_binary(*out, &baseline)) {
      baselines[key] = std::move(baseline);
    }
  } else {
    baselines.erase(key);
  }
  return true;
}

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

}

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
                                            PersistedProjectSessionState state) const {
  if (target_path.empty()) {
    return false;
  }
  if (backup_recovery_baseline_.contains(GuardKey(target_path))) {
    std::vector<std::byte> body;
    return EncodeProjectSessionRecord(state, &body) &&
           WriteGuardedRecord(target_path, body, 3u, backup_recovery_baseline_, write_queue_);
  }
  auto shared = std::make_shared<const PersistedProjectSessionState>(std::move(state));
  write_queue_.QueueEncoded(
      target_path,
      [shared = std::move(shared)](std::vector<std::byte>* body) { return EncodeProjectSessionRecord(*shared, body); },
      3u);
  return true;
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
  write_queue_.QueueDelete(target_path);
}

}
