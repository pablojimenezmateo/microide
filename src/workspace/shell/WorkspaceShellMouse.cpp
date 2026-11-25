#include "workspace/BreadcrumbLayout.h"
#include "workspace/shell/WorkspaceShell.h"

#include <algorithm>
#include <cmath>

#include "editor/EditorInsetLayout.h"
#include "editor/EditorRowYLayout.h"
#include "editor/EditorViewRenderer.h"
#include "editor/PluginDecorationStore.h"
#include "editor/WelcomeView.h"
#include "util/PerformanceTrace.h"
#include "workspace/render/RenderViewModelBuilder.h"
#include "workspace/SettingFlags.h"
#include "workspace/actions/WorkspaceActionCoordinator.h"
#include "workspace/coordinators/EditorPointerHit.h"
#include "workspace/coordinators/SelectionAutoscroll.h"
#include "workspace/coordinators/WorkspaceCompareMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceChromeMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceEditorMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceMenuCoordinator.h"
#include "workspace/coordinators/WorkspaceMergeMouseCoordinator.h"
#include "workspace/debug/DebugPaneMouseCoordinator.h"
#include "workspace/render/NotificationLayout.h"
#include "workspace/coordinators/WorkspacePanelMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceSidebarMouseCoordinator.h"
#include "workspace/coordinators/WorkspaceTabMouseCoordinator.h"

namespace microide::workspace {

bool WorkspaceShell::HandleMouseButtonDown(const SDL_Event& event) {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::HandleMouseButtonDown");
  DamageMark mark = MarkDamage();
  if (event.button.button != SDL_BUTTON_LEFT && event.button.button != SDL_BUTTON_MIDDLE &&
      event.button.button != SDL_BUTTON_RIGHT) {
    return false;
  }
  const auto window_rect = CurrentWindowRect();
  const auto layout_state = CurrentWorkspaceLayout();
  if (!window_rect.has_value() || !layout_state.has_value()) {
    return false;
  }
  const WorkspaceLayout layout = *layout_state;

  if (MenuSurfaceCapturingMouse()) {
    UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y), false);
    mark = MarkDamage();
    if (MakeChromeMouseCoordinator().HandleButtonDown(event, layout)) {
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      return true;
    }
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT && !notification_service_.Empty()) {
    const NotificationsViewModel toasts = RenderViewModelBuilder(context_).BuildNotifications(
        notification_service_, layout.status_bar, text_renderer_);
    if (const std::optional<NotificationHit> hit =
            NotificationHitAt(toasts, static_cast<float>(event.button.x),
                              static_cast<float>(event.button.y));
        hit.has_value()) {
      if (!hit->action.has_value()) {
        notification_service_.Dismiss(hit->index);
      } else if (const std::optional<NotificationService::Action> action =
                     notification_service_.TakeAction(hit->index, *hit->action)) {
        ActionCoordinator(MakeActionContext()).Execute(action->id, action->args,
                                                       ActionSource::Command);
      }
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      return true;
    }
  }

  const auto visible_hover_popup = ActiveEditorHoverPopupLayout();
  if (event.button.button == SDL_BUTTON_LEFT && visible_hover_popup.has_value() &&
      Contains(visible_hover_popup->rect, event.button.x, event.button.y)) {
    const bool primary_action_hit =
        visible_hover_popup->primary_action_rect.has_value() &&
        Contains(EditorHoverPopupPrimaryActionHitRect(*visible_hover_popup), event.button.x,
                 event.button.y);
    if (visible_hover_popup->kind == EditorHoverTarget::Kind::Blame && primary_action_hit) {
      if (const editor::EditorBlameLine* blame_line =
              editor_blame_overlay_service_.VisibleLine(visible_hover_popup->blame_line_index);
          blame_line != nullptr && !blame_line->commit_id.empty() &&
          WriteClipboardText(blame_line->commit_id)) {
      }
    } else if (visible_hover_popup->kind == EditorHoverTarget::Kind::Diagnostic &&
               primary_action_hit && visible_hover_popup->diagnostic.has_value()) {
      const editor::SelectionRange range = visible_hover_popup->diagnostic->range;
      active_editor_hover_target_.reset();
      assist_service_.code_actions().ShowOverlay(nullptr, &range);
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      return true;
    }
    context_.current_project_state.surface.focus = FocusTarget::Editor;
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }
  UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y));
  mark = MarkDamage();
  if (const auto decided = MouseDownOnModals(event, layout, mark, window_rect)) {
    return *decided;
  }
  if (const auto decided = MouseDownOnChrome(event, layout, mark)) {
    return *decided;
  }
  if (const auto decided = MouseDownOnDividersAndPanels(event, layout, mark)) {
    return *decided;
  }
  if (const auto decided = MouseDownOpensContextMenu(event, layout, mark)) {
    return *decided;
  }
  return MouseDownOnEditorArea(event, layout, mark);
}

