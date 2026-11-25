#include "workspace/services/SettingsOverlayService.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <system_error>

#include "util/Parse.h"
#include "util/StringUtil.h"
#include "workspace/persistence/SettingsStore.h"

namespace microide::workspace {

namespace {

void CompactFloatInto(std::string_view value, std::string* out) {
  const auto parsed = util::ParseFloat(value);
  if (!parsed.has_value()) {
    out->assign(value);
    return;
  }
  char buffer[24];
  const auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), *parsed);
  if (ec != std::errc{}) {
    out->assign(value);
    return;
  }
  out->assign(buffer, static_cast<std::size_t>(end - buffer));
}

template <typename Layer, typename Payload>
void BuildSortedIndex(const Layer& layer,
                      std::vector<std::pair<std::string_view, Payload>>* index,
                      auto&& key_of,
                      auto&& payload_of) {
  index->clear();
  index->reserve(layer.size());
  for (const auto& entry : layer) {
    index->emplace_back(key_of(entry), payload_of(entry));
  }
  std::sort(index->begin(), index->end(), [](const auto& a, const auto& b) {
    return a.first != b.first ? a.first < b.first : std::greater<Payload>{}(a.second, b.second);
  });
}

template <typename Payload>
Payload LookupSortedIndex(const std::vector<std::pair<std::string_view, Payload>>& index,
                          std::string_view key,
                          Payload missing) {
  const auto it = std::lower_bound(
      index.begin(), index.end(), key,
      [](const auto& entry, std::string_view value) { return entry.first < value; });
  return it != index.end() && it->first == key ? it->second : missing;
}

}

std::string_view SettingsCategoryLabel(std::string_view group) {
  if (group.empty()) {
    return "General";
  }
  const std::size_t arrow = group.find(" → ");
  return arrow == std::string_view::npos ? group : group.substr(0, arrow);
}

void SettingsOverlayService::OpenSettings() {
  visible_ = true;
  mode_ = SettingsOverlayMode::Settings;
  scroll_row_ = 0;
  category_scroll_row_ = 0;
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
  focused_pane_ = SettingsPane::Filter;
  query_.clear();
  query_editor_.SetText("");
  CancelValueEdit();
}

void SettingsOverlayService::Close() {
  visible_ = false;
  scroll_row_ = 0;
  category_scroll_row_ = 0;
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
  filtered_font_cache_valid_ = false;
  picker_highlight_ = -1;
}

std::string SettingsOverlayService::ValueEditText() const { return value_editor_.text(); }

void SettingsOverlayService::BeginFontValueEdit(std::string row_id,
                                                std::vector<std::string> families) {
  editing_value_ = true;
  editing_fonts_ = true;
  editing_row_id_ = std::move(row_id);
  value_editor_.SetText("");
  font_families_ = std::move(families);
  constexpr std::size_t kMaxFontFamilies = 100000;
  if (font_families_.size() > kMaxFontFamilies) {
    font_families_.resize(kMaxFontFamilies);
  }
  filtered_font_cache_valid_ = false;
  picker_highlight_ = -1;
  picker_scroll_ = 0;
}

const std::vector<std::string_view>& SettingsOverlayService::FilteredFontFamilies() const {
  const std::string query = value_editor_.text();
  if (filtered_font_cache_valid_ && filtered_font_cache_query_ == query) {
    return filtered_font_cache_;
  }
  filtered_font_cache_.clear();
  filtered_font_cache_.reserve(font_families_.size());
  for (const std::string& family : font_families_) {
    if (query.empty() || util::ContainsCaseInsensitiveAscii(family, query)) {
      filtered_font_cache_.emplace_back(family);
    }
  }
  filtered_font_cache_query_ = query;
  filtered_font_cache_valid_ = true;
  return filtered_font_cache_;
}

int SettingsOverlayService::PickerRowCount() const {
  return static_cast<int>(FilteredFontFamilies().size()) + 1;
}

int SettingsOverlayService::PickerChooseFileIndex() const {
  return static_cast<int>(FilteredFontFamilies().size());
}

