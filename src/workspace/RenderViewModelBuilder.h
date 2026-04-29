#pragma once

#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

struct FrameSurfaceViewModel {
  WorkspaceLayout layout{};
  bool sidebar_visible = false;
  bool bottom_panel_visible = false;
};

struct OverlaySurfaceViewModel {
  bool visible = false;
  OverlayMode mode = OverlayMode::FileFinder;
  int scroll_row = 0;
};

struct TextInputSurfaceViewModel {
  bool prompt_editing = false;
  bool command_mode = false;
};

struct SidebarSurfaceViewModel {
  bool visible = false;
  SidebarMode mode = SidebarMode::Tree;
  int scroll_row = 0;
};

struct BottomPanelSurfaceViewModel {
  bool command_mode = false;
  PanelContentKind content = PanelContentKind::None;
  float height = 0.0f;
};

struct HoverPopupViewModel {
  bool visible = false;
  bool has_active_target = false;
};

struct HoverTargetsViewModel {
  bool hover_enabled = false;
};

class RenderViewModelBuilder {
 public:
  explicit RenderViewModelBuilder(const WorkspaceContext& context);

  FrameSurfaceViewModel BuildFrameSurface(const WorkspaceLayout& layout) const;
  OverlaySurfaceViewModel BuildOverlaySurface() const;
  TextInputSurfaceViewModel BuildTextInputSurface() const;
  SidebarSurfaceViewModel BuildSidebarSurface() const;
  BottomPanelSurfaceViewModel BuildBottomPanelSurface() const;
  HoverPopupViewModel BuildHoverPopup(bool has_active_target) const;
  HoverTargetsViewModel BuildHoverTargets() const;

 private:
  const WorkspaceContext& context_;
};

}  // namespace microide::workspace
