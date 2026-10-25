#pragma once

#include "editor/EditTypes.h"
#include "editor/TextViewport.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

// The find widget's Find in Selection ranges (BufferSearchState::scope and
// ::opened_over): ranges into one buffer, carried through its edits the way the
// editor's other ranges are (diagnostics, breakpoints).

// The range Find in Selection takes from `viewport` as it is now: the
// selection, or with none the caret's line (VS Code), stamped against it.
BufferSearchState::Scope FindScopeAt(const editor::TextViewport& viewport);

// Records which buffer state `scope.range` describes (clamping it into the
// buffer), and its end measured back from the end of the document.
void StampFindScope(BufferSearchState::Scope& scope, const editor::TextViewport& viewport);

// Carries `scope` through the edits made since it was last stamped: through the
// edit the viewport describes. An edit that does not describe itself (a
// multi-range edit: Replace All, multi-caret typing) keeps the start and puts the
// end back where the document's tail says, which is exact for an edit made
// inside the scope -- the case such an edit has under an open widget. False
// when the scope belongs to another document and has to go.
bool FollowFindScope(BufferSearchState::Scope& scope, const editor::TextViewport& viewport);

// FollowFindScope on both of the widget's ranges, dropping either once it
// belongs to another document.
void FollowFindScopes(BufferSearchState& state, const editor::TextViewport& viewport);

}  // namespace microide::workspace
