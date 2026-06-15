#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "editor/SingleLineEditor.h"
#include "workspace/WorkspaceSettingsRegistry.h"

namespace microide::workspace {

enum class SettingsOverlayMode {
  Settings,
  HelpAbout,
};

// Inline control drawn for a settings row, chosen from the setting's type.
enum class SettingsControlKind {
  None,       // display-only (e.g. free-form String)
  Checkbox,   // Bool
  Stepper,    // Int / Float / large Enum: [◀] value [▶]
  Segmented,  // small Enum: a value box that cycles on click
};

// Which of the two-pane surface's focusable regions currently owns the keyboard.
enum class SettingsPane {
  Filter,
  Categories,
  Values,
};

// Returns the top-level category label for a setting group path. An empty group
// maps to "General"; "Editor → Foo" maps to "Editor". The returned view points
// into `group` (for the top-level segment) or at a static literal ("General").
std::string_view SettingsCategoryLabel(std::string_view group);

struct SettingsOverlayRow {
  std::string id;
  std::string label;
  std::string value;          // canonical stored value, used for stepping
  std::string value_display;  // value text to draw (Float compacted, else == value)
  std::string description;     // setting help text (from the spec)
  std::string detail;
  std::string scope_label;     // "User" / "Project"
  std::string group;  // path like "Editor → Essentials → Block Structure"
  SettingType type = SettingType::String;
  SettingScope scope = SettingScope::Project;
  SettingsControlKind control_kind = SettingsControlKind::None;
  bool resettable = false;
  bool editable = false;
};

struct HelpAboutRow {
  std::string label;
  std::string detail;
};

class SettingsOverlayService {
 public:
  void OpenSettings();
  void OpenHelpAbout();
  void Close();

  bool Visible() const { return visible_; }
  SettingsOverlayMode Mode() const { return mode_; }
  int ScrollRow() const { return scroll_row_; }
  void SetScrollRow(int row);
  const std::string& Query() const { return query_; }
  void SetQuery(std::string query);

  // Live filter input. The text-input coordinator types into this editor; on
  // change the host calls SyncQueryFromEditor() then RebuildSettingsRows().
  editor::SingleLineEditor& QueryEditor() { return query_editor_; }
  const editor::SingleLineEditor& QueryEditor() const { return query_editor_; }
  void SyncQueryFromEditor();

  void RebuildSettingsRows(const std::vector<SettingInfo>& settings,
                           const std::vector<std::pair<std::string, std::string>>& user_settings,
                           const std::vector<std::pair<std::string, std::string>>& project_settings,
                           const std::vector<SettingsOverlayRow>& extra_rows = {});
  void RebuildHelpRows(std::vector<HelpAboutRow> rows);

  const std::vector<SettingsOverlayRow>& SettingsRows() const { return settings_rows_; }
  const std::vector<HelpAboutRow>& HelpRows() const { return help_rows_; }
  std::size_t VisibleRowCount() const;

  // --- Two-pane navigation state ---
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

  // Number of rows belonging to the selected category, and resolution of a row
  // by (category, row-within-category). RowAtVisibleIndex returns nullptr when
  // out of range.
  std::size_t RowCountInSelectedCategory() const;
  std::size_t RowCountInCategory(int category) const;
  const SettingsOverlayRow* RowAtVisibleIndex(int category, int row_in_category) const;
  // The currently selected setting row (selected category + selected row), or nullptr.
  const SettingsOverlayRow* SelectedSettingRow() const;

 private:
  bool RowMatchesQuery(std::string_view label, std::string_view detail) const;
  bool RowInCategory(const SettingsOverlayRow& row, int category) const;
  void ClampSelection();

  bool visible_ = false;
  SettingsOverlayMode mode_ = SettingsOverlayMode::Settings;
  int scroll_row_ = 0;
  std::string query_;
  editor::SingleLineEditor query_editor_;
  std::vector<SettingsOverlayRow> settings_rows_;
  std::vector<HelpAboutRow> help_rows_;
  std::vector<std::string> categories_;
  int selected_category_ = 0;
  int selected_row_ = 0;
  SettingsPane focused_pane_ = SettingsPane::Filter;
};

}  // namespace microide::workspace