std::optional<bool> WorkspaceShell::MouseDownOnModals(const SDL_Event& event, const WorkspaceLayout& layout,
                                                      DamageMark mark, const std::optional<SDL_FRect>& window_rect) {
  if (context_.prompts.dirty_visible) {
    const SDL_FRect dialog = ComputeDirtyPromptRect(*window_rect);
    const auto buttons = ComputeDirtyPromptButtonRects(dialog);
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      if (Contains(buttons[i], event.button.x, event.button.y)) {
        context_.prompts.dirty.selected_action = static_cast<int>(i);
        ConfirmDirtyPrompt();
        if (context_.prompts.dirty_visible) {
          RequestPreparedFrame();
        } else {
          EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
        }
        return true;
      }
    }
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  if (context_.prompts.surface_visible) {
    const SDL_FRect dialog = ComputePromptSurfaceRect(*window_rect);
    const auto buttons =
        ComputePromptSurfaceButtonRects(dialog, context_.prompts.surface.button_count);
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      if (Contains(buttons[i], event.button.x, event.button.y)) {
        context_.prompts.surface.selected_button = static_cast<int>(i);
        if (event.button.button == SDL_BUTTON_LEFT) {
          ConfirmPromptSurface();
        }
        EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
        return true;
      }
    }
    if (HandleSingleLineInputMouseDown(event, layout)) {
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      return true;
    }
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  if (HandleSingleLineInputMouseDown(event, layout)) {
    switch (context_.interaction_state.single_line_drag_surface) {
      case TextInputSurface::SidebarSearchQuery:
      case TextInputSurface::SidebarSearchReplace:
      case TextInputSurface::SidebarSearchInclude:
      case TextInputSurface::SidebarSearchExclude:
      case TextInputSurface::CommitSubject:
        EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
        break;
      case TextInputSurface::FileFinder:
      case TextInputSurface::BufferSearch:
      case TextInputSurface::BufferReplaceSearch:
      case TextInputSurface::BufferReplaceReplace:
      case TextInputSurface::ProjectSearchOverlay:
      case TextInputSurface::CommitPicker:
        EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
        break;
      default:
        EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
        break;
    }
    return true;
  }

  if (!context_.text_input.composition.text.empty()) {
    context_.text_input.composition = TextCompositionState{};
    if (SDL_Window* window = SDL_GetKeyboardFocus(); window != nullptr) {
      SDL_ClearComposition(window);
    }
  }

  context_.interaction_state.mouse_selecting = false;
  context_.interaction_state.text_drag = InteractionState::TextDragState::None;
  context_.interaction_state.text_drag_has_drop = false;
  selection_autoscroll::Disarm(context_.interaction_state);
  return std::nullopt;
}

