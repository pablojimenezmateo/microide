#include "workspace/services/QuickPickService.h"

#include <algorithm>
#include <utility>

#include "editor/RuntimeSyntaxRegistry.h"
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
  if (picker.server_filtered) {
    // The server filters: the items shown are its last answer while it is asked
    // again.
    picker.loading = true;
    if (operations_.requery) {
      operations_.requery(picker.kind, picker.query.text());
    }
    ShowItemsAsTheyAre();
    return;
  }
  const std::string query = util::ToLowerAscii(picker.query.text());
  // An exact match first, then the items the query starts, then the rest that
  // hold it, each in item order: `light` must not sit under `dukelight-tc`. The
  // search text leads with the item's name (a symbol's kind follows a space).
  const auto rank_of = [&](const std::string& text) {
    const std::size_t at = text.find(query);
    if (at == std::string::npos) {
      return 3;
    }
    if (query.empty() || (at == 0 && (text.size() == query.size() || text[query.size()] == ' '))) {
      return 0;
    }
    return at == 0 ? 1 : 2;
  };
  for (int rank = 0; rank < 3; ++rank) {
    for (const QuickPickItem& item : picker.items) {
      if (rank_of(item.search_text) == rank) {
        picker.matches.push_back(item);
      }
    }
  }
  picker.summary_line = picker.loading ? "Loading " + picker.noun + "…"
                                       : BuildFilteredCountSummary(picker.matches.size(), picker.items.size(),
                                                                   picker.noun);
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
    case QuickPickKind::DocumentSymbol:
      if (operations_.reveal && state_->revert_view.has_value()) {
        operations_.reveal(state_->revert_view->path, item.line, item.column);
      }
      break;
    case QuickPickKind::LaunchConfig:
    case QuickPickKind::WorkspaceSymbol:  // VS Code opens nothing until Enter
    case QuickPickKind::LanguageMode:
    case QuickPickKind::RecentEditor:
      break;
  }
}

void QuickPickService::Accept() {
  if (state_ == nullptr || state_->selected_index >= state_->matches.size()) {
    return;
  }
  const QuickPickItem item = state_->matches[state_->selected_index];
  state_->revert_value.clear();
  const std::optional<QuickPickState::View> view = std::exchange(state_->revert_view, std::nullopt);
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
    case QuickPickKind::DocumentSymbol:
      if (operations_.reveal && view.has_value()) {
        operations_.reveal(view->path, item.line, item.column);
      }
      break;
    case QuickPickKind::WorkspaceSymbol:
      if (operations_.open_workspace_symbol) {
        operations_.open_workspace_symbol(item.value, item.line, item.column);
      }
      break;
    case QuickPickKind::LanguageMode:
      if (operations_.set_language) {
        operations_.set_language(item.value);
      }
      break;
    case QuickPickKind::RecentEditor:
      if (operations_.activate_tab) {
        operations_.activate_tab(item.index);
      }
      break;
  }
}

void QuickPickService::Cancel() {
  if (state_ == nullptr) {
    return;
  }
  if (std::optional<QuickPickState::View> view = std::exchange(state_->revert_view, std::nullopt);
      view.has_value() && operations_.restore_view) {
    operations_.restore_view(*view);
  }
  if (state_->revert_value.empty()) {
    return;
  }
  const std::string revert = std::exchange(state_->revert_value, {});
  if (state_->kind == QuickPickKind::ColorTheme && operations_.apply_colorscheme) {
    operations_.apply_colorscheme(revert, /*persist=*/false);
  }
}

bool QuickPickService::DeliverItems(QuickPickKind kind, std::vector<QuickPickItem> items) {
  if (state_ == nullptr || state_->kind != kind || !state_->loading ||
      (operations_.showing && !operations_.showing())) {
    return false;
  }
  state_->loading = false;
  state_->items = std::move(items);
  if (state_->server_filtered) {
    ShowItemsAsTheyAre();
  } else {
    Refilter();
  }
  return true;
}

void QuickPickService::Fail(QuickPickKind kind, std::string reason) {
  if (state_ == nullptr || state_->kind != kind) {
    return;
  }
  state_->loading = false;
  state_->items.clear();
  state_->matches.clear();
  state_->summary_line = std::move(reason);
  if (operations_.list_changed) {
    operations_.list_changed();
  }
}