void SettingsOverlayService::SetPickerHighlight(int index) {
  const int family_count = static_cast<int>(FilteredFontFamilies().size());
  const int max_index = family_count;
  picker_highlight_ = std::clamp(index, -1, std::max(-1, max_index));
  if (picker_highlight_ >= 0 && picker_highlight_ < family_count) {
    if (picker_highlight_ < picker_scroll_) {
      picker_scroll_ = picker_highlight_;
    } else if (picker_highlight_ >= picker_scroll_ + kPickerVisibleFamilies) {
      picker_scroll_ = picker_highlight_ - kPickerVisibleFamilies + 1;
    }
  }
  SetPickerScroll(picker_scroll_);
}

void SettingsOverlayService::MovePickerHighlight(int delta) {
  SetPickerHighlight(picker_highlight_ + delta);
}

void SettingsOverlayService::ResetPickerHighlight() {
  picker_highlight_ = -1;
  picker_scroll_ = 0;
}

void SettingsOverlayService::SetPickerScroll(int top) {
  const int family_count = static_cast<int>(FilteredFontFamilies().size());
  const int max_top = std::max(0, family_count - kPickerVisibleFamilies);
  picker_scroll_ = std::clamp(top, 0, max_top);
}

void SettingsOverlayService::SetScrollRow(int row) {
  scroll_row_ = std::max(0, row);
}

void SettingsOverlayService::SetCategoryScrollRow(int row) {
  category_scroll_row_ = std::max(0, row);
}

void SettingsOverlayService::SetQuery(std::string query) {
  query_ = std::move(query);
  scroll_row_ = 0;
  category_scroll_row_ = 0;
}

void SettingsOverlayService::SyncQueryFromEditor() {
  query_ = query_editor_.text();
  scroll_row_ = 0;
  category_scroll_row_ = 0;
}

