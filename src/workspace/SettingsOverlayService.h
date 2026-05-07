#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/WorkspaceAiProvider.h"
#include "workspace/WorkspaceSettingsRegistry.h"

namespace microide::workspace {

enum class SettingsOverlayMode {
  Settings,
  AiProvider,
  HelpAbout,
};

struct SettingsOverlayRow {
  std::string id;
  std::string label;
  std::string value;
  std::string detail;
  SettingType type = SettingType::String;
  SettingScope scope = SettingScope::Project;
  bool resettable = false;
  bool editable = false;
};

struct AiProviderPickerRow {
  std::string id;
  std::string label;
  std::string model;
  std::string auth_method;
  bool requires_api_key = false;
  bool active = false;
};

struct HelpAboutRow {
  std::string label;
  std::string detail;
};

class SettingsOverlayService {
 public:
  void OpenSettings();
  void OpenAiProviderPicker();
  void OpenHelpAbout();
  void Close();

  bool Visible() const { return visible_; }
  SettingsOverlayMode Mode() const { return mode_; }
  int ScrollRow() const { return scroll_row_; }
  void SetScrollRow(int row);
  const std::string& Query() const { return query_; }
  void SetQuery(std::string query);

  void RebuildSettingsRows(const std::vector<SettingInfo>& settings,
                           const std::vector<std::pair<std::string, std::string>>& user_settings,
                           const std::vector<std::pair<std::string, std::string>>& project_settings);
  void RebuildProviderRows(const std::vector<AiProviderSpec>& providers,
                           std::string_view active_provider_id);
  void RebuildHelpRows(std::vector<HelpAboutRow> rows);

  const std::vector<SettingsOverlayRow>& SettingsRows() const { return settings_rows_; }
  const std::vector<AiProviderPickerRow>& ProviderRows() const { return provider_rows_; }
  const std::vector<HelpAboutRow>& HelpRows() const { return help_rows_; }
  std::size_t VisibleRowCount() const;

 private:
  bool RowMatchesQuery(std::string_view label, std::string_view detail) const;

  bool visible_ = false;
  SettingsOverlayMode mode_ = SettingsOverlayMode::Settings;
  int scroll_row_ = 0;
  std::string query_;
  std::vector<SettingsOverlayRow> settings_rows_;
  std::vector<AiProviderPickerRow> provider_rows_;
  std::vector<HelpAboutRow> help_rows_;
};

}  // namespace microide::workspace