void QuickPickService::ShowItemsAsTheyAre() {
  QuickPickState& picker = *state_;
  picker.matches = picker.items;
  picker.selected_index = 0;
  picker.summary_line = picker.loading ? "Loading " + picker.noun + "…"
                                       : BuildFilteredCountSummary(picker.matches.size(), picker.items.size(),
                                                                   picker.noun);
  if (operations_.list_changed) {
    operations_.list_changed();
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

QuickPickState QuickPickService::DocumentSymbolsLoading(QuickPickState::View view) {
  QuickPickState picker;
  picker.kind = QuickPickKind::DocumentSymbol;
  picker.title = "Go to Symbol in Editor";
  picker.noun = "symbols";
  picker.loading = true;
  picker.revert_view = std::move(view);
  return picker;
}

QuickPickState QuickPickService::WorkspaceSymbols(std::string query) {
  QuickPickState picker;
  picker.kind = QuickPickKind::WorkspaceSymbol;
  picker.title = "Search Workspace Symbols";
  picker.noun = "symbols";
  picker.server_filtered = true;
  picker.query.SetText(std::move(query));
  return picker;
}

QuickPickState QuickPickService::RecentEditors(const EditorGroup& group, const std::filesystem::path& root,
                                              bool back) {
  QuickPickState picker;
  picker.kind = QuickPickKind::RecentEditor;
  picker.title = "Open Editors";
  picker.noun = "editors";
  const auto add = [&](const TabEntry& tab) {
    std::string where;
    if (!tab.path.empty()) {
      const std::filesystem::path relative = tab.path.lexically_relative(root);
      where = relative.empty() || relative.native().starts_with("..") ? tab.path.parent_path().generic_string()
                                                                     : relative.parent_path().generic_string();
    }
    picker.items.push_back(QuickPickItem{
        .index = tab.stable_id,
        .value = {},
        .line = 0,
        .column = 0,
        .primary_label = tab.title,
        .secondary_label = std::move(where),
        .search_text = util::ToLowerAscii(tab.title),
    });
  };
  // Most recent first, then any tab the order has not seen yet, in strip order.
  for (const std::uint64_t id : group.recent_tab_ids) {
    for (const TabEntry& tab : group.open_tabs) {
      if (tab.stable_id == id) {
        add(tab);
        break;
      }
    }
  }
  for (const TabEntry& tab : group.open_tabs) {
    if (std::find(group.recent_tab_ids.begin(), group.recent_tab_ids.end(), tab.stable_id) ==
        group.recent_tab_ids.end()) {
      add(tab);
    }
  }
  if (picker.items.size() > 1) {
    picker.selected_index = back ? picker.items.size() - 1 : 1;
  }
  return picker;
}

QuickPickState QuickPickService::LanguageModes(std::span<const std::string> languages, std::string_view chosen,
                                              std::string_view detected) {
  QuickPickState picker;
  picker.kind = QuickPickKind::LanguageMode;
  picker.title = "Select Language Mode";
  picker.noun = "languages";
  const auto add = [&](std::string value, std::string label, std::string detail) {
    if (value == chosen) {
      picker.selected_index = picker.items.size();
    }
    std::string search_text = util::ToLowerAscii(label);
    picker.items.push_back(QuickPickItem{
        .index = picker.items.size(),
        .value = std::move(value),
        .line = 0,
        .column = 0,
        .primary_label = std::move(label),
        .secondary_label = std::move(detail),
        .search_text = std::move(search_text),
    });
  };
  add("", "Auto Detect", detected.empty() || detected == "unknown" ? "Plain Text" : std::string(detected));
  add(std::string(editor::runtime_syntax::kPlainTextFiletype), "Plain Text", "");
  for (const std::string& language : languages) {
    if (language != "unknown") {
      add(language, language, "");
    }
  }
  return picker;
}

namespace {

void AppendSymbolItems(std::span<const plugin::PluginHost::DocumentSymbolNode> symbols, std::size_t depth,
                       std::vector<QuickPickItem>& out) {
  // A language server's tree is semi-trusted input; nothing real nests this deep.
  constexpr std::size_t kMaxDepth = 64;
  if (depth > kMaxDepth) {
    return;
  }
  for (const plugin::PluginHost::DocumentSymbolNode& symbol : symbols) {
    // Symbol nodes are 1-based; line 0 is a row with no place (the outline's
    // "truncated" marker), which a pick that goes somewhere leaves out.
    if (symbol.line == 0) {
      AppendSymbolItems(symbol.children, depth + 1, out);
      continue;
    }
    out.push_back(QuickPickItem{
        .index = out.size(),
        .value = {},
        .line = symbol.line - 1,
        .column = symbol.column > 0 ? symbol.column - 1 : 0,
        .primary_label = std::string(2 * depth, ' ') + symbol.name,
        .secondary_label = symbol.detail.empty() ? symbol.kind : symbol.detail,
        .search_text = util::ToLowerAscii(symbol.name + " " + symbol.kind),
    });
    AppendSymbolItems(symbol.children, depth + 1, out);
  }
}

}  // namespace

std::vector<QuickPickItem> QuickPickService::DocumentSymbolItems(
    std::span<const plugin::PluginHost::DocumentSymbolNode> symbols) {
  std::vector<QuickPickItem> items;
  AppendSymbolItems(symbols, 0, items);
  return items;
}

}  // namespace microide::workspace
