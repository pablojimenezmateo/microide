#pragma once

#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceLayout.h"

#include <filesystem>
#include <string>

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
  std::string buffer_search_query_text;
  const OverlayState* state = nullptr;
};

struct TextInputSurfaceViewModel {
  bool prompt_editing = false;
  bool command_mode = false;
  const util::SingleLineTextState* command_input = nullptr;
  const util::SingleLineTextState* prompt_input = nullptr;
  const util::SingleLineTextState* buffer_search_query = nullptr;
  const util::SingleLineTextState* buffer_search_replace = nullptr;
  const util::SingleLineTextState* project_search_query = nullptr;
  const util::SingleLineTextState* project_search_edit_buffer = nullptr;
  const util::SingleLineTextState* commit_picker_query = nullptr;
  const util::SingleLineTextState* file_finder_query = nullptr;
};

struct SidebarSurfaceViewModel {
  bool visible = false;
  SidebarMode mode = SidebarMode::Tree;
  int scroll_row = 0;
  bool project_search_editing = false;
};

struct BottomPanelSurfaceViewModel {
  bool command_mode = false;
  PanelContentKind content = PanelContentKind::None;
  float height = 0.0f;
  std::string output_channel_id;
  std::filesystem::path project_root;
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
