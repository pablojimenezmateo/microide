#include "workspace/persistence/WorkspacePersistenceBinaryInternal.h"

#include "workspace/persistence/SettingsStore.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace microide::workspace {

namespace {

// Product-sized decode budgets for persisted user/project config. The raw record is
// already capped at 256 MiB by PersistedRecordReader, but that is far above any real
// config: a corrupt/hostile file can otherwise spend startup time + heap on hundreds
// of thousands of tiny unique settings / disabled IDs / sidebar policies. Every other
// persisted collection (session trees, debug state, review state, plugin registries)
// has an explicit product cap; config restore now matches. (TD-2026-07-16-36.)
constexpr std::size_t kMaxPersistedSettings = 8192;
constexpr std::size_t kMaxPersistedDisabledIds = 8192;
constexpr std::size_t kMaxPersistedSidebarPolicies = 512;

// Append `id` to `ids` unless already present (dedupe by value) or the cap is reached.
// Returns false only when the cap would be exceeded by a NEW id, so the caller can fail
// the record closed. Repeated stale entries are silently deduped. `seen` is the caller's
// O(1) dedupe index over `ids` — a hostile config with thousands of unique disabled IDs
// otherwise pays O(n^2) string comparisons here during startup.
bool AppendDisabledIdCapped(std::vector<std::string>* ids,
                            std::unordered_set<std::string>* seen, std::string id) {
  if (!seen->insert(id).second) {
    return true;  // dedupe: repeated entries do not inflate downstream resolution
  }
  if (ids->size() >= kMaxPersistedDisabledIds) {
    return false;
  }
  ids->push_back(std::move(id));
  return true;
}

}  // namespace

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
  // O(1) dedupe indexes so a config with thousands of unique settings / disabled
  // IDs decodes in O(n) rather than paying an O(n) linear scan per record.
  std::unordered_map<std::string, std::size_t> setting_index;
  std::unordered_set<std::string> disabled_keybinding_seen;
  std::unordered_set<std::string> disabled_plugin_seen;
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
                   // Fail closed on a malformed setting id: runtime mutation already
                   // rejects invalid ids, so restore must not smuggle them in.
                   if (!SettingsStore::IsValidSettingId(setting.first)) {
                     return false;
                   }
                   // Dedupe by id, last-writer-wins: a corrupt/hand-edited config
                   // with duplicate keys must not become a split-brain state where
                   // the UI shows one value and layering applies another.
                   auto existing = setting_index.find(setting.first);
                   if (existing != setting_index.end()) {
                     state->settings[existing->second].second = std::move(setting.second);
                   } else {
                     if (state->settings.size() >= kMaxPersistedSettings) {
                       return false;  // over the product settings budget
                     }
                     setting_index.emplace(setting.first, state->settings.size());
                     state->settings.push_back(std::move(setting));
                   }
                   return true;
                 }
                 case UserConfigTag::DisabledKeybinding: {
                   std::string id;
                   if (!reader.ReadString(&id) || reader.remaining() != 0) {
                     return false;
                   }
                   return AppendDisabledIdCapped(&state->disabled_keybinding_ids,
                                                 &disabled_keybinding_seen, std::move(id));
                 }
                 case UserConfigTag::DisabledPlugin: {
                   std::string id;
                   if (!reader.ReadString(&id) || reader.remaining() != 0) {
                     return false;
                   }
                   return AppendDisabledIdCapped(&state->disabled_plugin_ids,
                                                 &disabled_plugin_seen, std::move(id));
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
  // Legacy typed editor-preference tags 2-4 (EditorTabSize/EditorIndentWidth/
  // EditorSoftTabs) were retired from encode; the canonical prefs now round-trip
  // through layered `Setting` records (tag 7). A project config written before that
  // migration carries these prefs ONLY in the typed tags, so capture them here and
  // fold them into the settings layer below — otherwise those files silently revert
  // to the spec default indentation on load.
  std::optional<std::size_t> legacy_tab_size;
  std::optional<std::size_t> legacy_indent_width;
  std::optional<bool> legacy_soft_tabs;
  // O(1) settings dedupe index (see DecodeUserConfigRecord).
  std::unordered_map<std::string, std::size_t> setting_index;
  const bool parsed = ParseRecordStream<ProjectConfigTag>(
             input, [&](ProjectConfigTag tag, std::span<const std::byte> payload) {
               PrimitiveReader reader(payload);
               switch (static_cast<std::uint16_t>(tag)) {
                 case 2: {  // retired ProjectConfigTag::EditorTabSize
                   std::size_t value = 0;
                   if (!ReadSize(reader, &value) || reader.remaining() != 0) {
                     return false;
                   }
                   legacy_tab_size = value;
                   return true;
                 }
                 case 3: {  // retired ProjectConfigTag::EditorIndentWidth
                   std::size_t value = 0;
                   if (!ReadSize(reader, &value) || reader.remaining() != 0) {
                     return false;
                   }
                   legacy_indent_width = value;
                   return true;
                 }
                 case 4: {  // retired ProjectConfigTag::EditorSoftTabs
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
                 case ProjectConfigTag::Setting: {
                   std::pair<std::string, std::string> setting;
                   if (!DecodeSettingPair(payload, &setting)) {
                     return false;
                   }
                   // Fail closed on a malformed setting id (see DecodeUserConfigRecord).
                   if (!SettingsStore::IsValidSettingId(setting.first)) {
                     return false;
                   }
                   // Dedupe by id, last-writer-wins (see DecodeUserConfigRecord).
                   auto existing = setting_index.find(setting.first);
                   if (existing != setting_index.end()) {
                     state->settings[existing->second].second = std::move(setting.second);
                   } else {
                     if (state->settings.size() >= kMaxPersistedSettings) {
                       return false;  // over the product settings budget
                     }
                     setting_index.emplace(setting.first, state->settings.size());
                     state->settings.push_back(std::move(setting));
                   }
                   return true;
                 }
                case ProjectConfigTag::SidebarPolicy: {
                  PersistedSidebarViewPolicy policy;
                  if (!DecodeSidebarPolicy(payload, &policy)) {
                    return false;
                  }
                  if (state->sidebar_policies.size() >= kMaxPersistedSidebarPolicies) {
                    return false;  // over the product sidebar-policy budget
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
  // Fold the captured legacy typed prefs into the settings layer, but only when a
  // modern `Setting` record does not already carry the same id — a newer file that
  // has both must let the layered record win.
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

}  // namespace microide::workspace
