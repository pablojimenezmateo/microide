#pragma once

#include "editor/EditTypes.h"
#include "editor/TextViewport.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

BufferSearchState::Scope FindScopeAt(const editor::TextViewport& viewport);

void StampFindScope(BufferSearchState::Scope& scope, const editor::TextViewport& viewport);

bool FollowFindScope(BufferSearchState::Scope& scope, const editor::TextViewport& viewport);

void FollowFindScopes(BufferSearchState& state, const editor::TextViewport& viewport);

}
