#include "workspace/render/WorkspaceShellRenderPrimitives.h"
#include "render/ScopedRenderClip.h"

#include "workspace/ProjectSearchPanelLayout.h"
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

constexpr float kSidebarInset = 10.0f;
// Frame prep derives the commit-draft body field width from the sidebar width and
// its own copy of this inset (it has no panel layout to read). If the two ever
// drift, the viewport is sized for a different width than the field is drawn at
// and the body's wrap/scroll silently disagrees with what is painted.
static_assert(kSidebarInset == kCommitWorkflowFieldInset,
              "PrepareCommitBodyViewportForFrame sizes the commit body field with "
              "kCommitWorkflowFieldInset; keep it equal to the sidebar inset");

}  // namespace

void WorkspaceShell::RenderSidebarSurface(SDL_Renderer* renderer, const WorkspaceLayout& layout) {
  const SidebarSurfaceViewModel& sidebar_vm = *prepare_cached_sidebar_vm_;
  ProjectWorkspaceState& project_state = *sidebar_vm.project_state;
  if (!sidebar_vm.visible) {
    return;
  }
  // Nothing the sidebar paints leaves it. Narrowed by a drag, its header buttons
  // and its rows' truncated labels ran past its right edge into the editor -- so
  // a full frame drew them there and a partial frame of the editor painted over
  // them, and the two disagreed (TD-2026-10-20-372, CI's ASan monkey, seed 4).
  const SDL_FRect& sidebar_area = layout.sidebar;
  const render::ScopedRenderClip sidebar_clip(
      renderer, SDL_Rect{static_cast<int>(std::floor(sidebar_area.x)), static_cast<int>(std::floor(sidebar_area.y)),
                         static_cast<int>(std::ceil(sidebar_area.w)), static_cast<int>(std::ceil(sidebar_area.h))});

  // The mode tab row (Project / Search / Source Control and the overflow button),
  // as frame prep laid it out (PrepareSidebarModeRowFrame).
  PaintSidebarModeRow(renderer, frame_state_, theme_, text_renderer_);

  const SidebarMode sidebar_mode = sidebar_vm.mode;
  if (sidebar_mode == SidebarMode::Search) {
    // The whole panel -- its fields and buttons, the status line, the result list's
    // rows, its note and its scrollbar -- as frame prep laid it out
    // (PrepareSearchSidebarFrame), each element in its own PaintElement.
    PaintSearchSidebar(renderer, frame_state_, theme_, text_renderer_);
  } else if (sidebar_mode == SidebarMode::Git) {
    // The whole panel -- the action and branch rows, the Commit button or line, the
    // banners, the open commit draft, and the list's rows and scrollbar -- as frame
    // prep laid it out (PrepareGitPanelFrame), each element in its own PaintElement.
    PaintGitPanel(renderer, frame_state_, theme_, text_renderer_);
  } else if (SidebarViewUsesTheList(sidebar_mode)) {
    // One two-column list surface for every secondary sidebar view (Problems,
    // Tests, Outline, plugin items), as frame prep laid it out
    // (PrepareSidebarListFrame), each element in its own PaintElement.
    PaintSidebarList(renderer, frame_state_, theme_, text_renderer_);
  } else {
    // The file tree -- its Collapse and Refresh buttons, its rows and its scrollbar
    // -- as frame prep laid it out (PrepareTreeSidebarFrame), each element in its
    // own PaintElement.
    PaintTreeSidebar(renderer, frame_state_, theme_, text_renderer_);
  }

  if (project_state.surface.focus == FocusTarget::Sidebar) {
    DrawSurfaceFocusRing(renderer, layout.sidebar);
  }
}

}  // namespace microide::workspace
