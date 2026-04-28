#include "workspace/PersistenceService.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "persistence/PersistedRecordReader.h"
#include "persistence/PersistedRecordWriter.h"
#include "util/TextFileIO.h"
#include "workspace/WorkspacePersistenceLegacyFormat.h"

namespace microide::workspace {
namespace {

bool ArchiveLegacyFile(const std::filesystem::path& legacy_path) {
  if (legacy_path.empty()) {
    return false;
  }
  std::error_code error;
  if (!std::filesystem::exists(legacy_path, error) || error) {
    return false;
  }
  const std::filesystem::path archived_path = legacy_path.string() + ".legacy";
  std::filesystem::remove(archived_path, error);
  error.clear();
  std::filesystem::rename(legacy_path, archived_path, error);
  return !error;
}

template <typename PersistedState, typename ParseLegacy, typename EncodeBinary, typename DecodeBinary>
bool ImportLegacyTextFile(const std::filesystem::path& source_path,
                          const std::filesystem::path& target_path,
                          std::uint32_t capability_flags,
                          ParseLegacy parse_legacy,
                          EncodeBinary encode_binary,
                          DecodeBinary decode_binary) {
  const auto text = util::ReadTextFile(source_path);
  if (!text.has_value()) {
    return false;
  }
  PersistedState parsed;
  if (!parse_legacy(*text, &parsed)) {
    return false;
  }
  std::vector<std::byte> body;
  if (!encode_binary(parsed, &body) ||
      !persistence::PersistedRecordWriter::WriteFile(target_path, body, capability_flags)) {
    return false;
  }
  const auto reread = persistence::PersistedRecordReader::ReadFile(target_path);
  PersistedState verify;
  if (!reread.has_value() || !decode_binary(reread->body, &verify)) {
    return false;
  }
  if (source_path == target_path) {
    ArchiveLegacyFile(persistence::PersistedRecordWriter::BackupPathFor(target_path));
  } else {
    ArchiveLegacyFile(source_path);
  }
  return true;
}

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

bool ParseLegacyChatConversationText(std::string_view text, PersistedChatState* chat) {
  if (chat == nullptr) {
    return false;
  }
  PersistedProjectSessionState wrapped;
  wrapped.sidebar_visible = true;
  wrapped.sidebar_width = 288.0f;
  wrapped.bottom_panel_height = 184.0f;
  wrapped.active_tab_index = 0;
  const std::string synthetic = std::string("version 5\nsidebar-visible 1\nsidebar-width 288\n"
                                            "bottom-panel-height 184\nactive-tab 0\n") +
                                std::string(text);
  if (!ParseProjectSessionText(synthetic, &wrapped)) {
    return false;
  }
  *chat = std::move(wrapped.chat);
  return true;
}

bool TryImportLegacyConversationRegistry(const std::filesystem::path& legacy_chat_path,
                                         const std::filesystem::path& target_session_path) {
  const auto text = util::ReadTextFile(legacy_chat_path);
  if (!text.has_value()) {
    return false;
  }
  PersistedChatState imported_chat;
  if (!ParseLegacyChatConversationText(*text, &imported_chat)) {
    return false;
  }

  PersistedProjectSessionState merged;
  if (!LoadStructuredRecord<PersistedProjectSessionState>(target_session_path,
                                                          DecodeProjectSessionRecord, &merged)) {
    merged.sidebar_visible = true;
    merged.sidebar_width = 288.0f;
    merged.bottom_panel_height = 184.0f;
    merged.active_tab_index = 0;
  }
  merged.chat = std::move(imported_chat);

  std::vector<std::byte> body;
  if (!EncodeProjectSessionRecord(merged, &body) ||
      !persistence::PersistedRecordWriter::WriteFile(target_session_path, body, 3u)) {
    return false;
  }
  const auto reread = persistence::PersistedRecordReader::ReadFile(target_session_path);
  PersistedProjectSessionState verify;
  if (!reread.has_value() || !DecodeProjectSessionRecord(reread->body, &verify)) {
    return false;
  }
  ArchiveLegacyFile(legacy_chat_path);
  return true;
}

}  // namespace

bool PersistenceService::LoadUserConfig(const std::filesystem::path& target_path,
                                        PersistedUserConfigState* state) const {
  if (target_path.empty() || state == nullptr) {
    return false;
  }

  const std::filesystem::path legacy_named_path = target_path.parent_path() / "user.config";
  std::error_code error;
  if (std::filesystem::exists(legacy_named_path, error) && !error) {
    ImportLegacyTextFile<PersistedUserConfigState>(legacy_named_path, target_path, 1u,
                                                   ParseUserConfigText, EncodeUserConfigRecord,
                                                   DecodeUserConfigRecord);
  }
  error.clear();
  if (std::filesystem::exists(target_path, error) && !error &&
      !persistence::PersistedRecordReader::ReadFile(target_path).has_value()) {
    ImportLegacyTextFile<PersistedUserConfigState>(target_path, target_path, 1u,
                                                   ParseUserConfigText, EncodeUserConfigRecord,
                                                   DecodeUserConfigRecord);
  }

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

  const std::filesystem::path legacy_named_path = target_path.parent_path() / "project.state";
  std::error_code error;
  if (std::filesystem::exists(legacy_named_path, error) && !error) {
    ImportLegacyTextFile<PersistedProjectConfigState>(legacy_named_path, target_path, 2u,
                                                      ParseProjectConfigText, EncodeProjectConfigRecord,
                                                      DecodeProjectConfigRecord);
  }
  error.clear();
  if (std::filesystem::exists(target_path, error) && !error &&
      !persistence::PersistedRecordReader::ReadFile(target_path).has_value()) {
    ImportLegacyTextFile<PersistedProjectConfigState>(target_path, target_path, 2u,
                                                      ParseProjectConfigText, EncodeProjectConfigRecord,
                                                      DecodeProjectConfigRecord);
  }

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

  const std::filesystem::path legacy_session_path = target_path.parent_path() / "session.workspace";
  const std::filesystem::path legacy_chat_path = target_path.parent_path() / "chat.conversations";

  std::error_code error;
  if (std::filesystem::exists(legacy_session_path, error) && !error) {
    ImportLegacyTextFile<PersistedProjectSessionState>(legacy_session_path, target_path, 3u,
                                                       ParseProjectSessionText, EncodeProjectSessionRecord,
                                                       DecodeProjectSessionRecord);
  }
  error.clear();
  if (std::filesystem::exists(legacy_chat_path, error) && !error) {
    TryImportLegacyConversationRegistry(legacy_chat_path, target_path);
  }
  error.clear();
  if (std::filesystem::exists(target_path, error) && !error &&
      !persistence::PersistedRecordReader::ReadFile(target_path).has_value()) {
    ImportLegacyTextFile<PersistedProjectSessionState>(target_path, target_path, 3u,
                                                       ParseProjectSessionText, EncodeProjectSessionRecord,
                                                       DecodeProjectSessionRecord);
  }

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

  const std::filesystem::path legacy_named_path = target_path.parent_path() / "session.workspace";
  std::error_code error;
  if (std::filesystem::exists(legacy_named_path, error) && !error) {
    ImportLegacyTextFile<PersistedWorkspaceSessionState>(legacy_named_path, target_path, 4u,
                                                         ParseWorkspaceSessionText, EncodeWorkspaceSessionRecord,
                                                         DecodeWorkspaceSessionRecord);
  }
  error.clear();
  if (std::filesystem::exists(target_path, error) && !error &&
      !persistence::PersistedRecordReader::ReadFile(target_path).has_value()) {
    ImportLegacyTextFile<PersistedWorkspaceSessionState>(target_path, target_path, 4u,
                                                         ParseWorkspaceSessionText, EncodeWorkspaceSessionRecord,
                                                         DecodeWorkspaceSessionRecord);
  }

  return LoadStructuredRecord<PersistedWorkspaceSessionState>(target_path, DecodeWorkspaceSessionRecord,
                                                              state);
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

}  // namespace microide::workspace