void SettingsOverlayService::RebuildSettingsRows(
    const std::vector<SettingInfo>& settings,
    const std::vector<std::pair<std::string, std::string>>& user_settings,
    const std::vector<std::pair<std::string, std::string>>& project_settings,
    const std::vector<SettingsOverlayRow>& extra_rows) {
  settings_rows_total_ = settings.size() + extra_rows.size();
  const auto layer_key = [](const std::pair<std::string, std::string>& entry) {
    return std::string_view(entry.first);
  };
  const auto layer_value = [](const std::pair<std::string, std::string>& entry) {
    return &entry.second;
  };
  BuildSortedIndex(user_settings, &user_layer_index_, layer_key, layer_value);
  BuildSortedIndex(project_settings, &project_layer_index_, layer_key, layer_value);

  std::size_t out = 0;
  const auto next_row = [&]() -> SettingsOverlayRow& {
    if (out == settings_rows_.size()) {
      settings_rows_.emplace_back();
    }
    return settings_rows_[out++];
  };
  for (const SettingInfo& setting : settings) {
    if (!RowMatchesQuery(setting.label, setting.id)) {
      continue;
    }
    const std::string* user_stored =
        LookupSortedIndex<const std::string*>(user_layer_index_, setting.id, nullptr);
    const std::string* project_stored =
        LookupSortedIndex<const std::string*>(project_layer_index_, setting.id, nullptr);
    const bool scope_selectable =
        setting.plugin_id.empty() && setting.scope == SettingScope::Project;
    const std::string* active_stored =
        setting.scope == SettingScope::User ? user_stored
                                            : (project_stored != nullptr ? project_stored
                                                                         : user_stored);
    SettingsOverlayRow& row = next_row();
    row.id.assign(setting.id);
    row.label.assign(setting.label);
    if (active_stored != nullptr) {
      row.value.assign(*active_stored);
    } else {
      SerializeSettingValueInto(setting.default_value, &row.value);
    }
    if (setting.type == SettingType::Float) {
      CompactFloatInto(row.value, &row.value_display);
    } else {
      row.value_display.assign(row.value);
    }
    row.description.assign(setting.description);
    row.scope_selectable = scope_selectable;
    row.project_override = project_stored != nullptr;
    row.has_user_default = user_stored != nullptr;
    if (setting.scope == SettingScope::User) {
      row.scope_label.assign("User");
    } else {
      row.scope_label.assign(project_stored != nullptr ? "Project" : "Default");
    }
    row.detail.assign(setting.scope == SettingScope::User ? "User / " : "Project / ");
    if (setting.plugin_id.empty()) {
      row.detail.append("built-in");
    } else {
      row.detail.append("plugin:").append(setting.plugin_id);
    }
    row.group.assign(setting.group);
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
    row.resettable = active_stored != nullptr;
    row.editable = true;
  }

  for (const SettingsOverlayRow& extra : extra_rows) {
    if (RowMatchesQuery(extra.label, extra.id)) {
      next_row() = extra;
    }
  }
  settings_rows_.resize(out);

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

  category_index_.clear();
  category_index_.reserve(categories_.size());
  for (int c = 0; c < static_cast<int>(categories_.size()); ++c) {
    category_index_.emplace_back(std::string_view(categories_[static_cast<std::size_t>(c)]), c);
  }
  std::sort(category_index_.begin(), category_index_.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  category_row_indices_.resize(categories_.size());
  for (std::vector<int>& rows : category_row_indices_) {
    rows.clear();
  }
  for (int i = 0; i < static_cast<int>(settings_rows_.size()); ++i) {
    const std::string_view label = SettingsCategoryLabel(settings_rows_[static_cast<std::size_t>(i)].group);
    const int category = LookupSortedIndex<int>(category_index_, label, -1);
    if (category >= 0) {
      category_row_indices_[static_cast<std::size_t>(category)].push_back(i);
    }
  }

  ClampSelection();
}

void SettingsOverlayService::RebuildHelpRows(std::vector<HelpAboutRow> rows) {
  help_rows_total_ = rows.size();
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

std::size_t SettingsOverlayService::TotalRowCount() const {
  switch (mode_) {
    case SettingsOverlayMode::Settings:
      return settings_rows_total_;
    case SettingsOverlayMode::HelpAbout:
      return help_rows_total_;
  }
  return 0;
}

bool SettingsOverlayService::RowMatchesQuery(std::string_view label, std::string_view detail) const {
  if (query_.empty()) {
    return true;
  }
  return util::ContainsCaseInsensitiveAscii(label, query_) ||
         util::ContainsCaseInsensitiveAscii(detail, query_);
}

std::size_t SettingsOverlayService::RowCountInCategory(int category) const {
  if (category < 0 || category >= static_cast<int>(category_row_indices_.size())) {
    return 0;
  }
  return category_row_indices_[static_cast<std::size_t>(category)].size();
}

std::size_t SettingsOverlayService::RowCountInSelectedCategory() const {
  return RowCountInCategory(selected_category_);
}

const SettingsOverlayRow* SettingsOverlayService::RowAtVisibleIndex(int category,
                                                                    int row_in_category) const {
  if (category < 0 || category >= static_cast<int>(category_row_indices_.size()) ||
      row_in_category < 0) {
    return nullptr;
  }
  const std::vector<int>& indices = category_row_indices_[static_cast<std::size_t>(category)];
  if (row_in_category >= static_cast<int>(indices.size())) {
    return nullptr;
  }
  return &settings_rows_[static_cast<std::size_t>(indices[static_cast<std::size_t>(row_in_category)])];
}

const SettingsOverlayRow* SettingsOverlayService::SelectedSettingRow() const {
  return RowAtVisibleIndex(selected_category_, selected_row_);
}

void SettingsOverlayService::ClampSelection() {
  if (categories_.empty()) {
    selected_category_ = 0;
    selected_row_ = 0;
    category_scroll_row_ = 0;
    return;
  }
  selected_category_ = std::clamp(selected_category_, 0, static_cast<int>(categories_.size()) - 1);
  category_scroll_row_ =
      std::clamp(category_scroll_row_, 0, static_cast<int>(categories_.size()) - 1);
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
  constexpr int kPaneCount = static_cast<int>(SettingsPane::Count);
  int pane = static_cast<int>(focused_pane_) + delta;
  pane = ((pane % kPaneCount) + kPaneCount) % kPaneCount;
  focused_pane_ = static_cast<SettingsPane>(pane);
}

}
