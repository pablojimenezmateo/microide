#include "workspace/persistence/WorkspacePersistenceBinaryInternal.h"

#include "workspace/persistence/SettingsStore.h"

#include <algorithm>
#include <string>
#include <string_view>

#include "util/FlatDedupSet.h"

namespace microide::workspace {

namespace {

constexpr std::size_t kMaxPersistedSettings = 8192;
constexpr std::size_t kMaxPersistedDisabledIds = 8192;
constexpr std::size_t kMaxPersistedSidebarPolicies = 512;

auto MakeIdIndex(const std::vector<std::string>& ids) {
  return util::FlatKeyIndex(
      8, [&ids](std::size_t index) -> std::string_view { return ids[index]; });
}

auto MakeSettingIndex(const std::vector<std::pair<std::string, std::string>>& settings) {
  return util::FlatKeyIndex(
      8, [&settings](std::size_t index) -> std::string_view { return settings[index].first; });
}

template <typename Index>
bool AppendDisabledIdCapped(std::vector<std::string>* ids, Index* index, std::string id) {
  if (index->Find(id) != util::kFlatKeyNotFound) {
    return true;
  }
  if (ids->size() >= kMaxPersistedDisabledIds) {
    return false;
  }
  ids->push_back(std::move(id));
  index->Insert(ids->size() - 1);
  return true;
}

template <typename Index>
bool AppendSettingRecord(std::span<const std::byte> payload, std::vector<std::pair<std::string, std::string>>* settings,
                         Index* index) {
  std::pair<std::string, std::string> setting;
  if (!DecodeSettingPair(payload, &setting)) {
    return false;
  }
  if (!SettingsStore::IsValidSettingId(setting.first)) {
    return true;
  }
  const std::size_t existing = index->Find(setting.first);
  if (existing != util::kFlatKeyNotFound) {
    (*settings)[existing].second = std::move(setting.second);
    return true;
  }
  if (settings->size() >= kMaxPersistedSettings) {
    return false;
  }
  settings->push_back(std::move(setting));
  index->Insert(settings->size() - 1);
  return true;
}

}

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
    if (!SettingsStore::IsValidSettingId(setting.first)) {
      continue;
    }
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
  for (const auto& id : state.disabled_plugin_ids) {
    if (!AppendRecord(UserConfigTag::DisabledPlugin,
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
  auto setting_index = MakeSettingIndex(state->settings);
  auto disabled_keybinding_index = MakeIdIndex(state->disabled_keybinding_ids);
  auto disabled_plugin_index = MakeIdIndex(state->disabled_plugin_ids);
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
                 case UserConfigTag::Setting:
                   return AppendSettingRecord(payload, &state->settings, &setting_index);
                 case UserConfigTag::DisabledKeybinding: {
                   std::string id;
                   if (!reader.ReadString(&id) || reader.remaining() != 0) {
                     return false;
                   }
                   return AppendDisabledIdCapped(&state->disabled_keybinding_ids,
                                                 &disabled_keybinding_index, std::move(id));
                 }
                 case UserConfigTag::DisabledPlugin: {
                   std::string id;
                   if (!reader.ReadString(&id) || reader.remaining() != 0) {
                     return false;
                   }
                   return AppendDisabledIdCapped(&state->disabled_plugin_ids,
                                                 &disabled_plugin_index, std::move(id));
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
    if (!SettingsStore::IsValidSettingId(setting.first)) {
      continue;
    }
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
  if (!state.branch_review.targets.empty()) {
    std::vector<std::byte> payload;
    if (!EncodeBranchReviewState(state.branch_review, &payload) ||
        !AppendTaggedRecord(static_cast<std::uint16_t>(ProjectConfigTag::BranchReviewState), payload,
                            out)) {
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
  std::optional<std::size_t> legacy_tab_size;
  std::optional<std::size_t> legacy_indent_width;
  std::optional<bool> legacy_soft_tabs;
  auto setting_index = MakeSettingIndex(state->settings);
  const bool parsed = ParseRecordStream<ProjectConfigTag>(
             input, [&](ProjectConfigTag tag, std::span<const std::byte> payload) {
               PrimitiveReader reader(payload);
               switch (static_cast<std::uint16_t>(tag)) {
                 case 2: {
                   std::size_t value = 0;
                   if (!ReadSize(reader, &value) || reader.remaining() != 0) {
                     return false;
                   }
                   legacy_tab_size = value;
                   return true;
                 }
                 case 3: {
                   std::size_t value = 0;
                   if (!ReadSize(reader, &value) || reader.remaining() != 0) {
                     return false;
                   }
                   legacy_indent_width = value;
                   return true;
                 }
                 case 4: {
                   bool value = false;
                   if (!reader.ReadBool(&value) || reader.remaining() != 0) {
                     return false;
                   }
                   legacy_soft_tabs = value;
                   return true;
                 }
                 default:
                   break;
               }
               switch (tag) {
                 case ProjectConfigTag::Schema: {
                   std::uint32_t schema = 0;
                   if (!reader.ReadU32(&schema) || reader.remaining() != 0 || schema != kSchemaVersion) {
                     return false;
                   }
                   seen_schema = true;
                   return true;
                 }
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
                 case ProjectConfigTag::Setting:
                   return AppendSettingRecord(payload, &state->settings, &setting_index);
                 case ProjectConfigTag::SidebarPolicy: {
                   PersistedSidebarViewPolicy policy;
                   if (!DecodeSidebarPolicy(payload, &policy)) {
                     return false;
                   }
                   if (state->sidebar_policies.size() >= kMaxPersistedSidebarPolicies) {
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
                case ProjectConfigTag::BranchReviewState: {
                  PersistedBranchReviewState review_state;
                  if (!DecodeBranchReviewState(payload, &review_state)) {
                    return false;
                  }
                  state->branch_review = std::move(review_state);
                  return true;
                }
               }
               return true;
             });
  if (!parsed || !seen_schema) {
    return false;
  }
  const auto has_setting = [&](std::string_view id) {
    return std::any_of(state->settings.begin(), state->settings.end(),
                       [id](const auto& kv) { return kv.first == id; });
  };
  if (legacy_tab_size.has_value() && !has_setting("editor.tab_size")) {
    state->settings.emplace_back("editor.tab_size", std::to_string(*legacy_tab_size));
  }
  if (legacy_indent_width.has_value() && !has_setting("editor.indent_width")) {
    state->settings.emplace_back("editor.indent_width", std::to_string(*legacy_indent_width));
  }
  if (legacy_soft_tabs.has_value() && !has_setting("editor.soft_tabs")) {
    state->settings.emplace_back("editor.soft_tabs", *legacy_soft_tabs ? "true" : "false");
  }
  return true;
}

}
