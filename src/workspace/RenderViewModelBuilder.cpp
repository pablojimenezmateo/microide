#include "workspace/RenderViewModelBuilder.h"

#include "workspace/StatusBarService.h"

namespace microide::workspace {

namespace {

SidebarMode SidebarModeFromViewId(std::string_view view_id) {
  if (view_id == "search") {
    return SidebarMode::Search;
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
  std::optional<FrameSurfaceViewModel::CompareSurfaceViewModel> compare_surface;
  if (context_.current_project_state.active_tab_index < context_.current_project_state.open_tabs.size()) {
    const TabEntry& active_tab =
        context_.current_project_state.open_tabs[context_.current_project_state.active_tab_index];
    if (active_tab.kind == TabEntry::Kind::Compare || active_tab.kind == TabEntry::Kind::Merge) {
      compare_surface = FrameSurfaceViewModel::CompareSurfaceViewModel{
          .kind = active_tab.kind,
      };
    }
  }

  return FrameSurfaceViewModel{
      .layout = layout,
      .sidebar_visible = context_.current_project_state.sidebar.visible,
      .bottom_panel_visible = context_.current_project_state.panel.command_mode ||
                              context_.current_project_state.panel.content !=
                                  PanelContentKind::None,
      .compare_surface = compare_surface,
      .project_state = const_cast<ProjectWorkspaceState*>(&context_.current_project_state),
  };
}

OverlaySurfaceViewModel RenderViewModelBuilder::BuildOverlaySurface() const {
  return OverlaySurfaceViewModel{
      .visible = context_.current_project_state.overlay.visible,
      .mode = context_.current_project_state.overlay.mode,
      .scroll_row = context_.current_project_state.overlay.scroll_row,
      .current_surface = context_.text_input.active_surface,
      .buffer_search_query_text =
          context_.current_project_state.overlay.workflow.buffer_search.query.text(),
      .state = &context_.current_project_state.overlay,
      .project_state = const_cast<ProjectWorkspaceState*>(&context_.current_project_state),
  };
}

TextInputSurfaceViewModel RenderViewModelBuilder::BuildTextInputSurface() const {
  return TextInputSurfaceViewModel{
      .current_surface = context_.text_input.active_surface,
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
      .chat_composer = &context_.current_project_state.panel.chat.composer,
  };
}

SidebarSurfaceViewModel RenderViewModelBuilder::BuildSidebarSurface() const {
  const auto& project_search = context_.current_project_state.overlay.workflow.project_search;
  const bool editing_query =
      project_search.editing && project_search.edit_field == ProjectSearchEditField::Query;
  const bool editing_replace =
      project_search.editing && project_search.edit_field == ProjectSearchEditField::Replace;
  const std::string_view query_text =
      editing_query ? project_search.edit_buffer.text() : project_search.query.text();
  const std::string_view replace_text =
      editing_replace ? project_search.edit_buffer.text() : project_search.replace_text.text();

  std::string query_fallback_text;
  if (query_text.empty()) {
    query_fallback_text = "Search in project";
  } else {
    query_fallback_text = std::string(query_text);
  }

  std::string replace_fallback_text;
  if (replace_text.empty()) {
    replace_fallback_text = "Replace in project";
  } else {
    replace_fallback_text = std::string(replace_text);
  }

  return SidebarSurfaceViewModel{
      .visible = context_.current_project_state.sidebar.visible,
      .mode = SidebarModeFromViewId(context_.current_project_state.sidebar.view_id),
      .scroll_row = context_.current_project_state.sidebar.scroll_row,
      .project_search_editing =
          context_.current_project_state.overlay.workflow.project_search.editing,
      .query_fallback_text = std::move(query_fallback_text),
      .replace_fallback_text = std::move(replace_fallback_text),
      .project_state = const_cast<ProjectWorkspaceState*>(&context_.current_project_state),
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

StatusBarViewModel RenderViewModelBuilder::BuildStatusBar(const WorkspaceLayout& layout,
                                                          const StatusBarService& service) const {
  StatusBarViewModel vm;
  vm.visible = layout.status_bar.w > 0.0f && layout.status_bar.h > 0.0f;
  vm.rect = layout.status_bar;
  vm.layout_mode = layout.layout_mode;
  if (!vm.visible) {
    return vm;
  }
  const auto& snapshot = service.Snapshot();
  const auto add_segment = [&](StatusBarSegmentId id,
                                std::vector<StatusBarSegmentViewModel>& target) {
    const auto& seg = snapshot[static_cast<std::size_t>(id)];
    if (!seg.visible || seg.text.empty()) {
      return;
    }
    target.push_back(StatusBarSegmentViewModel{id, seg.text, seg.clickable});
  };
  add_segment(StatusBarSegmentId::Project, vm.left_segments);
  add_segment(StatusBarSegmentId::Branch, vm.left_segments);
  add_segment(StatusBarSegmentId::Language, vm.left_segments);
  add_segment(StatusBarSegmentId::Indent, vm.left_segments);
  add_segment(StatusBarSegmentId::Encoding, vm.left_segments);
  add_segment(StatusBarSegmentId::LineColumn, vm.right_segments);
  add_segment(StatusBarSegmentId::Lsp, vm.right_segments);
  add_segment(StatusBarSegmentId::LayoutMode, vm.right_segments);

  if (vm.layout_mode == LayoutMode::Compact) {
    if (!vm.right_segments.empty() &&
        vm.right_segments.back().id == StatusBarSegmentId::LayoutMode) {
      vm.right_segments.pop_back();  // drop layout-mode badge
    }
    while (vm.left_segments.size() > 2) {  // keep project + branch
      vm.left_segments.pop_back();
    }
  }
  return vm;
}

SettingsOverlayViewModel RenderViewModelBuilder::BuildSettingsOverlay(
    const WorkspaceLayout& layout,
    const SettingsOverlayService& service) const {
  SettingsOverlayViewModel vm;
  vm.visible = service.Visible();
  vm.mode = service.Mode();
  vm.rect = ComputeOverlaySurfaceRect(layout.editor_area);
  vm.scroll_row = service.ScrollRow();
  vm.query = service.Query();
  if (!vm.visible) {
    return vm;
  }
  switch (vm.mode) {
    case SettingsOverlayMode::Settings:
      vm.title = "Settings";
      vm.settings_rows = service.SettingsRows();
      break;
    case SettingsOverlayMode::HelpAbout:
      vm.title = "Help / About";
      vm.help_rows = service.HelpRows();
      break;
  }
  return vm;
}

}  // namespace microide::workspace
