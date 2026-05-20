#include "workspace/SettingsOverlayService.h"

#include <algorithm>
#include <cctype>

#include "util/StringUtil.h"

namespace microide::workspace {

namespace {

const std::string* FindStoredValue(const std::vector<std::pair<std::string, std::string>>& values,
                                   std::string_view id) {
  const auto it = std::find_if(values.begin(), values.end(),
                               [id](const auto& entry) { return entry.first == id; });
  return it == values.end() ? nullptr : &it->second;
}

}  // namespace

void SettingsOverlayService::OpenSettings() {
  visible_ = true;
  mode_ = SettingsOverlayMode::Settings;
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
    row.group = setting.group;
    row.type = setting.type;
    row.scope = setting.scope;
    row.resettable = stored != nullptr;
    row.editable = true;
    settings_rows_.push_back(std::move(row));
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
    case SettingsOverlayMode::HelpAbout:
      return help_rows_.size();
  }
  return 0;
}

bool SettingsOverlayService::RowMatchesQuery(std::string_view label, std::string_view detail) const {
  if (query_.empty()) {
    return true;
  }
  const std::string needle = util::ToLowerAscii(query_);
  return util::ToLowerAscii(label).find(needle) != std::string::npos ||
         util::ToLowerAscii(detail).find(needle) != std::string::npos;
}

}  // namespace microide::workspace
