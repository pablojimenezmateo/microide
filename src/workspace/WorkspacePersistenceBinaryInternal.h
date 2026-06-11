#pragma once

#include "workspace/WorkspacePersistenceFormat.h"

#include "persistence/PersistedRecord.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace microide::workspace {
namespace {

using persistence::AppendTaggedRecord;
using persistence::PrimitiveReader;
using persistence::PrimitiveWriter;
using persistence::ReadTaggedRecord;
using persistence::TaggedRecordView;

constexpr std::uint32_t kSchemaVersion = 1;

enum class UserConfigTag : std::uint16_t {
  Schema = 1,
  UiScale = 2,
  Setting = 3,
  DisabledKeybinding = 4,
};

enum class ProjectConfigTag : std::uint16_t {
  Schema = 1,
  EditorTabSize = 2,
  EditorIndentWidth = 3,
  EditorSoftTabs = 4,
  ColorschemeName = 5,
  ProjectBaseColor = 6,
  Setting = 7,
  SidebarPolicy = 8,
  CommitDraft = 9,
  BranchReviewState = 10,
};

enum class BranchReviewTargetTag : std::uint16_t {
  RepositoryRoot = 1,
  BaseCommit = 2,
  HeadCommit = 3,
  MergeBaseCommit = 4,
  SnapshotGeneration = 5,
  LastAccessedUnixMs = 6,
  ReviewedFile = 7,
  ReviewedHunk = 8,
  Note = 9,
};

enum class BranchReviewHunkIdentityTag : std::uint16_t {
  Path = 1,
  OldStart = 2,
  OldCount = 3,
  NewStart = 4,
  NewCount = 5,
  ContentHash = 6,
};

enum class BranchReviewFileEntryTag : std::uint16_t {
  Path = 1,
  ReviewedSnapshotGeneration = 2,
  ReviewedAtUnixMs = 3,
};

enum class BranchReviewHunkEntryTag : std::uint16_t {
  Identity = 1,
  ReviewedAtUnixMs = 2,
};

enum class BranchReviewNoteTag : std::uint16_t {
  Scope = 1,
  Path = 2,
  HunkIdentity = 3,
  Text = 4,
  UpdatedAtUnixMs = 5,
};

enum class BranchReviewStateTag : std::uint16_t {
  Target = 1,
};

enum class CommitDraftTag : std::uint16_t {
  HeadOid = 1,
  BranchName = 2,
  Subject = 3,
  Body = 4,
};

enum class ConversationRegistryTag : std::uint16_t {
  Schema = 1,
  ActiveConversationId = 2,
  Conversation = 3,
};

enum class ConversationTag : std::uint16_t {
  SchemaVersion = 1,
  Id = 2,
  Title = 3,
  ProviderId = 4,
  ModelId = 5,
  Status = 6,
  ToolMode = 7,
  Draft = 8,
  SystemPrompt = 9,
  CreatedAt = 10,
  UpdatedAt = 11,
  LastRequestDurationMs = 12,
  Message = 13,
};

enum class MessageTag : std::uint16_t {
  Id = 1,
  Role = 2,
  Content = 3,
  Timestamp = 4,
  ProviderId = 5,
  Model = 6,
  Status = 7,
  RequestDurationMs = 8,
  Error = 9,
  ToolEvent = 10,
};

enum class ToolEventTag : std::uint16_t {
  CallId = 1,
  ToolId = 2,
  DisplayName = 3,
  ArgumentsSummary = 4,
  Status = 5,
  PermissionDecision = 6,
  CapabilityScope = 7,
  StartedAt = 8,
  FinishedAt = 9,
  DurationMs = 10,
  Error = 11,
  OutputSummary = 12,
};

enum class ProjectSessionTag : std::uint16_t {
  Schema = 1,
  SidebarVisible = 2,
  SidebarWidth = 3,
  BottomPanelHeight = 4,
  ActiveTabIndex = 5,
  Tab = 6,
  ChatRegistry = 7,
  OutgoingBaseKind = 8,
  OutgoingBaseCustomRef = 9,
};

enum class EditorTabTag : std::uint16_t {
  Kind = 1,
  ActiveLeafId = 2,
  View = 3,
  SplitNode = 4,
  ComparePath = 5,
  CompareLeftPath = 6,
  CompareRightPath = 7,
  CompareCommitHash = 8,
  CompareCommitShortHash = 9,
  CompareRightRef = 10,
  CompareRightLabel = 11,
  CompareSelectedRow = 12,
  CompareScrollRow = 13,
  CompareHorizontalScroll = 14,
  CompareDividerFraction = 25,
  MergeBasePath = 15,
  MergeIncomingPath = 16,
  MergeCurrentPath = 17,
  MergeOutputPath = 18,
  MergeSelectedHunk = 19,
  MergeScrollRow = 20,
  MergeHorizontalScroll = 21,
  MergeLeftDividerFraction = 22,
  MergeRightDividerFraction = 23,
  MergeHunkChoice = 24,
};

enum class EditorViewTag : std::uint16_t {
  LeafId = 1,
  Path = 2,
  CursorLine = 3,
  CursorColumn = 4,
  ScrollLine = 5,
  HorizontalScroll = 6,
  DirtySnapshot = 7,
  LineEnding = 8,
  BufferLine = 9,
};

enum class WorkspaceSessionTag : std::uint16_t {
  Schema = 1,
  ProjectRoot = 2,
  ActiveProjectIndex = 3,
};

template <typename EnumTag, typename WritePayload>
bool AppendRecord(EnumTag tag, WritePayload write_payload, std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  std::vector<std::byte> payload;
  PrimitiveWriter writer(&payload);
  if (!write_payload(writer)) {
    return false;
  }
  return AppendTaggedRecord(static_cast<std::uint16_t>(tag), payload, out);
}

bool WriteSize(PrimitiveWriter& writer, std::size_t value) {
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  return writer.WriteU32(static_cast<std::uint32_t>(value));
}

bool ReadSize(PrimitiveReader& reader, std::size_t* value) {
  if (value == nullptr) {
    return false;
  }
  std::uint32_t parsed = 0;
  if (!reader.ReadU32(&parsed)) {
    return false;
  }
  *value = parsed;
  return true;
}

template <typename EnumTag>
bool ParseRecordStream(std::span<const std::byte> input,
                       const std::function<bool(EnumTag, std::span<const std::byte>)>& on_record) {
  std::size_t offset = 0;
  while (offset < input.size()) {
    TaggedRecordView record;
    if (!ReadTaggedRecord(input, &offset, &record)) {
      return false;
    }
    if (!on_record(static_cast<EnumTag>(record.tag), record.payload)) {
      return false;
    }
  }
  return true;
}

template <typename EnumTag>
bool ReadStringRecord(EnumTag expected_tag,
                      EnumTag actual_tag,
                      std::span<const std::byte> payload,
                      std::string* out) {
  if (actual_tag != expected_tag || out == nullptr) {
    return false;
  }
  PrimitiveReader reader(payload);
  return reader.ReadString(out) && reader.remaining() == 0;
}

bool EncodeToolEvent(const PersistedMessageState::PersistedToolEventState& tool,
                     std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  return AppendRecord(ToolEventTag::CallId, [&](PrimitiveWriter& w) { return w.WriteString(tool.call_id); },
                      out) &&
         AppendRecord(ToolEventTag::ToolId, [&](PrimitiveWriter& w) { return w.WriteString(tool.tool_id); },
                      out) &&
         AppendRecord(ToolEventTag::DisplayName,
                      [&](PrimitiveWriter& w) { return w.WriteString(tool.display_name); }, out) &&
         AppendRecord(ToolEventTag::ArgumentsSummary,
                      [&](PrimitiveWriter& w) { return w.WriteString(tool.arguments_summary); },
                      out) &&
         AppendRecord(ToolEventTag::Status, [&](PrimitiveWriter& w) { return w.WriteString(tool.status); },
                      out) &&
         AppendRecord(ToolEventTag::PermissionDecision,
                      [&](PrimitiveWriter& w) { return w.WriteString(tool.permission_decision); },
                      out) &&
         AppendRecord(ToolEventTag::CapabilityScope,
                      [&](PrimitiveWriter& w) { return w.WriteString(tool.capability_scope); },
                      out) &&
         AppendRecord(ToolEventTag::StartedAt,
                      [&](PrimitiveWriter& w) { return w.WriteString(tool.started_at); }, out) &&
         AppendRecord(ToolEventTag::FinishedAt,
                      [&](PrimitiveWriter& w) { return w.WriteString(tool.finished_at); }, out) &&
         AppendRecord(ToolEventTag::DurationMs,
                      [&](PrimitiveWriter& w) { return w.WriteI64(tool.duration_ms); }, out) &&
         AppendRecord(ToolEventTag::Error, [&](PrimitiveWriter& w) { return w.WriteString(tool.error); },
                      out) &&
         AppendRecord(ToolEventTag::OutputSummary,
                      [&](PrimitiveWriter& w) { return w.WriteString(tool.output_summary); }, out);
}

bool DecodeToolEvent(std::span<const std::byte> input,
                     PersistedMessageState::PersistedToolEventState* tool) {
  if (tool == nullptr) {
    return false;
  }
  *tool = PersistedMessageState::PersistedToolEventState{};
  return ParseRecordStream<ToolEventTag>(
      input, [&](ToolEventTag tag, std::span<const std::byte> payload) {
        PrimitiveReader reader(payload);
        switch (tag) {
          case ToolEventTag::CallId: return reader.ReadString(&tool->call_id) && reader.remaining() == 0;
          case ToolEventTag::ToolId: return reader.ReadString(&tool->tool_id) && reader.remaining() == 0;
          case ToolEventTag::DisplayName:
            return reader.ReadString(&tool->display_name) && reader.remaining() == 0;
          case ToolEventTag::ArgumentsSummary:
            return reader.ReadString(&tool->arguments_summary) && reader.remaining() == 0;
          case ToolEventTag::Status: return reader.ReadString(&tool->status) && reader.remaining() == 0;
          case ToolEventTag::PermissionDecision:
            return reader.ReadString(&tool->permission_decision) && reader.remaining() == 0;
          case ToolEventTag::CapabilityScope:
            return reader.ReadString(&tool->capability_scope) && reader.remaining() == 0;
          case ToolEventTag::StartedAt:
            return reader.ReadString(&tool->started_at) && reader.remaining() == 0;
          case ToolEventTag::FinishedAt:
            return reader.ReadString(&tool->finished_at) && reader.remaining() == 0;
          case ToolEventTag::DurationMs: return reader.ReadI64(&tool->duration_ms) && reader.remaining() == 0;
          case ToolEventTag::Error: return reader.ReadString(&tool->error) && reader.remaining() == 0;
          case ToolEventTag::OutputSummary:
            return reader.ReadString(&tool->output_summary) && reader.remaining() == 0;
        }
        return true;
      });
}

bool EncodeMessage(const PersistedMessageState& message, std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  if (!AppendRecord(MessageTag::Id, [&](PrimitiveWriter& w) { return w.WriteString(message.id); }, out) ||
      !AppendRecord(MessageTag::Role,
                    [&](PrimitiveWriter& w) { return w.WriteString(message.role); }, out) ||
      !AppendRecord(MessageTag::Content,
                    [&](PrimitiveWriter& w) { return w.WriteString(message.content); }, out) ||
      !AppendRecord(MessageTag::Timestamp,
                    [&](PrimitiveWriter& w) { return w.WriteString(message.timestamp); }, out) ||
      !AppendRecord(MessageTag::ProviderId,
                    [&](PrimitiveWriter& w) { return w.WriteString(message.provider_id); }, out) ||
      !AppendRecord(MessageTag::Model,
                    [&](PrimitiveWriter& w) { return w.WriteString(message.model); }, out) ||
      !AppendRecord(MessageTag::Status,
                    [&](PrimitiveWriter& w) { return w.WriteString(message.status); }, out) ||
      !AppendRecord(MessageTag::RequestDurationMs,
                    [&](PrimitiveWriter& w) { return w.WriteI64(message.request_duration_ms); },
                    out) ||
      !AppendRecord(MessageTag::Error,
                    [&](PrimitiveWriter& w) { return w.WriteString(message.error); }, out)) {
    return false;
  }

  for (const auto& tool : message.tool_events) {
    std::vector<std::byte> payload;
    if (!EncodeToolEvent(tool, &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(MessageTag::ToolEvent), payload, out)) {
      return false;
    }
  }
  return true;
}

bool DecodeMessage(std::span<const std::byte> input, PersistedMessageState* message) {
  if (message == nullptr) {
    return false;
  }
  *message = PersistedMessageState{};
  return ParseRecordStream<MessageTag>(
      input, [&](MessageTag tag, std::span<const std::byte> payload) {
        PrimitiveReader reader(payload);
        switch (tag) {
          case MessageTag::Id: return reader.ReadString(&message->id) && reader.remaining() == 0;
          case MessageTag::Role: return reader.ReadString(&message->role) && reader.remaining() == 0;
          case MessageTag::Content:
            return reader.ReadString(&message->content) && reader.remaining() == 0;
          case MessageTag::Timestamp:
            return reader.ReadString(&message->timestamp) && reader.remaining() == 0;
          case MessageTag::ProviderId:
            return reader.ReadString(&message->provider_id) && reader.remaining() == 0;
          case MessageTag::Model: return reader.ReadString(&message->model) && reader.remaining() == 0;
          case MessageTag::Status: return reader.ReadString(&message->status) && reader.remaining() == 0;
          case MessageTag::RequestDurationMs:
            return reader.ReadI64(&message->request_duration_ms) && reader.remaining() == 0;
          case MessageTag::Error: return reader.ReadString(&message->error) && reader.remaining() == 0;
          case MessageTag::ToolEvent: {
            PersistedMessageState::PersistedToolEventState tool;
            if (!DecodeToolEvent(payload, &tool)) {
              return false;
            }
            message->tool_events.push_back(std::move(tool));
            return true;
          }
        }
        return true;
      });
}

[[maybe_unused]] bool EncodeConversation(const PersistedConversationState& conversation,
                                         std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  if (!AppendRecord(ConversationTag::SchemaVersion,
                    [&](PrimitiveWriter& w) { return w.WriteI32(conversation.schema_version); },
                    out) ||
      !AppendRecord(ConversationTag::Id,
                    [&](PrimitiveWriter& w) { return w.WriteString(conversation.id); }, out) ||
      !AppendRecord(ConversationTag::Title,
                    [&](PrimitiveWriter& w) { return w.WriteString(conversation.title); }, out) ||
      !AppendRecord(ConversationTag::ProviderId,
                    [&](PrimitiveWriter& w) { return w.WriteString(conversation.provider_id); },
                    out) ||
      !AppendRecord(ConversationTag::ModelId,
                    [&](PrimitiveWriter& w) { return w.WriteString(conversation.model_id); }, out) ||
      !AppendRecord(ConversationTag::Status,
                    [&](PrimitiveWriter& w) { return w.WriteString(conversation.status); }, out) ||
      !AppendRecord(ConversationTag::ToolMode,
                    [&](PrimitiveWriter& w) { return w.WriteString(conversation.tool_mode); }, out) ||
      !AppendRecord(ConversationTag::Draft,
                    [&](PrimitiveWriter& w) { return w.WriteString(conversation.draft); }, out) ||
      !AppendRecord(ConversationTag::SystemPrompt,
                    [&](PrimitiveWriter& w) { return w.WriteString(conversation.system_prompt); },
                    out) ||
      !AppendRecord(ConversationTag::CreatedAt,
                    [&](PrimitiveWriter& w) { return w.WriteString(conversation.created_at); }, out) ||
      !AppendRecord(ConversationTag::UpdatedAt,
                    [&](PrimitiveWriter& w) { return w.WriteString(conversation.updated_at); }, out) ||
      !AppendRecord(ConversationTag::LastRequestDurationMs,
                    [&](PrimitiveWriter& w) { return w.WriteI64(conversation.last_request_duration_ms); },
                    out)) {
    return false;
  }

  for (const auto& message : conversation.messages) {
    std::vector<std::byte> payload;
    if (!EncodeMessage(message, &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(ConversationTag::Message), payload, out)) {
      return false;
    }
  }
  return true;
}

[[maybe_unused]] bool DecodeConversation(std::span<const std::byte> input,
                                         PersistedConversationState* conversation) {
  if (conversation == nullptr) {
    return false;
  }
  *conversation = PersistedConversationState{};
  return ParseRecordStream<ConversationTag>(
      input, [&](ConversationTag tag, std::span<const std::byte> payload) {
        PrimitiveReader reader(payload);
        switch (tag) {
          case ConversationTag::SchemaVersion:
            return reader.ReadI32(&conversation->schema_version) && reader.remaining() == 0;
          case ConversationTag::Id:
            return reader.ReadString(&conversation->id) && reader.remaining() == 0;
          case ConversationTag::Title:
            return reader.ReadString(&conversation->title) && reader.remaining() == 0;
          case ConversationTag::ProviderId:
            return reader.ReadString(&conversation->provider_id) && reader.remaining() == 0;
          case ConversationTag::ModelId:
            return reader.ReadString(&conversation->model_id) && reader.remaining() == 0;
          case ConversationTag::Status:
            return reader.ReadString(&conversation->status) && reader.remaining() == 0;
          case ConversationTag::ToolMode:
            return reader.ReadString(&conversation->tool_mode) && reader.remaining() == 0;
          case ConversationTag::Draft:
            return reader.ReadString(&conversation->draft) && reader.remaining() == 0;
          case ConversationTag::SystemPrompt:
            return reader.ReadString(&conversation->system_prompt) && reader.remaining() == 0;
          case ConversationTag::CreatedAt:
            return reader.ReadString(&conversation->created_at) && reader.remaining() == 0;
          case ConversationTag::UpdatedAt:
            return reader.ReadString(&conversation->updated_at) && reader.remaining() == 0;
          case ConversationTag::LastRequestDurationMs:
            return reader.ReadI64(&conversation->last_request_duration_ms) && reader.remaining() == 0;
          case ConversationTag::Message: {
            PersistedMessageState message;
            if (!DecodeMessage(payload, &message)) {
              return false;
            }
            conversation->messages.push_back(std::move(message));
            return true;
          }
        }
        return true;
      });
}

bool EncodeEditorView(const PersistedEditorViewState& view, std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  if (!AppendRecord(EditorViewTag::LeafId, [&](PrimitiveWriter& w) { return WriteSize(w, view.leaf_id); },
                    out) ||
      !AppendRecord(EditorViewTag::Path, [&](PrimitiveWriter& w) { return w.WritePath(view.path); }, out) ||
      !AppendRecord(EditorViewTag::CursorLine,
                    [&](PrimitiveWriter& w) { return WriteSize(w, view.cursor_line); }, out) ||
      !AppendRecord(EditorViewTag::CursorColumn,
                    [&](PrimitiveWriter& w) { return WriteSize(w, view.cursor_column); }, out) ||
      !AppendRecord(EditorViewTag::ScrollLine,
                    [&](PrimitiveWriter& w) { return WriteSize(w, view.scroll_line); }, out) ||
      !AppendRecord(EditorViewTag::HorizontalScroll,
                    [&](PrimitiveWriter& w) { return WriteSize(w, view.horizontal_scroll); }, out) ||
      !AppendRecord(EditorViewTag::DirtySnapshot,
                    [&](PrimitiveWriter& w) { return w.WriteBool(view.dirty_snapshot); }, out) ||
      !AppendRecord(EditorViewTag::LineEnding,
                    [&](PrimitiveWriter& w) {
                      return w.WriteU8(static_cast<std::uint8_t>(view.line_ending));
                    },
                    out)) {
    return false;
  }
  for (const std::string& line : view.buffer_lines) {
    if (!AppendRecord(EditorViewTag::BufferLine,
                      [&](PrimitiveWriter& w) { return w.WriteString(line); }, out)) {
      return false;
    }
  }
  return true;
}

bool DecodeEditorView(std::span<const std::byte> input, PersistedEditorViewState* view) {
  if (view == nullptr) {
    return false;
  }
  *view = PersistedEditorViewState{};
  return ParseRecordStream<EditorViewTag>(
      input, [&](EditorViewTag tag, std::span<const std::byte> payload) {
        PrimitiveReader reader(payload);
        switch (tag) {
          case EditorViewTag::LeafId: return ReadSize(reader, &view->leaf_id) && reader.remaining() == 0;
          case EditorViewTag::Path: return reader.ReadPath(&view->path) && reader.remaining() == 0;
          case EditorViewTag::CursorLine:
            return ReadSize(reader, &view->cursor_line) && reader.remaining() == 0;
          case EditorViewTag::CursorColumn:
            return ReadSize(reader, &view->cursor_column) && reader.remaining() == 0;
          case EditorViewTag::ScrollLine:
            return ReadSize(reader, &view->scroll_line) && reader.remaining() == 0;
          case EditorViewTag::HorizontalScroll:
            return ReadSize(reader, &view->horizontal_scroll) && reader.remaining() == 0;
          case EditorViewTag::DirtySnapshot:
            return reader.ReadBool(&view->dirty_snapshot) && reader.remaining() == 0;
          case EditorViewTag::LineEnding: {
            std::uint8_t line_ending = 0;
            if (!reader.ReadU8(&line_ending) || reader.remaining() != 0 || line_ending > 2) {
              return false;
            }
            view->line_ending = static_cast<util::LineEnding>(line_ending);
            return true;
          }
          case EditorViewTag::BufferLine: {
            std::string line;
            if (!reader.ReadString(&line) || reader.remaining() != 0) {
              return false;
            }
            view->buffer_lines.push_back(std::move(line));
            return true;
          }
        }
        return true;
      });
}

bool EncodeSplitNode(const PersistedSplitNodeState& node, std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  PrimitiveWriter writer(out);
  if (!WriteSize(writer, node.path.size())) {
    return false;
  }
  for (std::size_t index : node.path) {
    if (!WriteSize(writer, index)) {
      return false;
    }
  }
  return writer.WriteString(node.orientation) && writer.WriteF32(node.size_fraction) &&
         WriteSize(writer, node.leaf_id);
}

bool DecodeSplitNode(std::span<const std::byte> input, PersistedSplitNodeState* node) {
  if (node == nullptr) {
    return false;
  }
  *node = PersistedSplitNodeState{};
  PrimitiveReader reader(input);
  std::size_t count = 0;
  if (!ReadSize(reader, &count)) {
    return false;
  }
  // Bound the reservation by the bytes actually available: each path element is a
  // ReadSize (4-byte u32), so a corrupt/adversarial `count` cannot force an
  // allocation larger than the remaining input. Prevents OOM on malformed state.
  node->path.reserve(std::min<std::size_t>(count, reader.remaining() / sizeof(std::uint32_t)));
  for (std::size_t i = 0; i < count; ++i) {
    std::size_t value = 0;
    if (!ReadSize(reader, &value)) {
      return false;
    }
    node->path.push_back(value);
  }
  return reader.ReadString(&node->orientation) && reader.ReadF32(&node->size_fraction) &&
         ReadSize(reader, &node->leaf_id) && reader.remaining() == 0;
}

[[maybe_unused]] bool EncodeEditorTab(const PersistedEditorTabState& tab, std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  if (!AppendRecord(EditorTabTag::Kind, [&](PrimitiveWriter& w) { return w.WriteString(tab.kind); }, out) ||
      !AppendRecord(EditorTabTag::ActiveLeafId,
                    [&](PrimitiveWriter& w) { return WriteSize(w, tab.active_leaf_id); }, out) ||
      !AppendRecord(EditorTabTag::ComparePath,
                    [&](PrimitiveWriter& w) { return w.WritePath(tab.compare_path); }, out) ||
      !AppendRecord(EditorTabTag::CompareLeftPath,
                    [&](PrimitiveWriter& w) { return w.WritePath(tab.compare_left_path); }, out) ||
      !AppendRecord(EditorTabTag::CompareRightPath,
                    [&](PrimitiveWriter& w) { return w.WritePath(tab.compare_right_path); }, out) ||
      !AppendRecord(EditorTabTag::CompareCommitHash,
                    [&](PrimitiveWriter& w) { return w.WriteString(tab.compare_commit_hash); }, out) ||
      !AppendRecord(EditorTabTag::CompareCommitShortHash,
                    [&](PrimitiveWriter& w) { return w.WriteString(tab.compare_commit_short_hash); },
                    out) ||
      !AppendRecord(EditorTabTag::CompareRightRef,
                    [&](PrimitiveWriter& w) { return w.WriteString(tab.compare_right_ref); }, out) ||
      !AppendRecord(EditorTabTag::CompareRightLabel,
                    [&](PrimitiveWriter& w) { return w.WriteString(tab.compare_right_label); }, out) ||
      !AppendRecord(EditorTabTag::CompareSelectedRow,
                    [&](PrimitiveWriter& w) { return WriteSize(w, tab.compare_selected_row); }, out) ||
      !AppendRecord(EditorTabTag::CompareScrollRow,
                    [&](PrimitiveWriter& w) { return WriteSize(w, tab.compare_scroll_row); }, out) ||
      !AppendRecord(EditorTabTag::CompareHorizontalScroll,
                    [&](PrimitiveWriter& w) { return WriteSize(w, tab.compare_horizontal_scroll); },
                    out) ||
      !AppendRecord(EditorTabTag::CompareDividerFraction,
                    [&](PrimitiveWriter& w) { return w.WriteF32(tab.compare_divider_fraction); },
                    out) ||
      !AppendRecord(EditorTabTag::MergeBasePath,
                    [&](PrimitiveWriter& w) { return w.WritePath(tab.merge_base_path); }, out) ||
      !AppendRecord(EditorTabTag::MergeIncomingPath,
                    [&](PrimitiveWriter& w) { return w.WritePath(tab.merge_incoming_path); }, out) ||
      !AppendRecord(EditorTabTag::MergeCurrentPath,
                    [&](PrimitiveWriter& w) { return w.WritePath(tab.merge_current_path); }, out) ||
      !AppendRecord(EditorTabTag::MergeOutputPath,
                    [&](PrimitiveWriter& w) { return w.WritePath(tab.merge_output_path); }, out) ||
      !AppendRecord(EditorTabTag::MergeSelectedHunk,
                    [&](PrimitiveWriter& w) { return WriteSize(w, tab.merge_selected_hunk); }, out) ||
      !AppendRecord(EditorTabTag::MergeScrollRow,
                    [&](PrimitiveWriter& w) { return WriteSize(w, tab.merge_scroll_row); }, out) ||
      !AppendRecord(EditorTabTag::MergeHorizontalScroll,
                    [&](PrimitiveWriter& w) { return WriteSize(w, tab.merge_horizontal_scroll); }, out) ||
      !AppendRecord(EditorTabTag::MergeLeftDividerFraction,
                    [&](PrimitiveWriter& w) { return w.WriteF32(tab.merge_left_divider_fraction); },
                    out) ||
      !AppendRecord(EditorTabTag::MergeRightDividerFraction,
                    [&](PrimitiveWriter& w) { return w.WriteF32(tab.merge_right_divider_fraction); },
                    out)) {
    return false;
  }

  for (const auto& view : tab.views) {
    std::vector<std::byte> payload;
    if (!EncodeEditorView(view, &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(EditorTabTag::View), payload, out)) {
      return false;
    }
  }
  for (const auto& node : tab.split_nodes) {
    std::vector<std::byte> payload;
    if (!EncodeSplitNode(node, &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(EditorTabTag::SplitNode), payload, out)) {
      return false;
    }
  }
  for (const std::string& choice : tab.merge_hunk_choices) {
    if (!AppendRecord(EditorTabTag::MergeHunkChoice,
                      [&](PrimitiveWriter& w) { return w.WriteString(choice); }, out)) {
      return false;
    }
  }
  return true;
}

[[maybe_unused]] bool DecodeEditorTab(std::span<const std::byte> input, PersistedEditorTabState* tab) {
  if (tab == nullptr) {
    return false;
  }
  *tab = PersistedEditorTabState{};
  return ParseRecordStream<EditorTabTag>(
      input, [&](EditorTabTag tag, std::span<const std::byte> payload) {
        PrimitiveReader reader(payload);
        switch (tag) {
          case EditorTabTag::Kind: return reader.ReadString(&tab->kind) && reader.remaining() == 0;
          case EditorTabTag::ActiveLeafId:
            return ReadSize(reader, &tab->active_leaf_id) && reader.remaining() == 0;
          case EditorTabTag::ComparePath:
            return reader.ReadPath(&tab->compare_path) && reader.remaining() == 0;
          case EditorTabTag::CompareLeftPath:
            return reader.ReadPath(&tab->compare_left_path) && reader.remaining() == 0;
          case EditorTabTag::CompareRightPath:
            return reader.ReadPath(&tab->compare_right_path) && reader.remaining() == 0;
          case EditorTabTag::CompareCommitHash:
            return reader.ReadString(&tab->compare_commit_hash) && reader.remaining() == 0;
          case EditorTabTag::CompareCommitShortHash:
            return reader.ReadString(&tab->compare_commit_short_hash) && reader.remaining() == 0;
          case EditorTabTag::CompareRightRef:
            return reader.ReadString(&tab->compare_right_ref) && reader.remaining() == 0;
          case EditorTabTag::CompareRightLabel:
            return reader.ReadString(&tab->compare_right_label) && reader.remaining() == 0;
          case EditorTabTag::CompareSelectedRow:
            return ReadSize(reader, &tab->compare_selected_row) && reader.remaining() == 0;
          case EditorTabTag::CompareScrollRow:
            return ReadSize(reader, &tab->compare_scroll_row) && reader.remaining() == 0;
          case EditorTabTag::CompareHorizontalScroll:
            return ReadSize(reader, &tab->compare_horizontal_scroll) && reader.remaining() == 0;
          case EditorTabTag::CompareDividerFraction:
            return reader.ReadF32(&tab->compare_divider_fraction) && reader.remaining() == 0;
          case EditorTabTag::MergeBasePath:
            return reader.ReadPath(&tab->merge_base_path) && reader.remaining() == 0;
          case EditorTabTag::MergeIncomingPath:
            return reader.ReadPath(&tab->merge_incoming_path) && reader.remaining() == 0;
          case EditorTabTag::MergeCurrentPath:
            return reader.ReadPath(&tab->merge_current_path) && reader.remaining() == 0;
          case EditorTabTag::MergeOutputPath:
            return reader.ReadPath(&tab->merge_output_path) && reader.remaining() == 0;
          case EditorTabTag::MergeSelectedHunk:
            return ReadSize(reader, &tab->merge_selected_hunk) && reader.remaining() == 0;
          case EditorTabTag::MergeScrollRow:
            return ReadSize(reader, &tab->merge_scroll_row) && reader.remaining() == 0;
          case EditorTabTag::MergeHorizontalScroll:
            return ReadSize(reader, &tab->merge_horizontal_scroll) && reader.remaining() == 0;
          case EditorTabTag::MergeLeftDividerFraction:
            return reader.ReadF32(&tab->merge_left_divider_fraction) && reader.remaining() == 0;
          case EditorTabTag::MergeRightDividerFraction:
            return reader.ReadF32(&tab->merge_right_divider_fraction) && reader.remaining() == 0;
          case EditorTabTag::View: {
            PersistedEditorViewState view;
            if (!DecodeEditorView(payload, &view)) {
              return false;
            }
            tab->views.push_back(std::move(view));
            return true;
          }
          case EditorTabTag::SplitNode: {
            PersistedSplitNodeState node;
            if (!DecodeSplitNode(payload, &node)) {
              return false;
            }
            tab->split_nodes.push_back(std::move(node));
            return true;
          }
          case EditorTabTag::MergeHunkChoice: {
            std::string choice;
            if (!reader.ReadString(&choice) || reader.remaining() != 0) {
              return false;
            }
            tab->merge_hunk_choices.push_back(std::move(choice));
            return true;
          }
        }
        return true;
      });
}

[[maybe_unused]] bool EncodeSidebarPolicy(const PersistedSidebarViewPolicy& policy,
                                          std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  PrimitiveWriter writer(out);
  return writer.WriteString(policy.view_id) && writer.WriteBool(policy.hidden) &&
         writer.WriteI32(policy.order);
}

[[maybe_unused]] bool DecodeSidebarPolicy(std::span<const std::byte> input,
                                          PersistedSidebarViewPolicy* policy) {
  if (policy == nullptr) {
    return false;
  }
  *policy = PersistedSidebarViewPolicy{};
  PrimitiveReader reader(input);
  return reader.ReadString(&policy->view_id) && reader.ReadBool(&policy->hidden) &&
         reader.ReadI32(&policy->order) && reader.remaining() == 0;
}

[[maybe_unused]] bool EncodeCommitDraft(const PersistedCommitDraftState& draft,
                                        std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  return AppendRecord(CommitDraftTag::HeadOid,
                        [&](PrimitiveWriter& w) { return w.WriteString(draft.head_oid); }, out) &&
         AppendRecord(CommitDraftTag::BranchName,
                      [&](PrimitiveWriter& w) { return w.WriteString(draft.branch_name); }, out) &&
         AppendRecord(CommitDraftTag::Subject,
                      [&](PrimitiveWriter& w) { return w.WriteString(draft.subject); }, out) &&
         AppendRecord(CommitDraftTag::Body,
                      [&](PrimitiveWriter& w) { return w.WriteString(draft.body); }, out);
}

[[maybe_unused]] bool EncodeBranchReviewHunkIdentity(const PersistedBranchReviewHunkIdentity& identity,
                                                    std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  return AppendRecord(BranchReviewHunkIdentityTag::Path,
                      [&](PrimitiveWriter& w) { return w.WritePath(identity.path); }, out) &&
         AppendRecord(BranchReviewHunkIdentityTag::OldStart,
                      [&](PrimitiveWriter& w) { return w.WriteI32(identity.old_start); }, out) &&
         AppendRecord(BranchReviewHunkIdentityTag::OldCount,
                      [&](PrimitiveWriter& w) { return w.WriteI32(identity.old_count); }, out) &&
         AppendRecord(BranchReviewHunkIdentityTag::NewStart,
                      [&](PrimitiveWriter& w) { return w.WriteI32(identity.new_start); }, out) &&
         AppendRecord(BranchReviewHunkIdentityTag::NewCount,
                      [&](PrimitiveWriter& w) { return w.WriteI32(identity.new_count); }, out) &&
         AppendRecord(BranchReviewHunkIdentityTag::ContentHash,
                      [&](PrimitiveWriter& w) { return w.WriteI64(identity.content_hash); }, out);
}

[[maybe_unused]] bool DecodeBranchReviewHunkIdentity(std::span<const std::byte> input,
                                                    PersistedBranchReviewHunkIdentity* identity) {
  if (identity == nullptr) {
    return false;
  }
  *identity = PersistedBranchReviewHunkIdentity{};
  return ParseRecordStream<BranchReviewHunkIdentityTag>(
      input, [&](BranchReviewHunkIdentityTag tag, std::span<const std::byte> payload) {
        PrimitiveReader reader(payload);
        switch (tag) {
          case BranchReviewHunkIdentityTag::Path:
            return reader.ReadPath(&identity->path) && reader.remaining() == 0;
          case BranchReviewHunkIdentityTag::OldStart:
            return reader.ReadI32(&identity->old_start) && reader.remaining() == 0;
          case BranchReviewHunkIdentityTag::OldCount:
            return reader.ReadI32(&identity->old_count) && reader.remaining() == 0;
          case BranchReviewHunkIdentityTag::NewStart:
            return reader.ReadI32(&identity->new_start) && reader.remaining() == 0;
          case BranchReviewHunkIdentityTag::NewCount:
            return reader.ReadI32(&identity->new_count) && reader.remaining() == 0;
          case BranchReviewHunkIdentityTag::ContentHash: {
            std::int64_t hash = 0;
            return reader.ReadI64(&hash) && reader.remaining() == 0 && hash >= 0 &&
                   (identity->content_hash = static_cast<std::uint64_t>(hash), true);
          }
        }
        return true;
      });
}

[[maybe_unused]] bool EncodeBranchReviewTarget(const PersistedBranchReviewTarget& target,
                                              std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  if (!AppendRecord(BranchReviewTargetTag::RepositoryRoot,
                    [&](PrimitiveWriter& w) { return w.WritePath(target.repository_root); }, out) ||
      !AppendRecord(BranchReviewTargetTag::BaseCommit,
                    [&](PrimitiveWriter& w) { return w.WriteString(target.base_commit); }, out) ||
      !AppendRecord(BranchReviewTargetTag::HeadCommit,
                    [&](PrimitiveWriter& w) { return w.WriteString(target.head_commit); }, out) ||
      !AppendRecord(BranchReviewTargetTag::MergeBaseCommit,
                    [&](PrimitiveWriter& w) { return w.WriteString(target.merge_base_commit); }, out) ||
      !AppendRecord(BranchReviewTargetTag::SnapshotGeneration,
                    [&](PrimitiveWriter& w) { return w.WriteI64(target.snapshot_generation); }, out) ||
      !AppendRecord(BranchReviewTargetTag::LastAccessedUnixMs,
                    [&](PrimitiveWriter& w) { return w.WriteI64(target.last_accessed_unix_ms); }, out)) {
    return false;
  }
  for (const PersistedBranchReviewFileEntry& file : target.reviewed_files) {
    std::vector<std::byte> payload;
    if (!AppendRecord(BranchReviewFileEntryTag::Path,
                      [&](PrimitiveWriter& w) { return w.WritePath(file.path); }, &payload) ||
        !AppendRecord(BranchReviewFileEntryTag::ReviewedSnapshotGeneration,
                      [&](PrimitiveWriter& w) {
                        return w.WriteI64(file.reviewed_snapshot_generation);
                      },
                      &payload) ||
        !AppendRecord(BranchReviewFileEntryTag::ReviewedAtUnixMs,
                      [&](PrimitiveWriter& w) { return w.WriteI64(file.reviewed_at_unix_ms); },
                      &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(BranchReviewTargetTag::ReviewedFile), payload,
                            out)) {
      return false;
    }
  }
  for (const PersistedBranchReviewHunkEntry& hunk : target.reviewed_hunks) {
    std::vector<std::byte> identity_payload;
    std::vector<std::byte> entry_payload;
    if (!EncodeBranchReviewHunkIdentity(hunk.identity, &identity_payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(BranchReviewHunkEntryTag::Identity),
                            identity_payload, &entry_payload) ||
        !AppendRecord(BranchReviewHunkEntryTag::ReviewedAtUnixMs,
                      [&](PrimitiveWriter& w) { return w.WriteI64(hunk.reviewed_at_unix_ms); },
                      &entry_payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(BranchReviewTargetTag::ReviewedHunk),
                            entry_payload, out)) {
      return false;
    }
  }
  for (const PersistedBranchReviewNote& note : target.notes) {
    std::vector<std::byte> payload;
    if (!AppendRecord(BranchReviewNoteTag::Scope,
                      [&](PrimitiveWriter& w) { return w.WriteString(note.scope); }, &payload) ||
        !AppendRecord(BranchReviewNoteTag::Path,
                      [&](PrimitiveWriter& w) { return w.WritePath(note.path); }, &payload)) {
      return false;
    }
    if (note.hunk_identity.has_value()) {
      std::vector<std::byte> identity_payload;
      if (!EncodeBranchReviewHunkIdentity(*note.hunk_identity, &identity_payload) ||
          !AppendTaggedRecord(static_cast<std::uint16_t>(BranchReviewNoteTag::HunkIdentity),
                              identity_payload, &payload)) {
        return false;
      }
    }
    if (!AppendRecord(BranchReviewNoteTag::Text,
                      [&](PrimitiveWriter& w) { return w.WriteString(note.text); }, &payload) ||
        !AppendRecord(BranchReviewNoteTag::UpdatedAtUnixMs,
                      [&](PrimitiveWriter& w) { return w.WriteI64(note.updated_at_unix_ms); },
                      &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(BranchReviewTargetTag::Note), payload, out)) {
      return false;
    }
  }
  return true;
}

[[maybe_unused]] bool DecodeBranchReviewTarget(std::span<const std::byte> input,
                                              PersistedBranchReviewTarget* target) {
  if (target == nullptr) {
    return false;
  }
  *target = PersistedBranchReviewTarget{};
  return ParseRecordStream<BranchReviewTargetTag>(
      input, [&](BranchReviewTargetTag tag, std::span<const std::byte> payload) {
        PrimitiveReader reader(payload);
        switch (tag) {
          case BranchReviewTargetTag::RepositoryRoot:
            return reader.ReadPath(&target->repository_root) && reader.remaining() == 0;
          case BranchReviewTargetTag::BaseCommit:
            return reader.ReadString(&target->base_commit) && reader.remaining() == 0;
          case BranchReviewTargetTag::HeadCommit:
            return reader.ReadString(&target->head_commit) && reader.remaining() == 0;
          case BranchReviewTargetTag::MergeBaseCommit:
            return reader.ReadString(&target->merge_base_commit) && reader.remaining() == 0;
          case BranchReviewTargetTag::SnapshotGeneration: {
            std::int64_t value = 0;
            return reader.ReadI64(&value) && reader.remaining() == 0 && value >= 0 &&
                   (target->snapshot_generation = static_cast<std::uint64_t>(value), true);
          }
          case BranchReviewTargetTag::LastAccessedUnixMs: {
            std::int64_t value = 0;
            return reader.ReadI64(&value) && reader.remaining() == 0 && value >= 0 &&
                   (target->last_accessed_unix_ms = static_cast<std::uint64_t>(value), true);
          }
          case BranchReviewTargetTag::ReviewedFile: {
            PersistedBranchReviewFileEntry file;
            return ParseRecordStream<BranchReviewFileEntryTag>(
                payload, [&](BranchReviewFileEntryTag file_tag,
                             std::span<const std::byte> file_payload) {
                  PrimitiveReader file_reader(file_payload);
                  switch (file_tag) {
                    case BranchReviewFileEntryTag::Path:
                      return file_reader.ReadPath(&file.path) && file_reader.remaining() == 0;
                    case BranchReviewFileEntryTag::ReviewedSnapshotGeneration: {
                      std::int64_t value = 0;
                      return file_reader.ReadI64(&value) && file_reader.remaining() == 0 &&
                             value >= 0 &&
                             (file.reviewed_snapshot_generation = static_cast<std::uint64_t>(value),
                              true);
                    }
                    case BranchReviewFileEntryTag::ReviewedAtUnixMs: {
                      std::int64_t value = 0;
                      return file_reader.ReadI64(&value) && file_reader.remaining() == 0 &&
                             value >= 0 &&
                             (file.reviewed_at_unix_ms = static_cast<std::uint64_t>(value), true);
                    }
                  }
                  return true;
                }) && (target->reviewed_files.push_back(std::move(file)), true);
          }
          case BranchReviewTargetTag::ReviewedHunk: {
            PersistedBranchReviewHunkEntry hunk;
            return ParseRecordStream<BranchReviewHunkEntryTag>(
                payload, [&](BranchReviewHunkEntryTag hunk_tag,
                             std::span<const std::byte> hunk_payload) {
                  switch (hunk_tag) {
                    case BranchReviewHunkEntryTag::Identity:
                      return DecodeBranchReviewHunkIdentity(hunk_payload, &hunk.identity);
                    case BranchReviewHunkEntryTag::ReviewedAtUnixMs: {
                      PrimitiveReader hunk_reader(hunk_payload);
                      std::int64_t value = 0;
                      return hunk_reader.ReadI64(&value) && hunk_reader.remaining() == 0 &&
                             value >= 0 &&
                             (hunk.reviewed_at_unix_ms = static_cast<std::uint64_t>(value), true);
                    }
                  }
                  return true;
                }) && (target->reviewed_hunks.push_back(std::move(hunk)), true);
          }
          case BranchReviewTargetTag::Note: {
            PersistedBranchReviewNote note;
            return ParseRecordStream<BranchReviewNoteTag>(
                payload, [&](BranchReviewNoteTag note_tag, std::span<const std::byte> note_payload) {
                  PrimitiveReader note_reader(note_payload);
                  switch (note_tag) {
                    case BranchReviewNoteTag::Scope:
                      return note_reader.ReadString(&note.scope) && note_reader.remaining() == 0;
                    case BranchReviewNoteTag::Path:
                      return note_reader.ReadPath(&note.path) && note_reader.remaining() == 0;
                    case BranchReviewNoteTag::HunkIdentity: {
                      PersistedBranchReviewHunkIdentity identity;
                      if (!DecodeBranchReviewHunkIdentity(note_payload, &identity)) {
                        return false;
                      }
                      note.hunk_identity = std::move(identity);
                      return true;
                    }
                    case BranchReviewNoteTag::Text:
                      return note_reader.ReadString(&note.text) && note_reader.remaining() == 0;
                    case BranchReviewNoteTag::UpdatedAtUnixMs: {
                      std::int64_t value = 0;
                      return note_reader.ReadI64(&value) && note_reader.remaining() == 0 &&
                             value >= 0 &&
                             (note.updated_at_unix_ms = static_cast<std::uint64_t>(value), true);
                    }
                  }
                  return true;
                }) && (target->notes.push_back(std::move(note)), true);
          }
        }
        return true;
      });
}

[[maybe_unused]] bool EncodeBranchReviewState(const PersistedBranchReviewState& state,
                                              std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  for (const PersistedBranchReviewTarget& target : state.targets) {
    std::vector<std::byte> payload;
    if (!EncodeBranchReviewTarget(target, &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(BranchReviewStateTag::Target), payload, out)) {
      return false;
    }
  }
  return true;
}

[[maybe_unused]] bool DecodeBranchReviewState(std::span<const std::byte> input,
                                              PersistedBranchReviewState* state) {
  if (state == nullptr) {
    return false;
  }
  *state = PersistedBranchReviewState{};
  return ParseRecordStream<BranchReviewStateTag>(
      input, [&](BranchReviewStateTag tag, std::span<const std::byte> payload) {
        if (tag != BranchReviewStateTag::Target) {
          return true;
        }
        PersistedBranchReviewTarget target;
        if (!DecodeBranchReviewTarget(payload, &target)) {
          return false;
        }
        state->targets.push_back(std::move(target));
        return true;
      });
}

[[maybe_unused]] bool DecodeCommitDraft(std::span<const std::byte> input,
                                        PersistedCommitDraftState* draft) {
  if (draft == nullptr) {
    return false;
  }
  *draft = PersistedCommitDraftState{};
  return ParseRecordStream<CommitDraftTag>(
      input, [&](CommitDraftTag tag, std::span<const std::byte> payload) {
        PrimitiveReader reader(payload);
        switch (tag) {
          case CommitDraftTag::HeadOid:
            return reader.ReadString(&draft->head_oid) && reader.remaining() == 0;
          case CommitDraftTag::BranchName:
            return reader.ReadString(&draft->branch_name) && reader.remaining() == 0;
          case CommitDraftTag::Subject:
            return reader.ReadString(&draft->subject) && reader.remaining() == 0;
          case CommitDraftTag::Body:
            return reader.ReadString(&draft->body) && reader.remaining() == 0;
        }
        return true;
      });
}

[[maybe_unused]] bool EncodeSettingPair(const std::pair<std::string, std::string>& setting,
                                        std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  PrimitiveWriter writer(out);
  return writer.WriteString(setting.first) && writer.WriteString(setting.second);
}

[[maybe_unused]] bool DecodeSettingPair(std::span<const std::byte> input,
                                        std::pair<std::string, std::string>* setting) {
  if (setting == nullptr) {
    return false;
  }
  PrimitiveReader reader(input);
  return reader.ReadString(&setting->first) && reader.ReadString(&setting->second) &&
         reader.remaining() == 0;
}

}  // namespace

}  // namespace microide::workspace
