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
  None,       // display-only
  Checkbox,   // Bool
  Stepper,    // Int / Float / large Enum: [◀] value [▶]
  Segmented,  // small Enum: a value box that cycles on click
  TextEdit,   // String: a value box that opens an inline text editor on click
};

// Which of the two-pane surface's focusable regions currently owns the keyboard.
enum class SettingsPane {
  Filter,
  Categories,
  Values,
  // Sentinel: number of focusable panes. Keep last. CycleFocusedPane derives the
  // wrap modulus from this so adding/removing a pane cannot desync keyboard
  // navigation (TD-2026-07-17-028).
  Count,
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
  // "Set as default" support (built-in project-scoped settings only). When
  // scope_selectable is true the row shows a per-row scope chip toggling the
  // write target between "This Project" (project layer) and "Default" (user
  // layer, applies to every project without its own override).
  bool scope_selectable = false;
  bool project_override = false;  // the project layer holds an explicit override
  bool has_user_default = false;  // the user layer holds a cross-project default
  bool suggests_fonts = false;    // String row rendered as a font picker
};

struct HelpAboutRow {
  std::string label;
  std::string detail;
};

class SettingsOverlayService {
 public:
  // Number of family rows the font-picker dropdown shows at once; the list scrolls
  // (wheel / scrollbar / keyboard keep-visible) when more families match. Single
  // source of truth shared with the view-model builder's picker window.
  static constexpr int kPickerVisibleFamilies = 10;

  void OpenSettings();
  void OpenHelpAbout();
  void Close();

  bool Visible() const { return visible_; }
  SettingsOverlayMode Mode() const { return mode_; }
  int ScrollRow() const { return scroll_row_; }
  void SetScrollRow(int row);
  // Whole-row scroll offset of the left-rail category list. The category count can
  // exceed the pane height, so the rail scrolls (wheel / scrollbar / keyboard
  // keep-visible) just like the right-pane rows. The builder clamps for display.
  int CategoryScrollRow() const { return category_scroll_row_; }
  void SetCategoryScrollRow(int row);
  const std::string& Query() const { return query_; }
  void SetQuery(std::string query);

  // Live filter input. The text-input coordinator types into this editor; on
  // change the host calls SyncQueryFromEditor() then RebuildSettingsRows().
  editor::SingleLineEditor& QueryEditor() { return query_editor_; }
  const editor::SingleLineEditor& QueryEditor() const { return query_editor_; }
  void SyncQueryFromEditor();

  // Inline value editing for String settings. While a value edit is active the
  // text-input coordinator routes typing into value_editor_; the host commits on
  // Return (writing the setting) and cancels on Esc / focus loss.
  editor::SingleLineEditor& ValueEditor() { return value_editor_; }
  const editor::SingleLineEditor& ValueEditor() const { return value_editor_; }
  bool EditingValue() const { return editing_value_; }
  const std::string& EditingRowId() const { return editing_row_id_; }
  void BeginValueEdit(std::string row_id, const std::string& initial_text);
  void CancelValueEdit();
  // The current editor text, for the host to commit through SetSettingValue.
  std::string ValueEditText() const;

  // --- Font picker (a specialization of value editing for font-family rows) ---
  // Begins a font value edit: an empty search field (value_editor_) plus a
  // dropdown of the supplied installed families. Typing filters; the host commits
  // the highlighted family / typed text / "Choose file…" entry.
  void BeginFontValueEdit(std::string row_id, std::vector<std::string> families);
  bool EditingFonts() const { return editing_fonts_; }
  // Families matching the current search text (case-insensitive substring, all when
  // empty). Views point into the service-owned family list, valid until the next
  // edit begins.
  const std::vector<std::string_view>& FilteredFontFamilies() const;
  // Dropdown rows are the filtered families followed by one "Choose file…" entry.
  int PickerRowCount() const;          // filtered families + 1
  int PickerChooseFileIndex() const;   // == filtered family count
  // Active dropdown selection: -1 = search-only (commit types text), [0..F-1] =
  // a family, F = the "Choose file…" entry.
  int PickerHighlight() const { return picker_highlight_; }
  void SetPickerHighlight(int index);
  void MovePickerHighlight(int delta);
  void ResetPickerHighlight();
  // Index of the topmost visible family row in the scrolling dropdown window.
  // The wheel and scrollbar drive it directly; keyboard highlight keeps it in view.
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
  int category_scroll_row_ = 0;
  std::string query_;
  editor::SingleLineEditor query_editor_;
  editor::SingleLineEditor value_editor_;
  bool editing_value_ = false;
  bool editing_fonts_ = false;
  std::string editing_row_id_;
  std::vector<std::string> font_families_;
  // Memoizes FilteredFontFamilies() — row-count / highlight / scroll all call it
  // per interaction, and the family list only changes when the search text or the
  // installed-family set does. Views point into font_families_ (stable until the
  // next edit begins). Mutable so const query accessors can fill the cache.
  mutable std::vector<std::string_view> filtered_font_cache_;
  mutable std::string filtered_font_cache_query_;
  mutable bool filtered_font_cache_valid_ = false;
  int picker_highlight_ = -1;
  int picker_scroll_ = 0;
  std::vector<SettingsOverlayRow> settings_rows_;
  std::vector<HelpAboutRow> help_rows_;
  std::vector<std::string> categories_;
  int selected_category_ = 0;
  int selected_row_ = 0;
  SettingsPane focused_pane_ = SettingsPane::Filter;
};

}  // namespace microide::workspace
