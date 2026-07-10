#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>

#include "editor/EditorInsetLayout.h"
#include "editor/EditorRowYLayout.h"
#include "editor/EditorViewRenderer.h"
#include "editor/PluginDecorationStore.h"
#include "editor/WelcomeView.h"
#include "util/PerformanceTrace.h"
#include "workspace/RenderViewModelBuilder.h"
#include "workspace/SettingFlags.h"
#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceCompareMouseCoordinator.h"
#include "workspace/WorkspaceChromeMouseCoordinator.h"
#include "workspace/WorkspaceEditorMouseCoordinator.h"
#include "workspace/WorkspaceMenuCoordinator.h"
#include "workspace/WorkspaceMergeMouseCoordinator.h"
#include "workspace/DebugPaneMouseCoordinator.h"
#include "workspace/WorkspacePanelMouseCoordinator.h"
#include "workspace/WorkspaceSidebarMouseCoordinator.h"
#include "workspace/WorkspaceTabMouseCoordinator.h"

namespace microide::workspace {

bool WorkspaceShell::HandleMouseButtonDown(const SDL_Event& event) {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::HandleMouseButtonDown");
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
  const auto invalidate_menu_blocked_hover_visuals = [this, &layout]() {
    if (const auto rect = HoveredProjectTabTooltipRect(layout); rect.has_value()) {
      RequestRedrawRect(*rect);
    }
    if (const auto rect = HoveredTabTooltipRect(layout); rect.has_value()) {
      RequestRedrawRect(*rect);
    }
    if (const auto rect = HoveredStatusTooltipRect(layout); rect.has_value()) {
      RequestRedrawRect(*rect);
    }
    if (const auto rect = HoveredGitSidebarTooltipRect(layout); rect.has_value()) {
      RequestRedrawRect(*rect);
    }
    if (const auto popup = ActiveEditorHoverPopupLayout(); popup.has_value()) {
      RequestRedrawRect(popup->rect);
    }
  };

  if (MenuSurfaceCapturingMouse()) {
    invalidate_menu_blocked_hover_visuals();
    UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y), false);
    if (MakeChromeMouseCoordinator().HandleButtonDown(event, layout)) {
      EnsureRedraw([this]() { RequestChromeRedraw(); });
      return true;
    }
    EnsureRedraw([this]() { RequestChromeRedraw(); });
    return true;
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
      // Open the code-action menu targeted at the diagnostic's range so the fix
      // list reflects this diagnostic (there may be several actions).
      const editor::SelectionRange range = visible_hover_popup->diagnostic->range;
      active_editor_hover_target_.reset();
      assist_service_.ShowCodeActionsOverlay(nullptr, &range);
      EnsureRedraw([this]() { RequestWindowRedraw(); });
      return true;
    }
    context_.current_project_state.surface.focus = FocusTarget::Editor;
    EnsureRedraw([this]() { RequestEditorSurfaceRedraw(); });
    return true;
  }
  UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y));

  if (context_.prompts.dirty_visible) {
    const SDL_FRect dialog = ComputeDirtyPromptRect(*window_rect);
    const auto buttons = ComputeDirtyPromptButtonRects(dialog);
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      if (Contains(buttons[i], event.button.x, event.button.y)) {
        context_.prompts.dirty.selected_action = static_cast<int>(i);
        ConfirmDirtyPrompt();
        EnsureRedraw([this]() { RequestPromptRedraw(); });
        return true;
      }
    }
    EnsureRedraw([this]() { RequestPromptRedraw(); });
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
        EnsureRedraw([this]() { RequestPromptRedraw(); });
        return true;
      }
    }
    if (HandleSingleLineInputMouseDown(event, layout)) {
      EnsureRedraw([this]() { RequestPromptRedraw(); });
      return true;
    }
    EnsureRedraw([this]() { RequestPromptRedraw(); });
    return true;
  }

  if (HandleSingleLineInputMouseDown(event, layout)) {
    switch (context_.interaction_state.single_line_drag_surface) {
      case TextInputSurface::SidebarSearchQuery:
      case TextInputSurface::SidebarSearchReplace:
      case TextInputSurface::CommitSubject:
        EnsureRedraw([this]() { RequestSidebarRedraw(); });
        break;
      case TextInputSurface::FileFinder:
      case TextInputSurface::BufferSearch:
      case TextInputSurface::BufferReplaceSearch:
      case TextInputSurface::BufferReplaceReplace:
      case TextInputSurface::ProjectSearchOverlay:
      case TextInputSurface::CommitPicker:
        EnsureRedraw([this]() { RequestOverlayRedraw(); });
        break;
      default:
        EnsureRedraw([this]() { RequestWindowRedraw(); });
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

  if (HandleSettingsOverlayButtonDown(event, layout)) {
    EnsureRedraw([this]() { RequestOverlayRedraw(); });
    return true;
  }

  const auto blocked_project_tab_tooltip_rect = HoveredProjectTabTooltipRect(layout);
  const auto blocked_tab_tooltip_rect = HoveredTabTooltipRect(layout);
  const auto blocked_status_tooltip_rect = HoveredStatusTooltipRect(layout);
  const auto blocked_git_sidebar_tooltip_rect = HoveredGitSidebarTooltipRect(layout);
  const auto blocked_editor_hover_popup_rect = [&]() -> std::optional<SDL_FRect> {
    if (const auto popup = ActiveEditorHoverPopupLayout(); popup.has_value()) {
      return popup->rect;
    }
    return std::nullopt;
  }();
  if (MakeChromeMouseCoordinator().HandleButtonDown(event, layout)) {
    if (MenuSurfaceCapturingMouse()) {
      if (blocked_project_tab_tooltip_rect.has_value()) {
        RequestRedrawRect(*blocked_project_tab_tooltip_rect);
      }
      if (blocked_tab_tooltip_rect.has_value()) {
        RequestRedrawRect(*blocked_tab_tooltip_rect);
      }
      if (blocked_status_tooltip_rect.has_value()) {
        RequestRedrawRect(*blocked_status_tooltip_rect);
      }
      if (blocked_git_sidebar_tooltip_rect.has_value()) {
        RequestRedrawRect(*blocked_git_sidebar_tooltip_rect);
      }
      if (blocked_editor_hover_popup_rect.has_value()) {
        RequestRedrawRect(*blocked_editor_hover_popup_rect);
      }
      UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y), false);
      EnsureRedraw([this]() { RequestChromeRedraw(); });
    } else {
      EnsureRedraw([this]() { RequestWindowRedraw(); });
    }
    return true;
  }

  // A plugin breadcrumb status item with a bound command dispatches it on click.
  if (event.button.button == SDL_BUTTON_LEFT && !MenuSurfaceCapturingMouse()) {
    for (const VisibleStatusItem& status_item : ComputeVisibleStatusItems(layout.breadcrumb)) {
      if (!status_item.item.command.empty() &&
          Contains(status_item.rect, event.button.x, event.button.y)) {
        std::string error_message;
        ExecuteCommandName(status_item.item.command, {}, ActionSource::Command, &error_message);
        EnsureRedraw([this]() { RequestChromeRedraw(); });
        return true;
      }
    }
  }

  if (event.button.button == SDL_BUTTON_LEFT) {
    // A plugin code-lens click dispatches its bound command. Checked before the
    // blame overlay (both anchor at end-of-line) so the actionable lens wins.
    // With `plugins.code_lens_above` the lens renders as an inset strip above its
    // line, so its click geometry comes from the gap layout instead of the EOL one.
    const bool code_lens_above = SettingFlagEnabled(GetSettingValue("plugins.code_lens_above"));
    const auto command =
        code_lens_above ? AboveLensCommandAtPosition(static_cast<float>(event.button.x),
                                                     static_cast<float>(event.button.y))
                        : CodeLensCommandAtPosition(static_cast<float>(event.button.x),
                                                    static_cast<float>(event.button.y));
    if (command.has_value()) {
      context_.current_project_state.surface.focus = FocusTarget::Editor;
      std::string error_message;
      ExecuteCommandName(*command, {}, ActionSource::Command, &error_message);
      EnsureRedraw([this]() { RequestWindowRedraw(); });
      return true;
    }
    if (editor_blame_overlay_service_.LineAtPosition(static_cast<float>(event.button.x),
                                                     static_cast<float>(event.button.y)) != nullptr) {
      context_.current_project_state.surface.focus = FocusTarget::Editor;
      EnsureRedraw([this]() { RequestEditorSurfaceRedraw(); });
      return true;
    }
  }

  if (event.button.button == SDL_BUTTON_LEFT && context_.current_project_state.sidebar.visible &&
      Contains(SidebarResizeHitRect(layout), event.button.x, event.button.y)) {
    context_.interaction_state.drag_target = DragTarget::SidebarDivider;
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT && context_.current_project_state.debug_pane.visible &&
      Contains(RightPaneResizeHitRect(layout), event.button.x, event.button.y)) {
    context_.interaction_state.drag_target = DragTarget::RightPaneDivider;
    return true;
  }

  // Editor-group split divider: grab it to resize the two groups.
  if (event.button.button == SDL_BUTTON_LEFT) {
    for (const EditorSplitDividerLayout& divider :
         ComputeEditorSplitDividerLayouts(layout.editor_surface)) {
      // Grab region == cursor region: the resize cursor is shown over divider.rect
      // exactly (see CursorKindForPosition), so the drag starts in the same span and
      // does not extend past where the cursor changes.
      if (Contains(divider.rect, event.button.x, event.button.y)) {
        context_.interaction_state.drag_target = DragTarget::EditorSplitDivider;
        context_.interaction_state.drag_editor_split_divider_index = divider.divider_index;
        context_.interaction_state.drag_editor_split_path = divider.node_path;
        return true;
      }
    }
  }

  if (MakeDebugPaneMouseCoordinator().HandleButtonDown(event, layout)) {
    EnsureRedraw([this]() { RequestDebugPaneRedraw(); });
    return true;
  }

  if (MakePanelMouseCoordinator().HandleResizeButtonDown(event, layout)) {
    EnsureRedraw([this]() { RequestBottomPanelRedraw(); });
    return true;
  }

  if (MakeSidebarMouseCoordinator().HandleButtonDown(event, layout)) {
    EnsureRedraw([this]() { RequestSidebarRedraw(); });
    return true;
  }

  {
    util::PerformanceTrace::Scope tab_scope("WorkspaceShell::HandleMouseButtonDown::Tabs");
    if (HandleTabMouseButtonDown(event, layout)) {
      EnsureRedraw([this]() { RequestWindowRedraw(); });
      return true;
    }
  }

  if (MakePanelMouseCoordinator().HandleButtonDown(event, layout)) {
    EnsureRedraw([this]() { RequestBottomPanelRedraw(); });
    return true;
  }

  // Breakpoint gutter right-click opens the breakpoint context menu (Phase 6),
  // taking precedence over the editor context menu. Declines (returns false)
  // for any non-gutter right-click, so the editor context menu still works.
  if (event.button.button == SDL_BUTTON_RIGHT && ActiveTabIsEditor() &&
      Contains(layout.editor_surface, event.button.x, event.button.y)) {
    SyncActiveEditorTab();
    if (MakeEditorMouseCoordinator().HandleGutterContextMenu(event, layout)) {
      EnsureRedraw([this]() { RequestChromeRedraw(); });
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

      const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
      const auto pane_it = std::find_if(
          panes.begin(), panes.end(),
          [&](const EditorPaneLayout& pane) {
            return Contains(pane.rect, event.button.x, event.button.y);
          });
      if (pane_it == panes.end()) {
        return false;
      }

      // Right-clicking a split group focuses it first (like left-click) so the
      // context menu and the retargeted caret act on the clicked group, not the
      // previously-focused one.
      auto& project_state = context_.current_project_state;
      if (pane_it->group_index != project_state.focused_group_index &&
          pane_it->group_index < project_state.editor_groups.size()) {
        project_state.focused_group_index = pane_it->group_index;
        RequestTabStripRedraw();
      }

      editor::TextViewport* viewport = ActiveEditorViewport();
      if (viewport == nullptr) {
        return false;
      }

      const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
          text_renderer_, *viewport, pane_it->rect, 0, LineNumbersEnabled());
      viewport->SetViewportSize(metrics.visible_rows, metrics.visible_columns);

      const auto setting_on = [this](std::string_view id) {
        return SettingFlagEnabled(GetSettingValue(id));
      };
      // No plugin/LSP contribution: no insets, so the gap-aware mapping collapses
      // to the legacy row formula. Skip the store probing entirely.
      const auto* pres = context_.current_project_state.plugin_presentation_if_present();
      std::size_t row = 0;
      if (pres == nullptr) {
        row = editor::EditorRowYLayout(metrics.first_line_y, metrics.line_height,
                                       static_cast<std::uint32_t>(viewport->scroll_line()))
                  .HitTest(event.button.y, metrics.visible_rows)
                  .row;
      } else {
        const editor::InsetGapOptions inset_options{
            .inline_surfaces = setting_on("plugins.inline_surfaces"),
            .code_lens_above = setting_on("plugins.code_lens_above"),
            .code_lens_height = metrics.line_height};
        row = editor::ResolveInsetClick(pres->surfaces, pres->decorations, *viewport,
                                        metrics.first_line_y, metrics.line_height,
                                        metrics.visible_rows, event.button.y, inset_options)
                  .hit.row;
      }
      const float text_offset_x = std::max(0.0f, event.button.x - metrics.text_x);
      std::size_t visual_column =
          viewport->horizontal_scroll() +
          static_cast<std::size_t>(std::max(
              0L, std::lround(text_offset_x / std::max(1.0f, text_renderer_.CharWidth()))));
      const int visual_row = static_cast<int>(viewport->scroll_line() + row);
      // Undo the mid-line inlay-hint display shift so the retargeted caret lands on
      // the glyph under the cursor (identity when no hints / soft-wrapped).
      if (pres != nullptr && !viewport->soft_wrap() &&
          static_cast<std::size_t>(visual_row) < viewport->visual_line_count()) {
        const std::size_t line_index =
            viewport->VisualRowLineIndex(static_cast<std::size_t>(visual_row));
        const editor::FileDecorations* file_dec =
            pres->decorations.FindByPath(viewport->path());
        const auto inline_texts =
            file_dec != nullptr ? file_dec->InlineTextsForLine(static_cast<std::uint32_t>(line_index))
                                : std::span<const editor::InlineTextDecoration>{};
        if (!inline_texts.empty()) {
          const editor::LayoutLine& layout = viewport->VisibleLineLayoutRef(line_index);
          const std::size_t row_start = viewport->horizontal_scroll();
          visual_column = editor::RealVisualColumnForDisplayColumn(
              inline_texts, &layout, nullptr, row_start, row_start + viewport->visible_columns(),
              text_renderer_, text_renderer_.CharWidth(), visual_column);
        }
      }
      const int visual_row_hit = visual_row;
      const editor::LogicalPosition hit =
          viewport->LogicalPositionForVisualHit(visual_row_hit, static_cast<int>(visual_column));
      viewport->MoveCursorToVisualColumn(hit.line, visual_column, false);
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
    EnsureRedraw([this, retargeted_cursor]() {
      RequestChromeRedraw();
      if (retargeted_cursor) {
        RequestFocusedEditorRedraw();
      }
    });
    return true;
  }

  if (!Contains(layout.editor_surface, event.button.x, event.button.y) ||
      (event.button.button != SDL_BUTTON_LEFT && event.button.button != SDL_BUTTON_MIDDLE)) {
    return false;
  }

  if (ActiveTabIsCompare()) {
    const bool handled = MakeCompareMouseCoordinator().HandleButtonDown(event, layout);
    if (handled) {
      EnsureRedraw([this]() { RequestEditorSurfaceRedraw(); });
    }
    return handled;
  }

  if (ActiveTabIsMerge()) {
    const bool handled = MakeMergeMouseCoordinator().HandleButtonDown(event, layout);
    if (handled) {
      EnsureRedraw([this]() { RequestEditorSurfaceRedraw(); });
    }
    return handled;
  }

  // Floating debug control bar sits over the top-right of the editor; intercept
  // its clicks before the editor coordinator turns them into text selection.
  if (HandleDebugToolbarButtonDown(event, layout)) {
    EnsureRedraw([this]() { RequestEditorSurfaceRedraw(); });
    return true;
  }

  // Welcome home surface: a placeholder editor shows clickable recent projects/files plus
  // primary-action buttons (open folder, or new/open/find when a project is open).
  // Intercept those clicks before the editor coordinator turns them into a text-selection
  // drag on the empty buffer.
  if (event.button.button == SDL_BUTTON_LEFT) {
    editor::WelcomeViewModel welcome_model;
    editor::WelcomeLayout welcome_layout;
    if (ProbeWelcomeSurface(&welcome_model, &welcome_layout)) {
      const float click_x = static_cast<float>(event.button.x);
      const float click_y = static_cast<float>(event.button.y);
      for (const editor::WelcomeHitRegion& region : welcome_layout.hit_regions) {
        if (!Contains(region.rect, click_x, click_y)) {
          continue;
        }
        const auto run_action = [this](ActionId id) {
          ActionCoordinator(MakeActionContext()).Execute(id, {}, ActionSource::Menu);
        };
        switch (region.kind) {
          case editor::WelcomeHitRegion::Kind::RecentProject:
            if (region.recent_index < welcome_model.recent_projects.size()) {
              OpenProjectTab(welcome_model.recent_projects[region.recent_index].path, true, true);
            }
            break;
          case editor::WelcomeHitRegion::Kind::OpenFolder:
            run_action(ActionId::ProjectOpen);
            break;
          case editor::WelcomeHitRegion::Kind::RecentFile:
            if (region.recent_index < welcome_model.recent_files.size()) {
              OpenFile(welcome_model.recent_files[region.recent_index].path);
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
        }
        EnsureRedraw([this]() { RequestEditorSurfaceRedraw(); });
        return true;
      }
    }
  }

  util::PerformanceTrace::Scope editor_scope("WorkspaceShell::HandleMouseButtonDown::Editor");
  const bool handled = MakeEditorMouseCoordinator().HandleButtonDown(event, layout);
  if (handled) {
    EnsureRedraw([this]() { RequestEditorSurfaceRedraw(); });
  }
  return handled;
}