std::optional<bool> WorkspaceShell::MouseDownOnChrome(const SDL_Event& event, const WorkspaceLayout& layout,
                                                      DamageMark mark) {
  if (HandleSettingsOverlayButtonDown(event, layout)) {
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT &&
      Contains(layout.status_bar, static_cast<float>(event.button.x),
               static_cast<float>(event.button.y))) {
    const StatusBarViewModel status_vm =
        RenderViewModelBuilder(context_).BuildStatusBar(layout, status_bar_service_);
    const StatusBarSegmentViewModel* hit = nullptr;
    SDL_FRect hit_rect{};
    ForEachStatusBarSegmentRect(
        status_vm, text_renderer_,
        [&](const StatusBarSegmentViewModel& segment, const SDL_FRect& rect) {
          if (hit == nullptr && segment.clickable &&
              Contains(rect, static_cast<float>(event.button.x), static_cast<float>(event.button.y))) {
            hit = &segment;
            hit_rect = rect;
          }
        });
    if (hit != nullptr && hit->menu != MenuId::None) {
      MakeMenuCoordinator().OpenAnchoredMenu(hit->menu, hit_rect);
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      return true;
    }
    const std::string_view command = hit != nullptr ? hit->command : std::string_view();
    const std::string_view command_arg = hit != nullptr ? hit->command_arg : std::string_view();
    if (!command.empty()) {
      std::vector<std::string> args;
      if (!command_arg.empty()) {
        args.emplace_back(command_arg);
      }
      std::string error_message;
      ExecuteCommandName(command, args, ActionSource::Command, &error_message);
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      return true;
    }
  }


  if (MakeChromeMouseCoordinator().HandleButtonDown(event, layout)) {
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    if (MenuSurfaceCapturingMouse()) {
      UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y), false);
    }
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT && !MenuSurfaceCapturingMouse()) {
    for (const VisibleStatusItem& status_item : ComputeVisibleStatusItems(layout.breadcrumb)) {
      if (!status_item.item.command.empty() &&
          Contains(status_item.rect, event.button.x, event.button.y)) {
        std::string error_message;
        ExecuteCommandName(status_item.item.command, {}, ActionSource::Command, &error_message);
        EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
        return true;
      }
    }
  }

  if (event.button.button == SDL_BUTTON_LEFT && !MenuSurfaceCapturingMouse()) {
    const EditorGroupRectsLayout group_rects = ComputeEditorGroupRectsForState(layout);
    for (std::size_t gi = 0; gi < group_rects.groups.size() && gi < context_.current_project_state.editor_groups.size();
         ++gi) {
      const SDL_FRect band = group_rects.groups[gi].breadcrumb;
      const EditorGroup& group = context_.current_project_state.editor_groups[gi];
      if (!Contains(band, event.button.x, event.button.y) || !group.has_active_tab() ||
          group.active_tab().path.empty()) {
        continue;
      }
      const auto [text_x, text_right] = BreadcrumbTextSpan(
          band, ComputeVisibleStatusItems(layout.breadcrumb),
          [](const VisibleStatusItem& item) { return item.item.alignment == StatusAlignment::Left; });
      std::string_view label = BreadcrumbLabel(gi);
      label = label.substr(0, label.find("  |  "));
      const std::optional<std::string> folder =
          BreadcrumbFolderAt(label, text_x, static_cast<float>(event.button.x),
                             [this](std::string_view prefix) { return text_renderer_.MeasureWidth(prefix); });
      if (!folder.has_value()) {
        break;
      }
      std::string error_message;
      (void)ExecuteCommandName("browse-folder", {(context_.current_project_state.root / *folder).string()},
                               ActionSource::Command, &error_message);
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      return true;
    }
  }

  if (event.button.button == SDL_BUTTON_LEFT) {
    const bool code_lens_above = SettingFlagEnabled(GetSettingValue("plugins.code_lens_above"));
    const auto lens = code_lens_above ? AboveLensAtPosition(static_cast<float>(event.button.x),
                                                            static_cast<float>(event.button.y))
                                      : CodeLensAtPosition(static_cast<float>(event.button.x),
                                                           static_cast<float>(event.button.y));
    if (lens.has_value()) {
      context_.current_project_state.surface.focus = FocusTarget::Editor;
      if (lens->payload != 0) {
        lsp_service_.ActivateCodeLens(lens->payload);
      } else {
        std::string error_message;
        ExecuteCommandName(lens->command, {}, ActionSource::Command, &error_message);
      }
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      return true;
    }
    if (editor_blame_overlay_service_.LineAtPosition(static_cast<float>(event.button.x),
                                                     static_cast<float>(event.button.y)) != nullptr) {
      context_.current_project_state.surface.focus = FocusTarget::Editor;
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      return true;
    }
  }
  return std::nullopt;
}

