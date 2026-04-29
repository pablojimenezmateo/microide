#include "workspace/RenderViewModelBuilder.h"

namespace microide::workspace {

namespace {

SidebarMode SidebarModeFromViewId(std::string_view view_id) {
  if (view_id == "search") {
    return SidebarMode::Search;
  }
  if (view_id == "chat") {
    return SidebarMode::Chat;
  }
  if (view_id == "problems") {
    return SidebarMode::Problems;
  }
  if (view_id == "git") {
    return SidebarMode::Git;
  }
  if (view_id == "tests") {
    return SidebarMode::Tests;
  }
  if (view_id == "plugin") {
    return SidebarMode::Plugin;
  }
  if (view_id == "tree") {
    return SidebarMode::Tree;
  }
  return SidebarMode::None;
}

}  // namespace

RenderViewModelBuilder::RenderViewModelBuilder(const WorkspaceContext& context)
    : context_(context) {}

FrameSurfaceViewModel RenderViewModelBuilder::BuildFrameSurface(const WorkspaceLayout& layout) const {
  return FrameSurfaceViewModel{
      .layout = layout,
      .sidebar_visible = context_.current_project_state.sidebar.visible,
      .bottom_panel_visible = context_.current_project_state.panel.command_mode ||
                              context_.current_project_state.panel.content !=
                                  PanelContentKind::None,
  };
}

OverlaySurfaceViewModel RenderViewModelBuilder::BuildOverlaySurface() const {
  return OverlaySurfaceViewModel{
      .visible = context_.current_project_state.overlay.visible,
      .mode = context_.current_project_state.overlay.mode,
      .scroll_row = context_.current_project_state.overlay.scroll_row,
      .buffer_search_query_text =
          context_.current_project_state.overlay.workflow.buffer_search.query.text,
  };
}

TextInputSurfaceViewModel RenderViewModelBuilder::BuildTextInputSurface() const {
  return TextInputSurfaceViewModel{
      .prompt_editing = context_.prompts.surface_visible,
      .command_mode = context_.current_project_state.panel.command_mode,
  };
}

SidebarSurfaceViewModel RenderViewModelBuilder::BuildSidebarSurface() const {
  return SidebarSurfaceViewModel{
      .visible = context_.current_project_state.sidebar.visible,
      .mode = SidebarModeFromViewId(context_.current_project_state.sidebar.view_id),
      .scroll_row = context_.current_project_state.sidebar.scroll_row,
      .project_search_editing =
          context_.current_project_state.overlay.workflow.project_search.editing,
  };
}

BottomPanelSurfaceViewModel RenderViewModelBuilder::BuildBottomPanelSurface() const {
  return BottomPanelSurfaceViewModel{
      .command_mode = context_.current_project_state.panel.command_mode,
      .content = context_.current_project_state.panel.content,
      .height = context_.current_project_state.panel.height,
      .output_channel_id = context_.current_project_state.panel.output.channel_id,
      .project_root = context_.current_project_state.root,
  };
}

HoverPopupViewModel RenderViewModelBuilder::BuildHoverPopup(bool has_active_target) const {
  return HoverPopupViewModel{
      .visible = has_active_target,
      .has_active_target = has_active_target,
  };
}

HoverTargetsViewModel RenderViewModelBuilder::BuildHoverTargets() const {
  return HoverTargetsViewModel{
      .hover_enabled = true,
  };
}

}  // namespace microide::workspace
