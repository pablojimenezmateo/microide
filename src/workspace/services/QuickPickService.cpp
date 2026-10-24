#include "workspace/services/QuickPickService.h"

#include <algorithm>
#include <utility>

#include "util/StringUtil.h"
#include "workspace/WorkspaceUiText.h"
#include "workspace/debug/LaunchConfig.h"

namespace microide::workspace {

void QuickPickService::Configure(QuickPickState& state, Operations operations) {
  state_ = &state;
  operations_ = std::move(operations);
}

void QuickPickService::Open(QuickPickState picker) {
  if (state_ == nullptr) {
    return;
  }
  // A pick replacing one still previewing hands back what that one changed.
  Cancel();
  // The selection the builder chose (the active theme) survives the first
  // filter, which an empty query leaves in item order.
  const std::size_t selected = picker.selected_index;
  *state_ = std::move(picker);
  Refilter();
  state_->selected_index = std::min(selected, state_->matches.empty() ? 0 : state_->matches.size() - 1);
  if (operations_.show_overlay) {
    operations_.show_overlay();
  }
}

void QuickPickService::Refilter() {
  if (state_ == nullptr) {
    return;
  }
  QuickPickState& picker = *state_;
  picker.matches.clear();
  picker.selected_index = 0;
  const std::string query = util::ToLowerAscii(picker.query.text());
  // An exact match first, then the items the query starts, then the rest that
  // hold it, each in item order: `light` must not sit under `dukelight-tc`.
  for (int rank = 0; rank < 3; ++rank) {
    for (const QuickPickItem& item : picker.items) {
      const std::size_t at = query.empty() ? 0 : item.search_text.find(query);
      if (at == std::string::npos) {
        continue;
      }
      const std::string_view primary = std::string_view(item.search_text).substr(0, item.primary_label.size());
      const int item_rank = query.empty() || primary == query ? 0 : at == 0 ? 1 : 2;
      if (item_rank == rank) {
        picker.matches.push_back(item);
      }
    }
  }
  picker.summary_line = BuildFilteredCountSummary(picker.matches.size(), picker.items.size(), picker.noun);
  if (operations_.list_changed) {
    operations_.list_changed();
  }
  // Typing a filter previews its top match, as VS Code's theme pick does; an
  // empty query (the open) keeps what is in use.
  if (!query.empty()) {
    Preview();
  }
}

void QuickPickService::SelectionChanged() { Preview(); }

void QuickPickService::Preview() {
  if (state_ == nullptr || state_->selected_index >= state_->matches.size()) {
    return;
  }
  const QuickPickItem& item = state_->matches[state_->selected_index];
  switch (state_->kind) {
    case QuickPickKind::ColorTheme:
      if (operations_.apply_colorscheme) {
        operations_.apply_colorscheme(item.value, /*persist=*/false);
      }
      break;
    case QuickPickKind::LaunchConfig:
      break;
  }
}

void QuickPickService::Accept() {
  if (state_ == nullptr || state_->selected_index >= state_->matches.size()) {
    return;
  }
  const QuickPickItem item = state_->matches[state_->selected_index];
  state_->revert_value.clear();
  switch (state_->kind) {
    case QuickPickKind::LaunchConfig:
      if (operations_.start_launch_config) {
        operations_.start_launch_config(item.index);
      }
      break;
    case QuickPickKind::ColorTheme:
      if (operations_.apply_colorscheme) {
        operations_.apply_colorscheme(item.value, /*persist=*/true);
      }
      break;
  }
}

void QuickPickService::Cancel() {
  if (state_ == nullptr || state_->revert_value.empty()) {
    return;
  }
  const std::string revert = std::exchange(state_->revert_value, {});
  switch (state_->kind) {
    case QuickPickKind::ColorTheme:
      if (operations_.apply_colorscheme) {
        operations_.apply_colorscheme(revert, /*persist=*/false);
      }
      break;
    case QuickPickKind::LaunchConfig:
      break;
  }
}

QuickPickState QuickPickService::LaunchConfigs(std::span<const LaunchConfig> configs) {
  QuickPickState picker;
  picker.kind = QuickPickKind::LaunchConfig;
  picker.title = "Select Launch Configuration";
  picker.noun = "configurations";
  picker.items.reserve(configs.size());
  for (std::size_t i = 0; i < configs.size(); ++i) {
    const LaunchConfig& config = configs[i];
    std::string secondary = config.type;
    if (!config.request.empty()) {
      secondary += " · " + config.request;
    }
    std::string primary = config.name.empty() ? config.type : config.name;
    std::string search_text = util::ToLowerAscii(primary + " " + secondary);
    picker.items.push_back(QuickPickItem{
        .index = i,
        .value = {},
        .primary_label = std::move(primary),
        .secondary_label = std::move(secondary),
        .search_text = std::move(search_text),
    });
  }
  return picker;
}

QuickPickState QuickPickService::ColorThemes(std::span<const std::string> names, std::string_view active) {
  QuickPickState picker;
  picker.kind = QuickPickKind::ColorTheme;
  picker.title = "Select Color Theme";
  picker.noun = "themes";
  picker.revert_value = std::string(active);
  picker.items.reserve(names.size());
  for (std::size_t i = 0; i < names.size(); ++i) {
    const std::string& name = names[i];
    if (name == active) {
      picker.selected_index = i;
    }
    picker.items.push_back(QuickPickItem{
        .index = i,
        .value = name,
        .primary_label = name,
        .secondary_label = name == active ? "current" : "",
        .search_text = util::ToLowerAscii(name),
    });
  }
  return picker;
}

}  // namespace microide::workspace
