#include "persistence/PersistedRecordDump.h"

#include "persistence/PersistedRecord.h"
#include "persistence/PersistedRecordReader.h"
#include "workspace/WorkspacePersistenceFormat.h"

#include <cstddef>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

namespace microide::persistence {
namespace {

std::string ReaderErrorToString(PersistedRecordReaderError error) {
  switch (error) {
    case PersistedRecordReaderError::None: return "none";
    case PersistedRecordReaderError::NotFound: return "not_found";
    case PersistedRecordReaderError::ReadFailed: return "read_failed";
    case PersistedRecordReaderError::ParseFailed: return "parse_failed";
    case PersistedRecordReaderError::UnsupportedVersion: return "unsupported_version";
  }
  return "unknown";
}

void AppendProjectSessionSummary(const workspace::PersistedProjectSessionState& state,
                                 std::ostringstream& stream) {
  stream << "decoded.project_session.sidebar_visible: " << (state.sidebar_visible ? "true" : "false")
         << '\n';
  stream << "decoded.project_session.sidebar_width: " << state.sidebar_width << '\n';
  stream << "decoded.project_session.bottom_panel_height: " << state.bottom_panel_height << '\n';
  stream << "decoded.project_session.active_tab_index: " << state.active_tab_index << '\n';
  stream << "decoded.project_session.tab_count: " << state.tabs.size() << '\n';
  stream << "decoded.project_session.active_conversation_id: " << state.chat.active_conversation_id
         << '\n';
  stream << "decoded.project_session.conversation_count: " << state.chat.conversations.size() << '\n';
}

void AppendUserConfigSummary(const workspace::PersistedUserConfigState& state,
                             std::ostringstream& stream) {
  stream << "decoded.user_config.ui_scale: " << state.ui_scale << '\n';
  stream << "decoded.user_config.settings_count: " << state.settings.size() << '\n';
  stream << "decoded.user_config.disabled_keybindings_count: "
         << state.disabled_keybinding_ids.size() << '\n';
}

void AppendProjectConfigSummary(const workspace::PersistedProjectConfigState& state,
                                std::ostringstream& stream) {
  stream << "decoded.project_config.editor_tab_size: " << state.editor_tab_size << '\n';
  stream << "decoded.project_config.editor_indent_width: " << state.editor_indent_width << '\n';
  stream << "decoded.project_config.editor_soft_tabs: " << (state.editor_soft_tabs ? "true" : "false")
         << '\n';
  stream << "decoded.project_config.colorscheme_name: " << state.colorscheme_name << '\n';
  stream << "decoded.project_config.settings_count: " << state.settings.size() << '\n';
  stream << "decoded.project_config.sidebar_policies_count: " << state.sidebar_policies.size() << '\n';
}

void AppendWorkspaceSessionSummary(const workspace::PersistedWorkspaceSessionState& state,
                                   std::ostringstream& stream) {
  stream << "decoded.workspace_session.active_project_index: " << state.active_project_index << '\n';
  stream << "decoded.workspace_session.project_count: " << state.project_roots.size() << '\n';
}

void AppendConversationRegistrySummary(const workspace::PersistedChatState& state,
                                       std::ostringstream& stream) {
  stream << "decoded.conversation_registry.active_conversation_id: " << state.active_conversation_id
         << '\n';
  stream << "decoded.conversation_registry.conversation_count: " << state.conversations.size() << '\n';
}

void AppendDecodedSummary(std::uint32_t capability_flags,
                          std::span<const std::byte> body,
                          std::ostringstream& stream) {
  if (capability_flags == 1u) {
    workspace::PersistedUserConfigState state;
    if (workspace::DecodeUserConfigRecord(body, &state)) {
      AppendUserConfigSummary(state, stream);
    } else {
      stream << "decoded.error: failed_to_decode_user_config\n";
    }
    return;
  }
  if (capability_flags == 2u) {
    workspace::PersistedProjectConfigState state;
    if (workspace::DecodeProjectConfigRecord(body, &state)) {
      AppendProjectConfigSummary(state, stream);
    } else {
      stream << "decoded.error: failed_to_decode_project_config\n";
    }
    return;
  }
  if (capability_flags == 3u) {
    workspace::PersistedProjectSessionState state;
    if (workspace::DecodeProjectSessionRecord(body, &state)) {
      AppendProjectSessionSummary(state, stream);
    } else {
      stream << "decoded.error: failed_to_decode_project_session\n";
    }
    return;
  }
  if (capability_flags == 4u) {
    workspace::PersistedWorkspaceSessionState state;
    if (workspace::DecodeWorkspaceSessionRecord(body, &state)) {
      AppendWorkspaceSessionSummary(state, stream);
    } else {
      stream << "decoded.error: failed_to_decode_workspace_session\n";
    }
    return;
  }
  if (capability_flags == 5u) {
    workspace::PersistedChatState state;
    if (workspace::DecodeConversationRegistryRecord(body, &state)) {
      AppendConversationRegistrySummary(state, stream);
    } else {
      stream << "decoded.error: failed_to_decode_conversation_registry\n";
    }
    return;
  }
  stream << "decoded.info: unsupported_capability_flags\n";
}

}  // namespace

bool DumpPersistedRecordFile(const std::filesystem::path& path,
                             std::string* output,
                             std::string* error) {
  if (output == nullptr) {
    if (error != nullptr) {
      *error = "invalid_output_buffer";
    }
    return false;
  }
  output->clear();
  if (error != nullptr) {
    error->clear();
  }

  PersistedRecordReaderError read_error = PersistedRecordReaderError::None;
  const std::optional<PersistedRecordReadResult> result =
      PersistedRecordReader::ReadFile(path, &read_error);
  if (!result.has_value()) {
    if (error != nullptr) {
      *error = ReaderErrorToString(read_error);
    }
    return false;
  }

  std::ostringstream stream;
  stream << "path: " << path.lexically_normal().string() << '\n';
  stream << "version: " << result->header.version << '\n';
  stream << "capability_flags: " << result->header.capability_flags << '\n';
  stream << "crc32c: 0x" << std::hex << std::setw(8) << std::setfill('0') << result->header.crc32c
         << std::dec << '\n';
  stream << "source: " << (result->used_backup ? "backup" : "primary") << '\n';

  std::size_t offset = 0;
  std::size_t index = 0;
  while (offset < result->body.size()) {
    TaggedRecordView record;
    if (!ReadTaggedRecord(result->body, &offset, &record)) {
      if (error != nullptr) {
        *error = "malformed_record_stream";
      }
      return false;
    }
    stream << "record[" << index << "].tag: " << record.tag << '\n';
    stream << "record[" << index << "].length: " << record.payload.size() << '\n';
    ++index;
  }

  AppendDecodedSummary(result->header.capability_flags, result->body, stream);
  *output = stream.str();
  return true;
}

}  // namespace microide::persistence