std::optional<bool> WorkspaceShell::MouseDownOnDividersAndPanels(const SDL_Event& event, const WorkspaceLayout& layout,
                                                                 DamageMark mark) {
  const bool divider_reset_click = event.button.clicks >= 2;

  if (event.button.button == SDL_BUTTON_LEFT && context_.current_project_state.sidebar.visible &&
      Contains(SidebarResizeHitRect(layout), event.button.x, event.button.y)) {
    if (divider_reset_click) {
      const float window_width = CurrentWindowRect().has_value() ? CurrentWindowRect()->w : 0.0f;
      context_.current_project_state.sidebar.width =
          ClampSidebarWidth(kWorkspaceDefaultSidebarWidth, window_width);
      ClearDragState();
      RequestSidebarLayoutChangeRedraw(layout);
      return true;
    }
    context_.interaction_state.drag_target = DragTarget::SidebarDivider;
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT && context_.current_project_state.debug_pane.visible &&
      Contains(RightPaneResizeHitRect(layout), event.button.x, event.button.y)) {
    if (divider_reset_click) {
      const float window_width = CurrentWindowRect().has_value() ? CurrentWindowRect()->w : 0.0f;
      const float resolved_sidebar_width = context_.current_project_state.sidebar.visible
                                               ? layout.sidebar.w
                                               : 0.0f;
      context_.current_project_state.debug_pane.width = ClampRightPaneWidth(
          kWorkspaceDefaultRightPaneWidth, window_width, resolved_sidebar_width);
      ClearDragState();
      NoteLayoutInputsChanged();
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      return true;
    }
    context_.interaction_state.drag_target = DragTarget::RightPaneDivider;
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT) {
    for (const EditorSplitDividerRect& divider :
         ComputeEditorGroupRectsForState(layout).dividers) {
      if (Contains(divider.rect, event.button.x, event.button.y)) {
        if (divider_reset_click) {
          context_.current_project_state.editor_split.ResetDivider(divider.node,
                                                                   divider.boundary);
          ClearDragState();
          NoteLayoutInputsChanged();
          EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
          return true;
        }
        context_.interaction_state.drag_target = DragTarget::EditorSplitDivider;
        context_.interaction_state.drag_editor_split_node = divider.node;
        context_.interaction_state.drag_editor_split_boundary = divider.boundary;
        return true;
      }
    }
  }

  if (MakeDebugPaneMouseCoordinator().HandleButtonDown(event, layout)) {
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  if (const float height_before = context_.current_project_state.panel.height;
      MakePanelMouseCoordinator().HandleResizeButtonDown(event, layout)) {
    if (context_.current_project_state.panel.height != height_before) {
      RequestBottomPanelLayoutChangeRedraw(layout);
      return true;
    }
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  if (MakeSidebarMouseCoordinator().HandleButtonDown(event, layout)) {
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  {
    util::PerformanceTrace::Scope tab_scope("WorkspaceShell::HandleMouseButtonDown::Tabs");
    if (MakeTabMouseCoordinator().HandleButtonDown(event, layout)) {
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      return true;
    }
  }

  if (MakePanelMouseCoordinator().HandleButtonDown(event, layout)) {
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }
  return std::nullopt;
}

std::optional<bool> WorkspaceShell::MouseDownOpensContextMenu(const SDL_Event& event, const WorkspaceLayout& layout,
                                                              DamageMark mark) {
  if (event.button.button == SDL_BUTTON_RIGHT && ActiveTabIsEditor() &&
      Contains(layout.editor_surface, event.button.x, event.button.y)) {
    SyncActiveEditorTab();
    if (MakeEditorMouseCoordinator().HandleGutterContextMenu(event, layout)) {
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      return true;
    }
  }

  if (event.button.button == SDL_BUTTON_RIGHT &&
      Contains(layout.editor_surface, event.button.x, event.button.y) &&
      ActiveEditableViewport() != nullptr) {
    const bool retargeted_cursor = [&]() {
      if (!ActiveTabIsEditor()) {
        return false;
      }

      SyncActiveEditorTab();
      if (ActiveEditorTab() == nullptr) {
        return false;
      }

      const auto panes = ComputeEditorPaneLayouts(layout);
      const auto pane_it = std::find_if(
          panes.begin(), panes.end(),
          [&](const EditorPaneLayout& pane) {
            return Contains(pane.rect, event.button.x, event.button.y);
          });
      if (pane_it == panes.end()) {
        return false;
      }

      auto& project_state = context_.current_project_state;
      if (pane_it->group_index != project_state.focused_group_index &&
          pane_it->group_index < project_state.editor_groups.size()) {
        project_state.focused_group_index = pane_it->group_index;
        RequestPreparedFrame();
      }

      editor::TextViewport* viewport = ActiveEditorViewport();
      if (viewport == nullptr) {
        return false;
      }

      const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
          editor_text_renderer_, *viewport, pane_it->rect, viewport->sticky_band_rows(),
          LineNumbersEnabled());
      viewport->SetViewportSize(metrics.visible_rows, metrics.visible_columns);

      const auto [visual_row, hit_column, hit] = ResolveEditorPointerHit(
          context_.current_project_state, *viewport, metrics, editor_text_renderer_,
          event.button.x, event.button.y,
          [this](std::string_view id) { return GetSettingValue(id); });
      if (viewport->AnySelectionContains(hit)) {
        return false;
      }
      viewport->MoveCursorToVisualHit(visual_row, hit_column, false);
      return true;
    }();
    MakeMenuCoordinator().OpenAnchoredMenu(
        MenuId::EditorContext,
        MakeRect(static_cast<float>(event.button.x), static_cast<float>(event.button.y), 1.0f,
                 1.0f));
    if (retargeted_cursor) {
      ResetCaretBlink();
    }
    context_.current_project_state.surface.focus = FocusTarget::Editor;
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }
  return std::nullopt;
}

bool WorkspaceShell::MouseDownOnEditorArea(const SDL_Event& event, const WorkspaceLayout& layout, DamageMark mark) {
  if (!Contains(layout.editor_surface, event.button.x, event.button.y) ||
      (event.button.button != SDL_BUTTON_LEFT && event.button.button != SDL_BUTTON_MIDDLE)) {
    return false;
  }

  if (EditorGroupCount() > 1) {
    const EditorGroupRectsLayout group_rects = ComputeEditorGroupRectsForState(layout);
    const auto& groups = context_.current_project_state.editor_groups;
    for (std::size_t gi = 0; gi < group_rects.groups.size() && gi < groups.size(); ++gi) {
      if (!Contains(group_rects.groups[gi].editor_surface, event.button.x, event.button.y)) {
        continue;
      }
      if (gi != context_.current_project_state.focused_group_index && groups[gi].has_active_tab() &&
          (groups[gi].active_tab().kind == TabEntry::Kind::Compare ||
           groups[gi].active_tab().kind == TabEntry::Kind::Merge)) {
        FocusEditorGroup(gi);
      }
      break;
    }
  }
  const SDL_FRect view_surface = ActiveTabSurface(layout);
  const bool in_view_surface = Contains(view_surface, event.button.x, event.button.y);

  if (ActiveTabIsCompare() && in_view_surface) {
    const bool handled = MakeCompareMouseCoordinator().HandleButtonDown(event, view_surface);
    if (handled) {
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    }
    return handled;
  }

  if (ActiveTabIsMerge() && in_view_surface) {
    const bool handled = MakeMergeMouseCoordinator().HandleButtonDown(event, view_surface);
    if (handled) {
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    }
    return handled;
  }

  if (HandleDebugToolbarButtonDown(event, layout)) {
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT) {
    const editor::WelcomeViewModel* welcome_model = nullptr;
    const editor::WelcomeLayout* welcome_layout = nullptr;
    if (ProbeWelcomeSurface(&welcome_model, &welcome_layout)) {
      const float click_x = static_cast<float>(event.button.x);
      const float click_y = static_cast<float>(event.button.y);
      for (const editor::WelcomeHitRegion& region : welcome_layout->hit_regions) {
        if (!Contains(region.rect, click_x, click_y)) {
          continue;
        }
        const auto run_action = [this](ActionId id) {
          ActionCoordinator(MakeActionContext()).Execute(id, {}, ActionSource::Menu);
        };
        switch (region.kind) {
          case editor::WelcomeHitRegion::Kind::RecentProject:
            if (region.recent_index < welcome_model->recent_projects.size()) {
              const editor::WelcomeRecent recent = welcome_model->recent_projects[region.recent_index];
              if (!recent.remote_folder.empty()) {
                ActionCoordinator(MakeActionContext())
                    .Execute(ActionId::RemoteOpenFolder, {recent.remote_folder}, ActionSource::Menu);
              } else {
                OpenProjectTab(recent.path, true, true);
              }
            }
            break;
          case editor::WelcomeHitRegion::Kind::OpenFolder:
            run_action(ActionId::ProjectOpen);
            break;
          case editor::WelcomeHitRegion::Kind::OpenRemoteFolder:
            run_action(ActionId::RemoteOpenFolder);
            break;
          case editor::WelcomeHitRegion::Kind::RecentFile:
            if (region.recent_index < welcome_model->recent_files.size()) {
              const std::filesystem::path file =
                  welcome_model->recent_files[region.recent_index].path;
              OpenFile(file);
            }
            break;
          case editor::WelcomeHitRegion::Kind::NewFile:
            run_action(ActionId::Tab);
            break;
          case editor::WelcomeHitRegion::Kind::OpenFile:
            run_action(ActionId::Open);
            break;
          case editor::WelcomeHitRegion::Kind::FindInProject:
            run_action(ActionId::ProjectSearch);
            break;
          case editor::WelcomeHitRegion::Kind::RemoteRetry:
            ActionCoordinator(MakeActionContext())
                .Execute(ActionId::RemoteReconnect, {welcome_model->remote.host}, ActionSource::Menu);
            break;
        }
        EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
        return true;
      }
    }
  }

  util::PerformanceTrace::Scope editor_scope("WorkspaceShell::HandleMouseButtonDown::Editor");
  const bool handled = MakeEditorMouseCoordinator().HandleButtonDown(event, layout);
  if (handled) {
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
  }
  return handled;
}

namespace {

thread_local struct WelcomeProbeLayoutCache {
  bool valid = false;
  std::uint64_t recents_instance = 0;
  std::uint64_t recents_revision = 0;
  std::uint64_t remote_revision = 0;
  std::filesystem::path root;
  SDL_FRect surface{};
  float line_height = 0.0f;
  editor::WelcomeLayout layout;
} g_welcome_probe_layout_cache;

}

bool WorkspaceShell::ProbeWelcomeSurface(const editor::WelcomeViewModel** model_out,
                                         const editor::WelcomeLayout** layout_out) const {
  if (model_out == nullptr || layout_out == nullptr) {
    return false;
  }
  const editor::TextViewport* viewport = ActiveEditorViewport();
  if (viewport == nullptr || !viewport->is_placeholder()) {
    return false;
  }
  const auto layout = CurrentWorkspaceLayout();
  if (!layout.has_value()) {
    return false;
  }
  const editor::WelcomeViewModel& model = RenderViewModelBuilder(context_).BuildWelcomeView(recents_service_);
  SDL_FRect surface = layout->editor_surface;
  for (const EditorPaneLayout& pane : ComputeEditorPaneLayouts(*layout)) {
    if (pane.active) {
      surface = pane.rect;
      break;
    }
  }
  const float line_height = editor_text_renderer_.LineHeight();
  WelcomeProbeLayoutCache& cache = g_welcome_probe_layout_cache;
  if (!cache.valid || cache.recents_instance != recents_service_.instance_id() ||
      cache.recents_revision != recents_service_.revision() ||
      cache.remote_revision != context_.current_project_state.remote_status_revision ||
      cache.root != context_.current_project_state.root || !RectsEqual(cache.surface, surface) ||
      cache.line_height != line_height) {
    cache.valid = true;
    cache.recents_instance = recents_service_.instance_id();
    cache.recents_revision = recents_service_.revision();
    cache.remote_revision = context_.current_project_state.remote_status_revision;
    cache.root = context_.current_project_state.root;
    cache.surface = surface;
    cache.line_height = line_height;
    cache.layout = editor::ComputeWelcomeLayout(surface, model, line_height);
  }
  *model_out = &model;
  *layout_out = &cache.layout;
  return true;
}

void WorkspaceShell::AbandonPointerGestures() {
  ClearDragState();
  context_.interaction_state.single_line_drag_surface = TextInputSurface::None;
  context_.interaction_state.mouse_selecting = false;
  context_.interaction_state.text_drag = InteractionState::TextDragState::None;
  context_.interaction_state.text_drag_has_drop = false;
  context_.interaction_state.editor_box_selecting = false;
  selection_autoscroll::Disarm(context_.interaction_state);
  if (TerminalPaneState* terminal = ActiveTerminalPane(); terminal != nullptr) {
    terminal->mouse_selecting = false;
  }
  (void)MakeSidebarMouseCoordinator().CancelPathDrag();
  (void)MakeTabMouseCoordinator().CancelDrag();
}

bool WorkspaceShell::HandleMouseButtonUp(const SDL_Event& event) {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::HandleMouseButtonUp");
  UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y));
  const DamageMark mark = MarkDamage();

  if (context_.prompts.dirty_visible || context_.prompts.surface_visible) {
    if (event.button.button == SDL_BUTTON_LEFT) {
      AbandonPointerGestures();
    }
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT && context_.interaction_state.path_drag.armed) {
    (void)MakeSidebarMouseCoordinator().FinishPathDrag((context_.interaction_state.key_modifiers & SDL_KMOD_CTRL) != 0);
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT && context_.interaction_state.tab_drag.kind != TabDragKind::None) {
    if (MakeTabMouseCoordinator().HandleButtonUp(event)) {
      EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
      return true;
    }
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  if (MakePanelMouseCoordinator().HandleButtonUp(event)) {
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }

  if (event.button.button != SDL_BUTTON_LEFT) {
    return false;
  }
  if (context_.interaction_state.drag_target == DragTarget::SettingsScrollbar ||
      context_.interaction_state.drag_target == DragTarget::SettingsCategoryScrollbar ||
      context_.interaction_state.drag_target == DragTarget::SettingsPickerScrollbar) {
    ClearDragState();
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }
  if (context_.interaction_state.drag_target == DragTarget::SingleLineSelection) {
    const TextInputSurface surface = context_.interaction_state.single_line_drag_surface;
    context_.interaction_state.drag_target = DragTarget::None;
    context_.interaction_state.single_line_drag_surface = TextInputSurface::None;
    UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y));
    RequestSingleLineDragSurfaceRedraw(mark, surface);
    return true;
  }
  if (context_.interaction_state.drag_target != DragTarget::None) {
    ClearDragState();
    context_.interaction_state.mouse_selecting = false;
    context_.interaction_state.text_drag = InteractionState::TextDragState::None;
    context_.interaction_state.text_drag_has_drop = false;
    selection_autoscroll::Disarm(context_.interaction_state);
    UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y));
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
    return true;
  }
  const bool was_selecting = context_.interaction_state.mouse_selecting;
  const bool finished_text_drag = MakeEditorMouseCoordinator().FinishTextDrag(
      (context_.interaction_state.key_modifiers & SDL_KMOD_CTRL) != 0);
  context_.interaction_state.mouse_selecting = false;
  context_.interaction_state.editor_box_selecting = false;
  selection_autoscroll::Disarm(context_.interaction_state);
  if (was_selecting) {
    SyncPrimarySelectionWithActiveEditor();
    EnsureRedraw(mark, [this]() { RequestPreparedFrame(); });
  }
  return was_selecting || finished_text_drag;
}

