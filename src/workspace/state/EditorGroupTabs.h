#pragma once

#include <filesystem>

#include "editor/FoldingModel.h"
#include "editor/TextViewport.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/state/WorkspaceTabState.h"

namespace microide::workspace {

TabEntry::EditorTabState* GroupActiveEditorTab(EditorGroup& group);
const TabEntry::EditorTabState* GroupActiveEditorTab(const EditorGroup& group);
editor::TextViewport* GroupActiveViewport(EditorGroup& group);
const editor::TextViewport* GroupActiveViewport(const EditorGroup& group);
editor::FoldingModel* GroupFoldingModelPtr(EditorGroup& group);

TabEntry::EditorTabState MakeEditorTabState(const editor::TextViewport& view);
std::filesystem::path EditorViewPath(const TabEntry::EditorTabState& editor_state);

}
