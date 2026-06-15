#include "workspace/SettingsOverlayService.h"

#include <algorithm>
#include <cctype>

#include "util/Parse.h"
#include "util/StringUtil.h"

namespace microide::workspace {

namespace {

const std::string* FindStoredValue(const std::vector<std::pair<std::string, std::string>>& values,
                                   std::string_view id) {
  const auto it = std::find_if(values.begin(), values.end(),
                               [id](const auto& entry) { return entry.first == id; });
  return it == values.end() ? nullptr : &it->second;
}

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
}

void SettingsOverlayService::OpenHelpAbout() {
  visible_ = true;
  mode_ = SettingsOverlayMode::HelpAbout;
  scroll_row_ = 0;
  // Help / About has no filter input; clear any leftover Settings-mode query so
  // its rows are never silently filtered by a stale needle.
  query_.clear();
  query_editor_.SetText("");
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
    const std::string* stored = setting.scope == SettingScope::User
                                    ? FindStoredValue(user_settings, setting.id)
                                    : FindStoredValue(project_settings, setting.id);
    SettingsOverlayRow row;
    row.id = setting.id;
    row.label = setting.label;
    row.value = stored != nullptr ? *stored : SerializeSettingValue(setting.default_value);
    row.value_display = setting.type == SettingType::Float ? CompactFloat(row.value) : row.value;
    row.description = setting.description;
    row.scope_label = setting.scope == SettingScope::User ? "User" : "Project";
    row.detail = setting.scope == SettingScope::User ? "User / " : "Project / ";
    row.detail += setting.plugin_id.empty() ? "built-in" : "plugin:" + setting.plugin_id;
    row.group = setting.group;
    row.type = setting.type;
    row.scope = setting.scope;
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
        row.control_kind = SettingsControlKind::None;
        break;
    }
    row.resettable = stored != nullptr;
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
