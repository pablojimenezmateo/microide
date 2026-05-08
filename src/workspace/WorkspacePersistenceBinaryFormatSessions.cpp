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
                    [&](PrimitiveWriter& w) { return w.WriteU32(kSchemaVersion); }, out) ||
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
      !AppendRecord(ProjectSessionTag::ActiveTabIndex,
                    [&](PrimitiveWriter& w) { return WriteSize(w, state.active_tab_index); }, out)) {
    return false;
  }

  for (const auto& tab : state.tabs) {
    std::vector<std::byte> payload;
    if (!EncodeEditorTab(tab, &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(ProjectSessionTag::Tab), payload, out)) {
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
                   if (!reader.ReadU32(&schema) || reader.remaining() != 0 || schema != kSchemaVersion) {
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
                 case ProjectSessionTag::ActiveTabIndex:
                   return ReadSize(reader, &state->active_tab_index) && reader.remaining() == 0;
                 case ProjectSessionTag::Tab: {
                   PersistedEditorTabState tab;
                   if (!DecodeEditorTab(payload, &tab)) {
                     return false;
                   }
                   state->tabs.push_back(std::move(tab));
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
