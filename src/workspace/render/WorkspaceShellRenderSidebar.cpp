#include "workspace/render/WorkspaceShellRenderPrimitives.h"
#include "render/ScopedRenderClip.h"

#include "workspace/ProjectSearchPanelLayout.h"
#include "workspace/SidebarMetrics.h"
#include "workspace/git/GitSidebarHeaderLayout.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "editor/GutterIconRegistry.h"
#include "workspace/git/CommitWorkflowLayout.h"
#include "workspace/git/CommitWorkflowService.h"
#include "workspace/git/CommitWorkflowState.h"
#include "workspace/git/GitSidebarCommandCenter.h"
#include "workspace/render/GitSidebarPaint.h"
#include "workspace/render/SearchSidebarPaint.h"
#include "workspace/render/SidebarListPaint.h"
#include "workspace/render/SidebarModeRowPaint.h"
#include "workspace/render/TreeSidebarPaint.h"
#include "workspace/SettingFlags.h"
#include "workspace/git/WorkspaceGitSidebarPresentation.h"
#include "workspace/registries/WorkspaceSidebarRegistry.h"

namespace microide::workspace {

using namespace detail;

namespace {

static_assert(kSidebarInset == kCommitWorkflowFieldInset,
              "PrepareCommitBodyViewportForFrame sizes the commit body field with "
              "kCommitWorkflowFieldInset; keep it equal to the sidebar inset");

}

void WorkspaceShell::RenderSidebarSurface(SDL_Renderer* renderer, const WorkspaceLayout& layout) {
  const SidebarSurfaceViewModel& sidebar_vm = *prepare_cached_sidebar_vm_;
  ProjectWorkspaceState& project_state = *sidebar_vm.project_state;
  if (!sidebar_vm.visible) {
    return;
  }
  const SDL_FRect& sidebar_area = layout.sidebar;
  const render::ScopedRenderClip sidebar_clip(
      renderer, SDL_Rect{static_cast<int>(std::floor(sidebar_area.x)), static_cast<int>(std::floor(sidebar_area.y)),
                         static_cast<int>(std::ceil(sidebar_area.w)), static_cast<int>(std::ceil(sidebar_area.h))});

  PaintSidebarModeRow(renderer, frame_state_, theme_, text_renderer_);

  const SidebarMode sidebar_mode = sidebar_vm.mode;
  if (sidebar_mode == SidebarMode::Search) {
    PaintSearchSidebar(renderer, frame_state_, theme_, text_renderer_);
  } else if (sidebar_mode == SidebarMode::Git) {
    PaintGitPanel(renderer, frame_state_, theme_, text_renderer_);
  } else if (SidebarViewUsesTheList(sidebar_mode)) {
    PaintSidebarList(renderer, frame_state_, theme_, text_renderer_);
  } else {
    PaintTreeSidebar(renderer, frame_state_, theme_, text_renderer_);
  }

  if (project_state.surface.focus == FocusTarget::Sidebar) {
    DrawSurfaceFocusRing(renderer, layout.sidebar);
  }
}

}
