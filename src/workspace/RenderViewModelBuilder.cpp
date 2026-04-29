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
      .project_state = &context_.current_project_state,
  };
}

OverlaySurfaceViewModel RenderViewModelBuilder::BuildOverlaySurface() const {
  return OverlaySurfaceViewModel{
      .visible = context_.current_project_state.overlay.visible,
      .mode = context_.current_project_state.overlay.mode,
      .scroll_row = context_.current_project_state.overlay.scroll_row,
      .buffer_search_query_text =
          context_.current_project_state.overlay.workflow.buffer_search.query.text,
      .state = &context_.current_project_state.overlay,
      .project_state = &context_.current_project_state,
  };
}

TextInputSurfaceViewModel RenderViewModelBuilder::BuildTextInputSurface() const {
  return TextInputSurfaceViewModel{
      .prompt_editing = context_.prompts.surface_visible,
      .command_mode = context_.current_project_state.panel.command_mode,
      .command_input = &context_.current_project_state.panel.command.input,
      .prompt_input = &context_.prompts.surface.input,
      .buffer_search_query = &context_.current_project_state.overlay.workflow.buffer_search.query,
      .buffer_search_replace =
          &context_.current_project_state.overlay.workflow.buffer_search.replace_text,
      .project_search_query = &context_.current_project_state.overlay.workflow.project_search.query,
      .project_search_edit_buffer =
          &context_.current_project_state.overlay.workflow.project_search.edit_buffer,
      .commit_picker_query = &context_.current_project_state.overlay.workflow.compare_picker.query,
      .file_finder_query = &context_.current_project_state.file_finder.query_state(),
  };
}

SidebarSurfaceViewModel RenderViewModelBuilder::BuildSidebarSurface() const {
  return SidebarSurfaceViewModel{
      .visible = context_.current_project_state.sidebar.visible,
      .mode = SidebarModeFromViewId(context_.current_project_state.sidebar.view_id),
      .scroll_row = context_.current_project_state.sidebar.scroll_row,
      .project_search_editing =
          context_.current_project_state.overlay.workflow.project_search.editing,
      .project_state = &context_.current_project_state,
  };
}

BottomPanelSurfaceViewModel RenderViewModelBuilder::BuildBottomPanelSurface() const {
  return BottomPanelSurfaceViewModel{
      .command_mode = context_.current_project_state.panel.command_mode,
      .content = context_.current_project_state.panel.content,
      .height = context_.current_project_state.panel.height,
      .output_channel_id = context_.current_project_state.panel.output.channel_id,
      .project_root = context_.current_project_state.root,
      .focus = context_.current_project_state.surface.focus,
      .command_state = &context_.current_project_state.panel.command,
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
      .diagnostics_store = &context_.current_project_state.diagnostics_store,
  };
}

}  // namespace microide::workspace
