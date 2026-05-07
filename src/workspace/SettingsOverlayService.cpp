#include "workspace/SettingsOverlayService.h"

#include <algorithm>
#include <cctype>

namespace microide::workspace {

namespace {

const std::string* FindStoredValue(const std::vector<std::pair<std::string, std::string>>& values,
                                   std::string_view id) {
  const auto it = std::find_if(values.begin(), values.end(),
                               [id](const auto& entry) { return entry.first == id; });
  return it == values.end() ? nullptr : &it->second;
}

std::string LowerAscii(std::string_view text) {
  std::string lowered(text);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lowered;
}

}  // namespace

void SettingsOverlayService::OpenSettings() {
  visible_ = true;
  mode_ = SettingsOverlayMode::Settings;
  scroll_row_ = 0;
}

void SettingsOverlayService::OpenAiProviderPicker() {
  visible_ = true;
  mode_ = SettingsOverlayMode::AiProvider;
  scroll_row_ = 0;
}

void SettingsOverlayService::OpenHelpAbout() {
  visible_ = true;
  mode_ = SettingsOverlayMode::HelpAbout;
  scroll_row_ = 0;
}

void SettingsOverlayService::Close() {
  visible_ = false;
  scroll_row_ = 0;
}

void SettingsOverlayService::SetScrollRow(int row) {
  scroll_row_ = std::max(0, row);
}

void SettingsOverlayService::SetQuery(std::string query) {
  query_ = std::move(query);
  scroll_row_ = 0;
}

void SettingsOverlayService::RebuildSettingsRows(
    const std::vector<SettingInfo>& settings,
    const std::vector<std::pair<std::string, std::string>>& user_settings,
    const std::vector<std::pair<std::string, std::string>>& project_settings) {
  settings_rows_.clear();
  settings_rows_.reserve(settings.size());
  for (const SettingInfo& setting : settings) {
    if (!RowMatchesQuery(setting.label, setting.id)) {
      continue;
    }
    const std::string* stored = setting.scope == SettingScope::User
                                    ? FindStoredValue(user_settings, setting.id)
                                    : FindStoredValue(project_settings, setting.id);
    SettingsOverlayRow row;
    row.id = setting.id;
    row.label = setting.label;
    row.value = stored != nullptr ? *stored : SerializeSettingValue(setting.default_value);
    row.detail = setting.scope == SettingScope::User ? "User / " : "Project / ";
    row.detail += setting.plugin_id.empty() ? "built-in" : "plugin:" + setting.plugin_id;
    row.type = setting.type;
    row.scope = setting.scope;
    row.resettable = stored != nullptr;
    row.editable = true;
    settings_rows_.push_back(std::move(row));
  }
}

void SettingsOverlayService::RebuildProviderRows(const std::vector<AiProviderSpec>& providers,
                                                 std::string_view active_provider_id) {
  provider_rows_.clear();
  provider_rows_.reserve(providers.size());
  for (const AiProviderSpec& provider : providers) {
    const std::string label = provider.display_name.empty() ? provider.label : provider.display_name;
    if (!RowMatchesQuery(label, provider.id)) {
      continue;
    }
    AiProviderPickerRow row;
    row.id = provider.id;
    row.label = label;
    row.model = provider.default_model.empty()
                    ? (provider.models.empty() ? std::string{} : provider.models.front())
                    : provider.default_model;
    row.auth_method = provider.requires_api_key ? provider.auth_method + " required" : provider.auth_method;
    row.requires_api_key = provider.requires_api_key;
    row.active = provider.id == active_provider_id;
    provider_rows_.push_back(std::move(row));
  }
}

void SettingsOverlayService::RebuildHelpRows(std::vector<HelpAboutRow> rows) {
  help_rows_.clear();
  help_rows_.reserve(rows.size());
  for (HelpAboutRow& row : rows) {
    if (RowMatchesQuery(row.label, row.detail)) {
      help_rows_.push_back(std::move(row));
    }
  }
}

std::size_t SettingsOverlayService::VisibleRowCount() const {
  switch (mode_) {
    case SettingsOverlayMode::Settings:
      return settings_rows_.size();
    case SettingsOverlayMode::AiProvider:
      return provider_rows_.size();
    case SettingsOverlayMode::HelpAbout:
      return help_rows_.size();
  }
  return 0;
}

bool SettingsOverlayService::RowMatchesQuery(std::string_view label, std::string_view detail) const {
  if (query_.empty()) {
    return true;
  }
  const std::string needle = LowerAscii(query_);
  return LowerAscii(label).find(needle) != std::string::npos ||
         LowerAscii(detail).find(needle) != std::string::npos;
}

}  // namespace microide::workspace
