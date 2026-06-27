#include "workspace/WorkspaceFoldingRefresh.h"

#include "editor/TextViewport.h"
#include "workspace/WorkspaceLanguageContract.h"

namespace microide::workspace {

namespace {

// Per-frame compute budget for the fold scan. The advisory budget in the
// design is "≤ 4ms P95 with `ComputeWithBudget(2000)` partial fallback on
// 50k-line file"; mirroring 2000 lines here keeps a single render frame off
// the typing-latency hot path on very large buffers. The model returns
// partial results in that case and the renderer paints the resolved ranges.
constexpr std::size_t kFoldingModelComputeBudget = 2000;

}  // namespace

void EnsureFoldingModelFresh(TabEntry::EditorTabState& tab,
                             editor::TextViewport& viewport,
                             const LanguageContract* contract,
                             std::size_t tab_size,
                             bool fold_enabled,
                             std::size_t visible_rows) {
  auto& model = *tab.folding_model;
  if (!fold_enabled) {
    if (!model.ranges().empty() || !model.collapsed_flags().empty()) {
      // Auto-expand any persisted user collapses so disabling the toggle
      // never leaves rows hidden, then drop the resolved ranges.
      model.ExpandAll();
      model.Clear();
    }
    return;
  }

  editor::FoldingModel::Fingerprint fingerprint;
  // FoldingModel's fingerprint identifies the source bytes the bracket scan
  // was built from; only content mutations change those.
  fingerprint.layout_revision = viewport.content_revision();
  fingerprint.tab_size = tab_size == 0 ? 1 : tab_size;
  fingerprint.language_id = contract == nullptr ? std::string{} : contract->language_id;

  if (model.IsFresh(fingerprint)) {
    return;
  }

  editor::FoldingModel::ComputeOptions options;
  options.tab_size = fingerprint.tab_size;
  options.use_indent_source = true;
  if (contract != nullptr) {
    options.bracket_pairs.reserve(contract->bracket_pairs.size());
    for (const auto& pair : contract->bracket_pairs) {
      if (pair.open.size() == 1 && pair.close.size() == 1) {
        options.bracket_pairs.emplace_back(pair.open.front(), pair.close.front());
      }
    }
  }

  const std::size_t fold_resume_line = viewport.ConsumeFoldEditAnchorLine();
  const std::size_t visible_start = viewport.scroll_line();
  const std::size_t visible_end =
      visible_rows == 0 ? visible_start : visible_start + visible_rows - 1;
  model.EnsureFoldsForVisibleRange(viewport.lines(), options, visible_start, visible_end,
                                   kFoldingModelComputeBudget, fold_resume_line, &viewport);
  model.SetFingerprint(fingerprint);
}

}  // namespace microide::workspace