bool WorkspaceShell::ProbeWelcomeSurface(editor::WelcomeViewModel* model,
                                         editor::WelcomeLayout* layout_out) const {
  if (model == nullptr || layout_out == nullptr) {
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
  *model = RenderViewModelBuilder(context_).BuildWelcomeView(recents_service_);
  *layout_out = editor::ComputeWelcomeLayout(layout->editor_surface, *model,
                                             text_renderer_.LineHeight());
  return true;
}

bool WorkspaceShell::HandleMouseButtonUp(const SDL_Event& event) {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::HandleMouseButtonUp");
  UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y));

  if (context_.prompts.dirty_visible) {
    EnsureRedraw([this]() { RequestPromptRedraw(); });
    return true;
  }
  if (context_.prompts.surface_visible) {
    EnsureRedraw([this]() { RequestPromptRedraw(); });
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT && context_.interaction_state.tab_drag.kind != TabDragKind::None) {
    if (HandleTabMouseButtonUp(event)) {
      EnsureRedraw([this]() { RequestWindowRedraw(); });
      return true;
    }
    EnsureRedraw([this]() { RequestWindowRedraw(); });
    return true;
  }

  if (MakePanelMouseCoordinator().HandleButtonUp(event)) {
    EnsureRedraw([this]() { RequestBottomPanelRedraw(); });
    return true;
  }

  if (event.button.button != SDL_BUTTON_LEFT) {
    return false;
  }
  if (context_.interaction_state.drag_target == DragTarget::SettingsScrollbar ||
      context_.interaction_state.drag_target == DragTarget::SettingsCategoryScrollbar) {
    context_.interaction_state.drag_target = DragTarget::None;
    EnsureRedraw([this]() { RequestOverlayRedraw(); });
    return true;
  }
  if (context_.interaction_state.drag_target == DragTarget::SingleLineSelection) {
    const TextInputSurface surface = context_.interaction_state.single_line_drag_surface;
    context_.interaction_state.drag_target = DragTarget::None;
    context_.interaction_state.single_line_drag_surface = TextInputSurface::None;
    UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y));
    switch (surface) {
      case TextInputSurface::PromptInput:
        EnsureRedraw([this]() { RequestPromptRedraw(); });
        break;
      case TextInputSurface::SidebarSearchQuery:
      case TextInputSurface::SidebarSearchReplace:
        EnsureRedraw([this]() { RequestSidebarRedraw(); });
        break;
      default:
        EnsureRedraw([this]() { RequestOverlayRedraw(); });
        break;
    }
    return true;
  }
  if (context_.interaction_state.drag_target != DragTarget::None) {
    ClearDragState();
    context_.interaction_state.mouse_selecting = false;
    UpdateMouseCursor(static_cast<float>(event.button.x), static_cast<float>(event.button.y));
    EnsureRedraw([this]() { RequestWindowRedraw(); });
    return true;
  }
  const bool was_selecting = context_.interaction_state.mouse_selecting;
  context_.interaction_state.mouse_selecting = false;
  if (was_selecting) {
    SyncPrimarySelectionWithActiveEditor();
    EnsureRedraw([this]() { RequestEditorSurfaceRedraw(); });
  }
  return was_selecting;
}

