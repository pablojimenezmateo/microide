#include "workspace/WorkspaceFoldingRefresh.h"

#include <vector>

#include <algorithm>

#include "editor/TextViewport.h"
#include "workspace/WorkspaceLanguageContract.h"

namespace microide::workspace {

namespace {

// Per-refresh line budget for the fold scan. It bounds how many *uncached* lines
// a single refresh may scan or measure, so opening a very large file never
// stalls one frame on the whole document; the model returns partial results and
// the renderer paints the resolved ranges. The model doubles this internally
// while it is still catching up, so a jump deep into a huge file converges in
// O(log n) frames rather than O(n / budget).
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
  // The fingerprint identifies the source bytes the scan was built from; only
  // content mutations change those.
  fingerprint.layout_revision = viewport.content_revision();
  fingerprint.tab_size = tab_size == 0 ? 1 : tab_size;
  fingerprint.language_id = contract == nullptr ? std::string{} : contract->language_id;
  return fingerprint;
}

// The LINE range the viewport's visual rows currently span. Fold ranges are
// addressed by logical line, and a collapsed fold makes those two coordinate
// systems diverge by however many lines it hides -- resolving row indices as if
// they were line indices would leave the rows below a collapsed fold outside the
// resolved window.
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

}  // namespace

void EnsureFoldingModelFresh(TabEntry::EditorTabState& tab,
                             editor::TextViewport& viewport,
                             const LanguageContract* contract,
                             std::size_t tab_size,
                             bool fold_enabled,
                             std::size_t visible_rows) {
  auto& model = *tab.folding_model;
  if (!fold_enabled) {
    if (model.HasFolds()) {
      // Auto-expand any persisted user collapses so disabling the toggle never
      // leaves rows hidden, then drop the resolved ranges.
      model.ExpandAll();
      model.Clear();
    }
    return;
  }

  const editor::FoldingModel::Fingerprint fingerprint =
      BuildFingerprint(viewport, contract, tab_size);
  const auto [first_line, last_line] = VisibleLineRange(viewport, visible_rows);

  // Content-freshness alone is not enough to skip: a resolve covers a window, so
  // scrolling to lines outside it must resolve again even though the content has
  // not changed.
  if (model.IsFresh(fingerprint) && model.IsWindowResolved(first_line, last_line)) {
    return;
  }

  const editor::FoldingModel::ComputeOptions options = BuildComputeOptions(contract, tab_size);
  const editor::LineEditSpan fold_edit_span = viewport.ConsumeFoldEditSpan();
  model.Refresh(viewport.lines(), options, first_line, last_line, kFoldingModelComputeBudget,
                fold_edit_span, &viewport);
  model.SetFingerprint(fingerprint);
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
    // Walk the collapsed set, not the resolved window: the fold hiding this line
    // can be anywhere in the document. Collect first -- expanding mutates the set.
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

}  // namespace microide::workspace
