#include "workspace/WorkspacePersistenceBinaryInternal.h"

namespace microide::workspace {

bool EncodeUserConfigRecord(const PersistedUserConfigState& state, std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  if (!AppendRecord(UserConfigTag::Schema, [&](PrimitiveWriter& w) { return w.WriteU32(kSchemaVersion); },
                    out) ||
      !AppendRecord(UserConfigTag::UiScale, [&](PrimitiveWriter& w) { return w.WriteF32(state.ui_scale); },
                    out)) {
    return false;
  }
  for (const auto& setting : state.settings) {
    std::vector<std::byte> payload;
    if (!EncodeSettingPair(setting, &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(UserConfigTag::Setting), payload, out)) {
      return false;
    }
  }
  for (const auto& id : state.disabled_keybinding_ids) {
    if (!AppendRecord(UserConfigTag::DisabledKeybinding,
                      [&](PrimitiveWriter& w) { return w.WriteString(id); }, out)) {
      return false;
    }
  }
  return true;
}

bool DecodeUserConfigRecord(std::span<const std::byte> input, PersistedUserConfigState* state) {
  if (state == nullptr) {
    return false;
  }
  *state = PersistedUserConfigState{};
  bool seen_schema = false;
  return ParseRecordStream<UserConfigTag>(
             input, [&](UserConfigTag tag, std::span<const std::byte> payload) {
               PrimitiveReader reader(payload);
               switch (tag) {
                 case UserConfigTag::Schema: {
                   std::uint32_t schema = 0;
                   if (!reader.ReadU32(&schema) || reader.remaining() != 0 || schema != kSchemaVersion) {
                     return false;
                   }
                   seen_schema = true;
                   return true;
                 }
                 case UserConfigTag::UiScale:
                   return reader.ReadF32(&state->ui_scale) && reader.remaining() == 0;
                 case UserConfigTag::Setting: {
                   std::pair<std::string, std::string> setting;
                   if (!DecodeSettingPair(payload, &setting)) {
                     return false;
                   }
                   state->settings.push_back(std::move(setting));
                   return true;
                 }
                 case UserConfigTag::DisabledKeybinding: {
                   std::string id;
                   if (!reader.ReadString(&id) || reader.remaining() != 0) {
                     return false;
                   }
                   state->disabled_keybinding_ids.push_back(std::move(id));
                   return true;
                 }
               }
               return true;
             }) &&
         seen_schema;
}

bool EncodeProjectConfigRecord(const PersistedProjectConfigState& state, std::vector<std::byte>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  if (!AppendRecord(ProjectConfigTag::Schema,
                    [&](PrimitiveWriter& w) { return w.WriteU32(kSchemaVersion); }, out) ||
      !AppendRecord(ProjectConfigTag::EditorTabSize,
                    [&](PrimitiveWriter& w) { return WriteSize(w, state.editor_tab_size); }, out) ||
      !AppendRecord(ProjectConfigTag::EditorIndentWidth,
                    [&](PrimitiveWriter& w) { return WriteSize(w, state.editor_indent_width); },
                    out) ||
      !AppendRecord(ProjectConfigTag::EditorSoftTabs,
                    [&](PrimitiveWriter& w) { return w.WriteBool(state.editor_soft_tabs); }, out) ||
      !AppendRecord(ProjectConfigTag::ColorschemeName,
                    [&](PrimitiveWriter& w) { return w.WriteString(state.colorscheme_name); }, out)) {
    return false;
  }
  if (state.project_base_color.has_value()) {
    if (!AppendRecord(ProjectConfigTag::ProjectBaseColor,
                      [&](PrimitiveWriter& w) {
                        return w.WriteU8(state.project_base_color->r) &&
                               w.WriteU8(state.project_base_color->g) &&
                               w.WriteU8(state.project_base_color->b) &&
                               w.WriteU8(state.project_base_color->a);
                      },
                      out)) {
      return false;
    }
  }
  for (const auto& setting : state.settings) {
    std::vector<std::byte> payload;
    if (!EncodeSettingPair(setting, &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(ProjectConfigTag::Setting), payload, out)) {
      return false;
    }
  }
  for (const auto& policy : state.sidebar_policies) {
    std::vector<std::byte> payload;
    if (!EncodeSidebarPolicy(policy, &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(ProjectConfigTag::SidebarPolicy), payload, out)) {
      return false;
    }
  }
  if (state.commit_draft.has_value()) {
    std::vector<std::byte> payload;
    if (!EncodeCommitDraft(*state.commit_draft, &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(ProjectConfigTag::CommitDraft), payload, out)) {
      return false;
    }
  }
  return true;
}

bool DecodeProjectConfigRecord(std::span<const std::byte> input, PersistedProjectConfigState* state) {
  if (state == nullptr) {
    return false;
  }
  *state = PersistedProjectConfigState{};
  bool seen_schema = false;
  return ParseRecordStream<ProjectConfigTag>(
             input, [&](ProjectConfigTag tag, std::span<const std::byte> payload) {
               PrimitiveReader reader(payload);
               switch (tag) {
                 case ProjectConfigTag::Schema: {
                   std::uint32_t schema = 0;
                   if (!reader.ReadU32(&schema) || reader.remaining() != 0 || schema != kSchemaVersion) {
                     return false;
                   }
                   seen_schema = true;
                   return true;
                 }
                 case ProjectConfigTag::EditorTabSize:
                   return ReadSize(reader, &state->editor_tab_size) && reader.remaining() == 0;
                 case ProjectConfigTag::EditorIndentWidth:
                   return ReadSize(reader, &state->editor_indent_width) && reader.remaining() == 0;
                 case ProjectConfigTag::EditorSoftTabs:
                   return reader.ReadBool(&state->editor_soft_tabs) && reader.remaining() == 0;
                 case ProjectConfigTag::ColorschemeName:
                   return reader.ReadString(&state->colorscheme_name) && reader.remaining() == 0;
                 case ProjectConfigTag::ProjectBaseColor: {
                   SDL_Color color{};
                   if (!reader.ReadU8(&color.r) || !reader.ReadU8(&color.g) || !reader.ReadU8(&color.b) ||
                       !reader.ReadU8(&color.a) || reader.remaining() != 0) {
                     return false;
                   }
                   state->project_base_color = color;
                   return true;
                 }
                 case ProjectConfigTag::Setting: {
                   std::pair<std::string, std::string> setting;
                   if (!DecodeSettingPair(payload, &setting)) {
                     return false;
                   }
                   state->settings.push_back(std::move(setting));
                   return true;
                 }
                case ProjectConfigTag::SidebarPolicy: {
                  PersistedSidebarViewPolicy policy;
                  if (!DecodeSidebarPolicy(payload, &policy)) {
                    return false;
                  }
                  state->sidebar_policies.push_back(std::move(policy));
                  return true;
                }
                case ProjectConfigTag::CommitDraft: {
                  PersistedCommitDraftState draft;
                  if (!DecodeCommitDraft(payload, &draft)) {
                    return false;
                  }
                  state->commit_draft = std::move(draft);
                  return true;
                }
               }
               return true;
             }) &&
         seen_schema;
}

}  // namespace microide::workspace
