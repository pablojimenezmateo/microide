#pragma once

#include <filesystem>

#include "editor/FoldingModel.h"
#include "editor/TextViewport.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/state/WorkspaceTabState.h"

namespace microide::workspace {

// The group's active tab's editor state, or null when that tab is no editor (a
// compare, a merge, a terminal) or the group has none.
TabEntry::EditorTabState* GroupActiveEditorTab(EditorGroup& group);
const TabEntry::EditorTabState* GroupActiveEditorTab(const EditorGroup& group);
// The viewport the group shows: its editor's, or its home surface's.
editor::TextViewport* GroupActiveViewport(EditorGroup& group);
const editor::TextViewport* GroupActiveViewport(const EditorGroup& group);
editor::FoldingModel* GroupFoldingModelPtr(EditorGroup& group);

// An editor tab over `view`, its restore point taken from the view.
TabEntry::EditorTabState MakeEditorTabState(const editor::TextViewport& view);
// The file an editor tab is for, whether or not its content has loaded.
std::filesystem::path EditorViewPath(const TabEntry::EditorTabState& editor_state);

}  // namespace microide::workspace
