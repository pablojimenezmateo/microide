#include "workspace/WorkspacePersistenceBinaryInternal.h"

namespace microide::workspace {

namespace {

std::string SerializeOutgoingBaseChoiceKind(OutgoingBaseChoice::Kind kind) {
  switch (kind) {
    case OutgoingBaseChoice::Kind::Auto:
      return "auto";
    case OutgoingBaseChoice::Kind::PreviousCommit:
      return "previous_commit";
    case OutgoingBaseChoice::Kind::SpecificRef:
      return "specific_ref";
  }
  return "auto";
}

OutgoingBaseChoice::Kind ParseOutgoingBaseChoiceKind(std::string_view value) {
  if (value == "previous_commit") {
    return OutgoingBaseChoice::Kind::PreviousCommit;
  }
  if (value == "specific_ref") {
    return OutgoingBaseChoice::Kind::SpecificRef;
  }
  return OutgoingBaseChoice::Kind::Auto;
}

}  // namespace

bool EncodeProjectSessionRecord(const PersistedProjectSessionState& state,
                                std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  if (!AppendRecord(ProjectSessionTag::Schema,
                    [&](PrimitiveWriter& w) { return w.WriteU32(kProjectSessionSchemaVersion); }, out) ||
      !AppendRecord(ProjectSessionTag::SidebarVisible,
                    [&](PrimitiveWriter& w) { return w.WriteBool(state.sidebar_visible); }, out) ||
      !AppendRecord(ProjectSessionTag::SidebarWidth,
                    [&](PrimitiveWriter& w) { return w.WriteF32(state.sidebar_width); }, out) ||
      !AppendRecord(ProjectSessionTag::BottomPanelHeight,
                    [&](PrimitiveWriter& w) { return w.WriteF32(state.bottom_panel_height); }, out) ||
      !AppendRecord(ProjectSessionTag::OutgoingBaseKind,
                    [&](PrimitiveWriter& w) {
                      return w.WriteString(
                          SerializeOutgoingBaseChoiceKind(state.outgoing_base_choice.kind));
                    },
                    out) ||
      !AppendRecord(ProjectSessionTag::OutgoingBaseCustomRef,
                    [&](PrimitiveWriter& w) {
                      return w.WriteString(state.outgoing_base_choice.custom_ref);
                    },
                    out) ||
      !AppendRecord(ProjectSessionTag::FocusedGroupIndex,
                    [&](PrimitiveWriter& w) { return WriteSize(w, state.focused_group_index); }, out) ||
      !AppendRecord(ProjectSessionTag::GroupSplitOrientation,
                    [&](PrimitiveWriter& w) { return w.WriteU8(state.group_split_orientation); }, out) ||
      !AppendRecord(ProjectSessionTag::GroupSplitFraction,
                    [&](PrimitiveWriter& w) { return w.WriteF32(state.group_split_fraction); }, out) ||
      !AppendRecord(ProjectSessionTag::RightPaneVisible,
                    [&](PrimitiveWriter& w) { return w.WriteBool(state.right_pane_visible); }, out) ||
      !AppendRecord(ProjectSessionTag::RightPaneWidth,
                    [&](PrimitiveWriter& w) { return w.WriteF32(state.right_pane_width); }, out) ||
      !AppendRecord(ProjectSessionTag::RightPaneMode,
                    [&](PrimitiveWriter& w) { return w.WriteU8(state.right_pane_mode); }, out)) {
    return false;
  }

  for (const auto& group : state.groups) {
    std::vector<std::byte> payload;
    if (!EncodeEditorGroup(group, &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(ProjectSessionTag::Group), payload, out)) {
      return false;
    }
  }

  return true;
}

bool DecodeProjectSessionRecord(std::span<const std::byte> input,
                                PersistedProjectSessionState* state) {
  if (state == nullptr) {
    return false;
  }
  *state = PersistedProjectSessionState{};
  bool seen_schema = false;
  return ParseRecordStream<ProjectSessionTag>(
             input, [&](ProjectSessionTag tag, std::span<const std::byte> payload) {
               PrimitiveReader reader(payload);
               switch (tag) {
                 case ProjectSessionTag::Schema: {
                   std::uint32_t schema = 0;
                   if (!reader.ReadU32(&schema) || reader.remaining() != 0 ||
                       schema != kProjectSessionSchemaVersion) {
                     return false;
                   }
                   seen_schema = true;
                   return true;
                 }
                 case ProjectSessionTag::SidebarVisible:
                   return reader.ReadBool(&state->sidebar_visible) && reader.remaining() == 0;
                 case ProjectSessionTag::SidebarWidth:
                   return reader.ReadF32(&state->sidebar_width) && reader.remaining() == 0;
                 case ProjectSessionTag::BottomPanelHeight:
                   return reader.ReadF32(&state->bottom_panel_height) && reader.remaining() == 0;
                 case ProjectSessionTag::OutgoingBaseKind: {
                   std::string value;
                   if (!reader.ReadString(&value) || reader.remaining() != 0) {
                     return false;
                   }
                   state->outgoing_base_choice.kind = ParseOutgoingBaseChoiceKind(value);
                   return true;
                 }
                 case ProjectSessionTag::OutgoingBaseCustomRef:
                   return reader.ReadString(&state->outgoing_base_choice.custom_ref) &&
                          reader.remaining() == 0;
                 case ProjectSessionTag::FocusedGroupIndex:
                   return ReadSize(reader, &state->focused_group_index) && reader.remaining() == 0;
                 case ProjectSessionTag::GroupSplitOrientation:
                   return reader.ReadU8(&state->group_split_orientation) && reader.remaining() == 0;
                 case ProjectSessionTag::GroupSplitFraction:
                   return reader.ReadF32(&state->group_split_fraction) && reader.remaining() == 0;
                 case ProjectSessionTag::RightPaneVisible:
                   return reader.ReadBool(&state->right_pane_visible) && reader.remaining() == 0;
                 case ProjectSessionTag::RightPaneWidth:
                   return reader.ReadF32(&state->right_pane_width) && reader.remaining() == 0;
                 case ProjectSessionTag::RightPaneMode:
                   return reader.ReadU8(&state->right_pane_mode) && reader.remaining() == 0;
                 case ProjectSessionTag::Group: {
                   PersistedEditorGroupState group;
                   if (!DecodeEditorGroup(payload, &group)) {
                     return false;
                   }
                   // Editor groups are capped at 2: tolerate a malformed/forged
                   // session that lists more by keeping only the first two.
                   if (state->groups.size() < 2) {
                     state->groups.push_back(std::move(group));
                   }
                   return true;
                 }
                case ProjectSessionTag::ChatRegistry:
                  // Legacy AI/chat records are tolerated during decode and ignored.
                  return true;
               }
               return true;
             }) &&
         seen_schema;
}

bool EncodeWorkspaceSessionRecord(const PersistedWorkspaceSessionState& state,
                                  std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  if (!AppendRecord(WorkspaceSessionTag::Schema,
                    [&](PrimitiveWriter& w) { return w.WriteU32(kSchemaVersion); }, out) ||
      !AppendRecord(WorkspaceSessionTag::ActiveProjectIndex,
                    [&](PrimitiveWriter& w) { return WriteSize(w, state.active_project_index); },
                    out)) {
    return false;
  }
  for (const auto& root : state.project_roots) {
    if (!AppendRecord(WorkspaceSessionTag::ProjectRoot,
                      [&](PrimitiveWriter& w) { return w.WritePath(root); }, out)) {
      return false;
    }
  }
  return true;
}

bool DecodeWorkspaceSessionRecord(std::span<const std::byte> input,
                                  PersistedWorkspaceSessionState* state) {
  if (state == nullptr) {
    return false;
  }
  *state = PersistedWorkspaceSessionState{};
  bool seen_schema = false;
  return ParseRecordStream<WorkspaceSessionTag>(
             input, [&](WorkspaceSessionTag tag, std::span<const std::byte> payload) {
               PrimitiveReader reader(payload);
               switch (tag) {
                 case WorkspaceSessionTag::Schema: {
                   std::uint32_t schema = 0;
                   if (!reader.ReadU32(&schema) || reader.remaining() != 0 || schema != kSchemaVersion) {
                     return false;
                   }
                   seen_schema = true;
                   return true;
                 }
                 case WorkspaceSessionTag::ProjectRoot: {
                   std::filesystem::path root;
                   if (!reader.ReadPath(&root) || reader.remaining() != 0) {
                     return false;
                   }
                   state->project_roots.push_back(std::move(root));
                   return true;
                 }
                 case WorkspaceSessionTag::ActiveProjectIndex:
                   return ReadSize(reader, &state->active_project_index) && reader.remaining() == 0;
               }
               return true;
             }) &&
         seen_schema;
}

}  // namespace microide::workspace