std::optional<editor::CodeLensDecoration> WorkspaceShell::AboveLensAtPosition(float x,
                                                                              float y) const {
  if (!ActiveTabIsEditor()) {
    return std::nullopt;
  }
  const bool inline_surfaces = SettingFlagEnabled(GetSettingValue("plugins.inline_surfaces"));
  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return std::nullopt;
  }
  const WorkspaceLayout layout = *layout_state;

  const auto* pres = context_.current_project_state.plugin_presentation_if_present();
  if (pres == nullptr) {
    return std::nullopt;
  }
  const auto resolve = [&](const editor::TextViewport& viewport,
                           const SDL_FRect& rect) -> std::optional<editor::CodeLensDecoration> {
    if (viewport.is_placeholder() || viewport.path().empty() || viewport.dirty()) {
      return std::nullopt;
    }
    const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
        editor_text_renderer_, viewport, rect, viewport.sticky_band_rows(), LineNumbersEnabled());
    const editor::InsetGapOptions options{.inline_surfaces = inline_surfaces,
                                          .code_lens_above = true,
                                          .code_lens_height = metrics.line_height};
    const editor::InsetClickResult result = editor::ResolveInsetClick(
        pres->surfaces, pres->decorations, viewport, metrics.first_line_y,
        metrics.line_height, metrics.visible_rows, y, options);
    if (result.gap_content.code_lens != nullptr && result.gap_content.code_lens->activatable()) {
      return *result.gap_content.code_lens;
    }
    return std::nullopt;
  };

  const auto panes = ComputeEditorPaneLayouts(layout);
  const editor::TextViewport* active_viewport = ActiveEditorViewport();
  if (panes.empty() && active_viewport != nullptr) {
    return resolve(*active_viewport, layout.editor_surface);
  }
  for (const EditorPaneLayout& pane : panes) {
    if (!Contains(pane.rect, x, y)) {
      continue;
    }
    const editor::TextViewport* viewport = ViewportForPane(pane);
    if (viewport == nullptr) {
      return std::nullopt;
    }
    return resolve(*viewport, pane.rect);
  }
  return std::nullopt;
}

}