std::optional<std::string> WorkspaceShell::AboveLensCommandAtPosition(float x, float y) const {
  if (!ActiveTabIsEditor()) {
    return std::nullopt;
  }
  const bool inline_surfaces = SettingFlagEnabled(GetSettingValue("plugins.inline_surfaces"));
  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return std::nullopt;
  }
  const WorkspaceLayout layout = *layout_state;

  // No plugin/LSP contribution: no above-line code lenses can exist.
  const auto* pres = context_.current_project_state.plugin_presentation_if_present();
  if (pres == nullptr) {
    return std::nullopt;
  }
  const auto resolve = [&](const editor::TextViewport& viewport,
                           const SDL_FRect& rect) -> std::optional<std::string> {
    if (viewport.is_placeholder() || viewport.path().empty() || viewport.dirty()) {
      return std::nullopt;
    }
    const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
        text_renderer_, viewport, rect, 0, LineNumbersEnabled());
    const editor::InsetGapOptions options{.inline_surfaces = inline_surfaces,
                                          .code_lens_above = true,
                                          .code_lens_height = metrics.line_height};
    const editor::InsetClickResult result = editor::ResolveInsetClick(
        pres->surfaces, pres->decorations, viewport, metrics.first_line_y,
        metrics.line_height, metrics.visible_rows, y, options);
    if (result.gap_content.code_lens != nullptr &&
        !result.gap_content.code_lens->command.empty()) {
      return result.gap_content.code_lens->command;
    }
    return std::nullopt;
  };

  const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
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

}  // namespace microide::workspace
