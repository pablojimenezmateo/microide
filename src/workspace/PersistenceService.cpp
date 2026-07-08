#include "workspace/PersistenceService.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

#include "persistence/PersistedRecordReader.h"
#include "persistence/PersistedRecordWriter.h"

namespace microide::workspace {
namespace {

template <typename PersistedState, typename DecodeBinary>
bool LoadStructuredRecord(const std::filesystem::path& path,
                          DecodeBinary decode_binary,
                          PersistedState* out) {
  if (out == nullptr) {
    return false;
  }
  if (const auto record = persistence::PersistedRecordReader::ReadFile(path); record.has_value()) {
    return decode_binary(record->body, out);
  }
  return false;
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
  return LoadStructuredRecord<PersistedUserConfigState>(target_path, DecodeUserConfigRecord, state);
}

bool PersistenceService::SaveUserConfig(const std::filesystem::path& target_path,
                                        const PersistedUserConfigState& state) const {
  if (target_path.empty()) {
    return false;
  }
  std::vector<std::byte> body;
  return EncodeUserConfigRecord(state, &body) &&
         persistence::PersistedRecordWriter::WriteFile(target_path, body, 1u);
}

bool PersistenceService::LoadProjectConfig(const std::filesystem::path& target_path,
                                           PersistedProjectConfigState* state) const {
  if (target_path.empty() || state == nullptr) {
    return false;
  }
  RemoveLegacyPersistenceArtifactsFor(target_path);
  return LoadStructuredRecord<PersistedProjectConfigState>(target_path, DecodeProjectConfigRecord,
                                                           state);
}

bool PersistenceService::SaveProjectConfig(const std::filesystem::path& target_path,
                                           const PersistedProjectConfigState& state) const {
  if (target_path.empty()) {
    return false;
  }
  std::vector<std::byte> body;
  return EncodeProjectConfigRecord(state, &body) &&
         persistence::PersistedRecordWriter::WriteFile(target_path, body, 2u);
}

bool PersistenceService::LoadProjectSession(const std::filesystem::path& target_path,
                                            PersistedProjectSessionState* state) const {
  if (target_path.empty() || state == nullptr) {
    return false;
  }
  RemoveLegacyPersistenceArtifactsFor(target_path);
  return LoadStructuredRecord<PersistedProjectSessionState>(target_path, DecodeProjectSessionRecord,
                                                            state);
}

bool PersistenceService::SaveProjectSession(const std::filesystem::path& target_path,
                                            const PersistedProjectSessionState& state) const {
  if (target_path.empty()) {
    return false;
  }
  std::vector<std::byte> body;
  return EncodeProjectSessionRecord(state, &body) &&
         persistence::PersistedRecordWriter::WriteFile(target_path, body, 3u);
}

bool PersistenceService::LoadWorkspaceSession(const std::filesystem::path& target_path,
                                              PersistedWorkspaceSessionState* state) const {
  if (target_path.empty() || state == nullptr) {
    return false;
  }
  RemoveLegacyPersistenceArtifactsFor(target_path);
  return LoadStructuredRecord<PersistedWorkspaceSessionState>(target_path,
                                                              DecodeWorkspaceSessionRecord, state);
}

bool PersistenceService::SaveWorkspaceSession(const std::filesystem::path& target_path,
                                              const PersistedWorkspaceSessionState& state) const {
  if (target_path.empty()) {
    return false;
  }
  std::vector<std::byte> body;
  return EncodeWorkspaceSessionRecord(state, &body) &&
         persistence::PersistedRecordWriter::WriteFile(target_path, body, 4u);
}

bool PersistenceService::LoadDebugState(const std::filesystem::path& target_path,
                                        PersistedDebugState* state) const {
  if (target_path.empty() || state == nullptr) {
    return false;
  }
  return LoadStructuredRecord<PersistedDebugState>(target_path, DecodeDebugStateRecord, state);
}

bool PersistenceService::SaveDebugState(const std::filesystem::path& target_path,
                                        const PersistedDebugState& state) const {
  if (target_path.empty()) {
    return false;
  }
  std::vector<std::byte> body;
  return EncodeDebugStateRecord(state, &body) &&
         persistence::PersistedRecordWriter::WriteFile(target_path, body, 5u);
}

bool PersistenceService::LoadMruState(const std::filesystem::path& target_path,
                                      PersistedMruState* state) const {
  if (target_path.empty() || state == nullptr) {
    return false;
  }
  return LoadStructuredRecord<PersistedMruState>(target_path, DecodeMruRecord, state);
}

bool PersistenceService::SaveMruState(const std::filesystem::path& target_path,
                                      const PersistedMruState& state) const {
  if (target_path.empty()) {
    return false;
  }
  std::vector<std::byte> body;
  return EncodeMruRecord(state, &body) &&
         persistence::PersistedRecordWriter::WriteFile(target_path, body, 6u);
}

void PersistenceService::DeleteState(const std::filesystem::path& target_path) const {
  if (target_path.empty()) {
    return;
  }
  std::error_code error;
  std::filesystem::remove(target_path, error);
  // Also remove the backup, or the reader would fall back to it and resurrect the
  // state this call is meant to clear.
  std::filesystem::remove(persistence::PersistedRecordWriter::BackupPathFor(target_path), error);
}

}  // namespace microide::workspace
