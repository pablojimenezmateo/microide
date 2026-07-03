#include "workspace/SettingsOverlayService.h"

#include <algorithm>
#include <cctype>

#include "util/Parse.h"
#include "util/StringUtil.h"
#include "workspace/SettingsStore.h"

namespace microide::workspace {

namespace {

// Renders a float setting value without std::to_string's trailing zeros so the
// overlay shows "1" / "1.25" instead of "1.000000".
std::string CompactFloat(std::string_view value) {
  const auto parsed = util::ParseFloat(value);
  if (!parsed.has_value()) {
    return std::string(value);
  }
  std::string text = std::to_string(*parsed);
  if (text.find('.') != std::string::npos) {
    while (!text.empty() && text.back() == '0') {
      text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
      text.pop_back();
    }
  }
  return text.empty() ? "0" : text;
}

}  // namespace

std::string_view SettingsCategoryLabel(std::string_view group) {
  if (group.empty()) {
    return "General";
  }
  const std::size_t arrow = group.find(" → ");  // "Group → Subsection"
  return arrow == std::string_view::npos ? group : group.substr(0, arrow);
}

void SettingsOverlayService::OpenSettings() {
  visible_ = true;
  mode_ = SettingsOverlayMode::Settings;
  scroll_row_ = 0;
  selected_category_ = 0;
  selected_row_ = 0;
  focused_pane_ = SettingsPane::Filter;
  query_.clear();
  query_editor_.SetText("");
  CancelValueEdit();
}

void SettingsOverlayService::OpenHelpAbout() {
  visible_ = true;
  mode_ = SettingsOverlayMode::HelpAbout;
  scroll_row_ = 0;
  // Help / About has no filter input; clear any leftover Settings-mode query so
  // its rows are never silently filtered by a stale needle.
  query_.clear();
  query_editor_.SetText("");
  CancelValueEdit();
}

void SettingsOverlayService::Close() {
  visible_ = false;
  scroll_row_ = 0;
  CancelValueEdit();
}

void SettingsOverlayService::BeginValueEdit(std::string row_id, const std::string& initial_text) {
  editing_value_ = true;
  editing_fonts_ = false;
  editing_row_id_ = std::move(row_id);
  value_editor_.SetText(initial_text);
  value_editor_.SelectAll();
}

void SettingsOverlayService::CancelValueEdit() {
  editing_value_ = false;
  editing_fonts_ = false;
  editing_row_id_.clear();
  value_editor_.SetText("");
  font_families_.clear();
  picker_highlight_ = -1;
}

std::string SettingsOverlayService::ValueEditText() const { return value_editor_.text(); }

void SettingsOverlayService::BeginFontValueEdit(std::string row_id,
                                                std::vector<std::string> families) {
  editing_value_ = true;
  editing_fonts_ = true;
  editing_row_id_ = std::move(row_id);
  // Empty search field: the dropdown starts showing every family and narrows as
  // the user types. Highlight starts at -1 so a stray Enter never commits a font.
  value_editor_.SetText("");
  font_families_ = std::move(families);
  picker_highlight_ = -1;
}

std::vector<std::string_view> SettingsOverlayService::FilteredFontFamilies() const {
  std::vector<std::string_view> out;
  out.reserve(font_families_.size());
  const std::string query = value_editor_.text();
  for (const std::string& family : font_families_) {
    if (query.empty() || util::ContainsCaseInsensitiveAscii(family, query)) {
      out.emplace_back(family);
    }
  }
  return out;
}

int SettingsOverlayService::PickerRowCount() const {
  return static_cast<int>(FilteredFontFamilies().size()) + 1;
}

int SettingsOverlayService::PickerChooseFileIndex() const {
  return static_cast<int>(FilteredFontFamilies().size());
}

void SettingsOverlayService::SetPickerHighlight(int index) {
  const int max_index = PickerRowCount() - 1;  // the "Choose file…" entry
  picker_highlight_ = std::clamp(index, -1, std::max(-1, max_index));
}

void SettingsOverlayService::MovePickerHighlight(int delta) {
  SetPickerHighlight(picker_highlight_ + delta);
}

void SettingsOverlayService::ResetPickerHighlight() { picker_highlight_ = -1; }

void SettingsOverlayService::SetScrollRow(int row) {
  scroll_row_ = std::max(0, row);
}

void SettingsOverlayService::SetQuery(std::string query) {
  query_ = std::move(query);
  scroll_row_ = 0;
}

void SettingsOverlayService::SyncQueryFromEditor() {
  query_ = query_editor_.text();
  scroll_row_ = 0;
}

void SettingsOverlayService::RebuildSettingsRows(
    const std::vector<SettingInfo>& settings,
    const std::vector<std::pair<std::string, std::string>>& user_settings,
    const std::vector<std::pair<std::string, std::string>>& project_settings,
    const std::vector<SettingsOverlayRow>& extra_rows) {
  settings_rows_.clear();
  settings_rows_.reserve(settings.size());
  for (const SettingInfo& setting : settings) {
    if (!RowMatchesQuery(setting.label, setting.id)) {
      continue;
    }
    const std::string* user_stored = settings_layer::Find(user_settings, setting.id);
    const std::string* project_stored = settings_layer::Find(project_settings, setting.id);
    // Built-in project-scoped settings support a user-level default that a
    // per-project override wins over (project → user default → spec default).
    const bool scope_selectable =
        setting.plugin_id.empty() && setting.scope == SettingScope::Project;
    const std::string* active_stored =
        setting.scope == SettingScope::User ? user_stored
                                            : (project_stored != nullptr ? project_stored
                                                                         : user_stored);
    SettingsOverlayRow row;
    row.id = setting.id;
    row.label = setting.label;
    row.value =
        active_stored != nullptr ? *active_stored : SerializeSettingValue(setting.default_value);
    row.value_display = setting.type == SettingType::Float ? CompactFloat(row.value) : row.value;
    row.description = setting.description;
    row.scope_selectable = scope_selectable;
    row.project_override = project_stored != nullptr;
    row.has_user_default = user_stored != nullptr;
    // The scope label reflects where the active value lives: a project override
    // reads "Project"; otherwise a project-scoped setting reads "Default".
    if (setting.scope == SettingScope::User) {
      row.scope_label = "User";
    } else {
      row.scope_label = project_stored != nullptr ? "Project" : "Default";
    }
    row.detail = setting.scope == SettingScope::User ? "User / " : "Project / ";
    row.detail += setting.plugin_id.empty() ? "built-in" : "plugin:" + setting.plugin_id;
    row.group = setting.group;
    row.type = setting.type;
    row.scope = setting.scope;
    row.suggests_fonts = setting.suggests_fonts;
    switch (setting.type) {
      case SettingType::Bool:
        row.control_kind = SettingsControlKind::Checkbox;
        break;
      case SettingType::Enum:
        row.control_kind = setting.enum_values.size() <= 4 ? SettingsControlKind::Segmented
                                                           : SettingsControlKind::Stepper;
        break;
      case SettingType::Int:
      case SettingType::Float:
        row.control_kind = SettingsControlKind::Stepper;
        break;
      case SettingType::String:
        row.control_kind = SettingsControlKind::TextEdit;
        break;
    }
    // Resettable when the active layer holds an override (project override when
    // present, else a user-level default).
    row.resettable = active_stored != nullptr;
    row.editable = true;
    settings_rows_.push_back(std::move(row));
  }

  // Append host-built extra rows (e.g. the per-plugin enable toggles), honoring the
  // same query filter so they show up in their own category alongside settings.
  for (const SettingsOverlayRow& row : extra_rows) {
    if (RowMatchesQuery(row.label, row.id)) {
      settings_rows_.push_back(row);
    }
  }

  // Derive the left-pane category list from the filtered rows: "General" first
  // (when any ungrouped row survives the filter), then each distinct top-level
  // group segment in first-seen order. Empty categories never appear.
  categories_.clear();
  bool has_general = false;
  for (const SettingsOverlayRow& row : settings_rows_) {
    if (SettingsCategoryLabel(row.group) == "General") {
      has_general = true;
      break;
    }
  }
  if (has_general) {
    categories_.emplace_back("General");
  }
  for (const SettingsOverlayRow& row : settings_rows_) {
    const std::string_view label = SettingsCategoryLabel(row.group);
    if (label == "General") {
      continue;
    }
    if (std::none_of(categories_.begin(), categories_.end(),
                     [label](const std::string& existing) { return existing == label; })) {
      categories_.emplace_back(label);
    }
  }
  ClampSelection();
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

bool SettingsOverlayService::RowInCategory(const SettingsOverlayRow& row, int category) const {
  if (category < 0 || category >= static_cast<int>(categories_.size())) {
    return false;
  }
  return SettingsCategoryLabel(row.group) == categories_[static_cast<std::size_t>(category)];
}

std::size_t SettingsOverlayService::RowCountInCategory(int category) const {
  std::size_t count = 0;
  for (const SettingsOverlayRow& row : settings_rows_) {
    if (RowInCategory(row, category)) {
      ++count;
    }
  }
  return count;
}

std::size_t SettingsOverlayService::RowCountInSelectedCategory() const {
  return RowCountInCategory(selected_category_);
}

const SettingsOverlayRow* SettingsOverlayService::RowAtVisibleIndex(int category,
                                                                    int row_in_category) const {
  if (row_in_category < 0) {
    return nullptr;
  }
  int seen = 0;
  for (const SettingsOverlayRow& row : settings_rows_) {
    if (!RowInCategory(row, category)) {
      continue;
    }
    if (seen == row_in_category) {
      return &row;
    }
    ++seen;
  }
  return nullptr;
}

const SettingsOverlayRow* SettingsOverlayService::SelectedSettingRow() const {
  return RowAtVisibleIndex(selected_category_, selected_row_);
}

void SettingsOverlayService::ClampSelection() {
  if (categories_.empty()) {
    selected_category_ = 0;
    selected_row_ = 0;
    return;
  }
  selected_category_ = std::clamp(selected_category_, 0, static_cast<int>(categories_.size()) - 1);
  const int count = static_cast<int>(RowCountInSelectedCategory());
  if (count <= 0) {
    selected_row_ = 0;
  } else {
    selected_row_ = std::clamp(selected_row_, 0, count - 1);
  }
}

void SettingsOverlayService::SetSelectedCategory(int category) {
  if (categories_.empty()) {
    selected_category_ = 0;
    selected_row_ = 0;
    scroll_row_ = 0;
    return;
  }
  selected_category_ = std::clamp(category, 0, static_cast<int>(categories_.size()) - 1);
  selected_row_ = 0;
  scroll_row_ = 0;
}

void SettingsOverlayService::SetSelectedRow(int row) {
  const int count = static_cast<int>(RowCountInSelectedCategory());
  selected_row_ = count <= 0 ? 0 : std::clamp(row, 0, count - 1);
}

void SettingsOverlayService::MoveCategory(int delta) {
  if (categories_.empty()) {
    return;
  }
  SetSelectedCategory(selected_category_ + delta);
}

void SettingsOverlayService::MoveRow(int delta) {
  SetSelectedRow(selected_row_ + delta);
}

void SettingsOverlayService::CycleFocusedPane(int delta) {
  constexpr int kPaneCount = 3;
  int pane = static_cast<int>(focused_pane_) + delta;
  pane = ((pane % kPaneCount) + kPaneCount) % kPaneCount;
  focused_pane_ = static_cast<SettingsPane>(pane);
}

}  // namespace microide::workspace
