#include "workspace/WorkspaceFoldingRefresh.h"

#include <vector>

#include <algorithm>

#include "editor/TextViewport.h"
#include "workspace/WorkspaceLanguageContract.h"

namespace microide::workspace {

namespace {

constexpr std::size_t kFoldingModelComputeBudget = 2000;

editor::FoldingModel::ComputeOptions BuildComputeOptions(const LanguageContract* contract,
                                                         std::size_t tab_size) {
  editor::FoldingModel::ComputeOptions options;
  options.tab_size = tab_size == 0 ? 1 : tab_size;
  options.use_indent_source = true;
  if (contract != nullptr) {
    options.bracket_pairs.reserve(contract->bracket_pairs.size());
    for (const auto& pair : contract->bracket_pairs) {
      if (pair.open.size() == 1 && pair.close.size() == 1) {
        options.bracket_pairs.emplace_back(pair.open.front(), pair.close.front());
      }
    }
  }
  return options;
}

editor::FoldingModel::Fingerprint BuildFingerprint(const editor::TextViewport& viewport,
                                                   const LanguageContract* contract,
                                                   std::size_t tab_size) {
  editor::FoldingModel::Fingerprint fingerprint;
  fingerprint.layout_revision = viewport.content_revision();
  fingerprint.tab_size = tab_size == 0 ? 1 : tab_size;
  fingerprint.language_id = contract == nullptr ? std::string{} : contract->language_id;
  return fingerprint;
}

std::pair<std::size_t, std::size_t> VisibleLineRange(const editor::TextViewport& viewport,
                                                     std::size_t visible_rows) {
  const std::size_t visual_count = viewport.visual_line_count();
  if (visual_count == 0) {
    return {0, 0};
  }
  const std::size_t first_row = std::min(viewport.scroll_line(), visual_count - 1);
  const std::size_t last_row =
      visible_rows == 0 ? first_row : std::min(first_row + visible_rows - 1, visual_count - 1);
  return {viewport.VisualRowLineIndex(first_row), viewport.VisualRowLineIndex(last_row)};
}

}

void EnsureFoldingModelFresh(TabEntry::EditorTabState& tab,
                             editor::TextViewport& viewport,
                             const LanguageContract* contract,
                             std::size_t tab_size,
                             bool fold_enabled,
                             std::size_t visible_rows) {
  auto& model = *tab.folding_model;
  if (!fold_enabled) {
    tab.restore.collapsed_fold_openers.clear();
    if (model.HasFolds()) {
      model.ExpandAll();
      model.Clear();
    }
    return;
  }

  const editor::FoldingModel::Fingerprint fingerprint =
      BuildFingerprint(viewport, contract, tab_size);
  const auto [first_line, last_line] = VisibleLineRange(viewport, visible_rows);

  if (model.IsFresh(fingerprint) && model.IsWindowResolved(first_line, last_line)) {
    CollapseRestoredFolds(tab, viewport);
    return;
  }

  const editor::FoldingModel::ComputeOptions options = BuildComputeOptions(contract, tab_size);
  const editor::LineEditSpan fold_edit_span = viewport.ConsumeFoldEditSpan();
  model.Refresh(viewport.lines(), options, first_line, last_line, kFoldingModelComputeBudget,
                fold_edit_span, &viewport);
  model.SetFingerprint(fingerprint);
  CollapseRestoredFolds(tab, viewport);
}

void CollapseRestoredFolds(TabEntry::EditorTabState& tab, editor::TextViewport& viewport) {
  std::vector<std::size_t>& openers = tab.restore.collapsed_fold_openers;
  if (openers.empty() || tab.content_pending()) {
    return;
  }
  editor::FoldingModel& model = *tab.folding_model;
  model.ResolveAllFolds(viewport.lines(), &viewport);
  const bool scroll_untouched = viewport.scroll_line() == tab.restore.scroll_line;
  for (const std::size_t opener : openers) {
    if (model.FoldStartingAt(opener).has_value()) {
      (void)model.Collapse(opener);
    }
  }
  openers.clear();
  tab.fold_reveal_last_caret = editor::TextPosition{viewport.cursor_line(), viewport.cursor_column()};
  tab.fold_reveal_last_secondary_count = viewport.secondary_caret_positions().size();
  viewport.SetFoldingModel(model.HasFolds() ? &model : nullptr);
  if (scroll_untouched) {
    viewport.SetScrollLine(tab.restore.scroll_line);
  }
}

bool RevealCaretsInsideCollapsedFolds(TabEntry::EditorTabState& tab,
                                      const editor::TextViewport& viewport) {
  const editor::TextPosition caret{viewport.cursor_line(), viewport.cursor_column()};
  const std::size_t secondary_count = viewport.secondary_caret_positions().size();
  const bool moved = !tab.fold_reveal_last_caret.has_value() ||
                     !(*tab.fold_reveal_last_caret == caret) ||
                     tab.fold_reveal_last_secondary_count != secondary_count;
  tab.fold_reveal_last_caret = caret;
  tab.fold_reveal_last_secondary_count = secondary_count;
  editor::FoldingModel* model = tab.folding_model.get();
  if (!moved || model == nullptr || !model->has_any_collapsed_fold()) {
    return false;
  }

  bool changed = false;
  std::vector<editor::FoldRange> hiding;
  const auto reveal_line = [&](std::size_t line) {
    if (!model->IsLineHidden(line)) {
      return;
    }
    hiding.clear();
    for (const editor::FoldRange& range : model->collapsed_ranges()) {
      if (line > range.opener_line && line <= range.closer_line) {
        hiding.push_back(range);
      }
    }
    for (const editor::FoldRange& range : hiding) {
      changed = model->Expand(range.opener_line) || changed;
    }
  };
  reveal_line(caret.line);
  for (const editor::TextPosition& secondary : viewport.secondary_caret_positions()) {
    reveal_line(secondary.line);
  }
  return changed;
}

}
