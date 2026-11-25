#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "editor/SingleLineEditor.h"
#include "workspace/registries/WorkspaceSettingsRegistry.h"

namespace microide::workspace {

enum class SettingsOverlayMode {
  Settings,
  HelpAbout,
};

enum class SettingsControlKind {
  None,
  Checkbox,
  Stepper,
  Segmented,
  TextEdit,
};

enum class SettingsPane {
  Filter,
  Categories,
  Values,
  Count,
};

std::string_view SettingsCategoryLabel(std::string_view group);

struct SettingsOverlayRow {
  std::string id;
  std::string label;
  std::string value;
  std::string value_display;
  std::string description;
  std::string detail;
  std::string scope_label;
  std::string group;
  SettingType type = SettingType::String;
  SettingScope scope = SettingScope::Project;
  SettingsControlKind control_kind = SettingsControlKind::None;
  bool resettable = false;
  bool editable = false;
  bool scope_selectable = false;
  bool project_override = false;
  bool has_user_default = false;
  bool suggests_fonts = false;
};

struct HelpAboutRow {
  std::string label;
  std::string detail;
};

class SettingsOverlayService {
 public:
  static constexpr int kPickerVisibleFamilies = 10;

  void OpenSettings();
  void OpenHelpAbout();
  void Close();

  bool Visible() const { return visible_; }
  SettingsOverlayMode Mode() const { return mode_; }
  int ScrollRow() const { return scroll_row_; }
  void SetScrollRow(int row);
  int CategoryScrollRow() const { return category_scroll_row_; }
  void SetCategoryScrollRow(int row);
  const std::string& Query() const { return query_; }
  void SetQuery(std::string query);

  editor::SingleLineEditor& QueryEditor() { return query_editor_; }
  const editor::SingleLineEditor& QueryEditor() const { return query_editor_; }
  void SyncQueryFromEditor();

  editor::SingleLineEditor& ValueEditor() { return value_editor_; }
  const editor::SingleLineEditor& ValueEditor() const { return value_editor_; }
  bool EditingValue() const { return editing_value_; }
  const std::string& EditingRowId() const { return editing_row_id_; }
  void BeginValueEdit(std::string row_id, const std::string& initial_text);
  void CancelValueEdit();
  std::string ValueEditText() const;

  void BeginFontValueEdit(std::string row_id, std::vector<std::string> families);
  bool EditingFonts() const { return editing_fonts_; }
  const std::vector<std::string_view>& FilteredFontFamilies() const;
  int PickerRowCount() const;
  int PickerChooseFileIndex() const;
  int PickerHighlight() const { return picker_highlight_; }
  void SetPickerHighlight(int index);
  void MovePickerHighlight(int delta);
  void ResetPickerHighlight();
  int PickerScroll() const { return picker_scroll_; }
  void SetPickerScroll(int top);

  void RebuildSettingsRows(const std::vector<SettingInfo>& settings,
                           const std::vector<std::pair<std::string, std::string>>& user_settings,
                           const std::vector<std::pair<std::string, std::string>>& project_settings,
                           const std::vector<SettingsOverlayRow>& extra_rows = {});
  void RebuildHelpRows(std::vector<HelpAboutRow> rows);

  const std::vector<SettingsOverlayRow>& SettingsRows() const { return settings_rows_; }
  const std::vector<HelpAboutRow>& HelpRows() const { return help_rows_; }
  std::size_t VisibleRowCount() const;
  std::size_t TotalRowCount() const;

  const std::vector<std::string>& Categories() const { return categories_; }
  int SelectedCategory() const { return selected_category_; }
  void SetSelectedCategory(int category);
  int SelectedRow() const { return selected_row_; }
  void SetSelectedRow(int row);
  void MoveCategory(int delta);
  void MoveRow(int delta);
  SettingsPane FocusedPane() const { return focused_pane_; }
  void SetFocusedPane(SettingsPane pane) { focused_pane_ = pane; }
  void CycleFocusedPane(int delta);

  std::size_t RowCountInSelectedCategory() const;
  std::size_t RowCountInCategory(int category) const;
  const SettingsOverlayRow* RowAtVisibleIndex(int category, int row_in_category) const;
  const SettingsOverlayRow* SelectedSettingRow() const;

 private:
  bool RowMatchesQuery(std::string_view label, std::string_view detail) const;
  void ClampSelection();

  bool visible_ = false;
  SettingsOverlayMode mode_ = SettingsOverlayMode::Settings;
  int scroll_row_ = 0;
  int category_scroll_row_ = 0;
  std::string query_;
  editor::SingleLineEditor query_editor_;
  editor::SingleLineEditor value_editor_;
  bool editing_value_ = false;
  bool editing_fonts_ = false;
  std::string editing_row_id_;
  std::vector<std::string> font_families_;
  mutable std::vector<std::string_view> filtered_font_cache_;
  mutable std::string filtered_font_cache_query_;
  mutable bool filtered_font_cache_valid_ = false;
  int picker_highlight_ = -1;
  int picker_scroll_ = 0;
  std::vector<SettingsOverlayRow> settings_rows_;
  std::vector<HelpAboutRow> help_rows_;
  std::size_t settings_rows_total_ = 0;
  std::size_t help_rows_total_ = 0;
  std::vector<std::string> categories_;
  std::vector<std::vector<int>> category_row_indices_;
  std::vector<std::pair<std::string_view, const std::string*>> user_layer_index_;
  std::vector<std::pair<std::string_view, const std::string*>> project_layer_index_;
  std::vector<std::pair<std::string_view, int>> category_index_;
  int selected_category_ = 0;
  int selected_row_ = 0;
  SettingsPane focused_pane_ = SettingsPane::Filter;
};

}
