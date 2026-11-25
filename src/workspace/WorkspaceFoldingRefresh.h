#pragma once

#include <cstddef>

#include "workspace/state/WorkspaceTabState.h"

namespace microide::editor {
class TextViewport;
}

namespace microide::workspace {

struct LanguageContract;

void EnsureFoldingModelFresh(TabEntry::EditorTabState& tab,
                             editor::TextViewport& viewport,
                             const LanguageContract* contract,
                             std::size_t tab_size,
                             bool fold_enabled,
                             std::size_t visible_rows);

void CollapseRestoredFolds(TabEntry::EditorTabState& tab, editor::TextViewport& viewport);

bool RevealCaretsInsideCollapsedFolds(TabEntry::EditorTabState& tab,
                                      const editor::TextViewport& viewport);

}
