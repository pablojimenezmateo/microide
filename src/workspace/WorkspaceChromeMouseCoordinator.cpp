#include "workspace/WorkspaceChromeMouseCoordinator.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

#include "util/PerformanceTrace.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/WorkspaceMenuCoordinator.h"

namespace microide::workspace {
namespace {

void RequestMenuHoverRowRedraw(
    const std::function<void(const SDL_FRect&)>& request_redraw_rect,
    const std::optional<SDL_FRect>& rect) {
  if (!rect.has_value()) {
    return;
  }
  request_redraw_rect(
      MakeRect(rect->x - 1.0f, rect->y - 1.0f, rect->w + 2.0f, rect->h + 2.0f));
}

bool OptionalRectsEqual(const std::optional<SDL_FRect>& lhs,
                        const std::optional<SDL_FRect>& rhs) {
  if (!lhs.has_value() || !rhs.has_value()) {
    return lhs.has_value() == rhs.has_value();
  }
  return RectsEqual(*lhs, *rhs);
}

}  // namespace

ChromeMouseCoordinator::ChromeMouseCoordinator(ProjectWorkspaceState& state,
                                               MenuSurfaceState& menu_state,
                                               InteractionState& interaction_state,
                                               Operations operations)
    : state_(state),
      menu_state_(menu_state),
      interaction_state_(interaction_state),
      operations_(std::move(operations)) {}

bool ChromeMouseCoordinator::HandleButtonDown(const SDL_Event& event,
                                              const WorkspaceLayout& layout) {
  if (HandleTreeContextMenuButtonDown(event)) {
    return true;
  }

  if (menu_state_.menu_bar_open && event.button.button != SDL_BUTTON_LEFT) {
    operations_.close_menu_bar();
  }

  if (event.button.button == SDL_BUTTON_LEFT && state_.sidebar.visible) {
    const SDL_FRect sidebar_mode_rect = operations_.sidebar_mode_control_rect(layout.sidebar);
    if (Contains(sidebar_mode_rect, event.button.x, event.button.y)) {
      if (menu_state_.menu_bar_open && menu_state_.active_menu_id == MenuId::SidebarMode &&
          menu_state_.active_menu_anchor_rect.has_value()) {
        operations_.close_menu_bar();
      } else {
        operations_.open_anchored_menu(MenuId::SidebarMode, sidebar_mode_rect);
      }
      state_.surface.focus = FocusTarget::Sidebar;
      operations_.request_chrome_redraw();
      return true;
    }
  }

  if (HandleMenuButtonDown(event, layout)) {
    return true;
  }

  if (HandleOverlayButtonDown(event, layout)) {
    return true;
  }

  return false;
}

bool ChromeMouseCoordinator::HandleMotion(const SDL_Event& event, const WorkspaceLayout& layout) {
  if (HandleTreeContextMenuMotion(event)) {
    return true;
  }

  return HandleMenuMotion(event, layout);
}

bool ChromeMouseCoordinator::HandleWheel(const SDL_Event& event,
                                         const WorkspaceLayout& layout,
                                         int vertical_ticks,
                                         int horizontal_ticks) {
  if (!state_.overlay.visible) {
    return false;
  }

  const int overlay_ticks = vertical_ticks != 0 ? vertical_ticks : horizontal_ticks;
  // The find/replace widget is non-modal: only consume the wheel when the pointer
  // is over it (to cycle matches); otherwise let the editor scroll normally.
  if (state_.overlay.mode == OverlayMode::BufferSearch ||
      state_.overlay.mode == OverlayMode::BufferReplace) {
    const FindWidgetLayout fw = ComputeFindWidgetLayout(
        layout.editor_area, state_.overlay.mode == OverlayMode::BufferReplace);
    if (!Contains(fw.widget, event.wheel.mouse_x, event.wheel.mouse_y)) {
      return false;
    }
    operations_.move_buffer_search_selection(-overlay_ticks);
    operations_.request_overlay_redraw();
    return true;
  }
  if (state_.overlay.mode == OverlayMode::CommitPicker) {
    operations_.move_compare_picker_selection(-overlay_ticks);
  } else if (state_.overlay.mode == OverlayMode::ProjectSearch) {
    // Detached scroll: the wheel pans the results list without moving the active
    // selection, so the highlighted result can scroll off-screen instead of the
    // view snapping to keep it visible.
    const SDL_FRect overlay = operations_.compute_overlay_rect(layout.editor_area);
    const auto list_layout = operations_.compute_overlay_list_layout(overlay);
    state_.overlay.scroll_row =
        std::clamp(state_.overlay.scroll_row - overlay_ticks, 0, list_layout.max_scroll);
  } else if (state_.overlay.mode == OverlayMode::Completion ||
             state_.overlay.mode == OverlayMode::CodeActions) {
    if (state_.overlay.mode == OverlayMode::Completion &&
        !state_.overlay.workflow.completion.items.empty()) {
      const int current = static_cast<int>(state_.overlay.workflow.completion.selected_index);
      const int max_index =
          static_cast<int>(state_.overlay.workflow.completion.items.size()) - 1;
      state_.overlay.workflow.completion.selected_index =
          static_cast<std::size_t>(std::clamp(current - overlay_ticks, 0, max_index));
    } else if (state_.overlay.mode == OverlayMode::CodeActions &&
               !state_.overlay.workflow.code_actions.items.empty()) {
      const int current = static_cast<int>(state_.overlay.workflow.code_actions.selected_index);
      const int max_index =
          static_cast<int>(state_.overlay.workflow.code_actions.items.size()) - 1;
      state_.overlay.workflow.code_actions.selected_index =
          static_cast<std::size_t>(std::clamp(current - overlay_ticks, 0, max_index));
    }
  } else {
    operations_.move_file_finder_selection(-overlay_ticks);
  }
  operations_.request_overlay_redraw();
  return true;
}

bool ChromeMouseCoordinator::HandleMenuButtonDown(const SDL_Event& event,
                                                  const WorkspaceLayout& layout) {
  if (event.button.button != SDL_BUTTON_LEFT) {
    return false;
  }

  const auto menu_bar_items = operations_.compute_visible_menu_bar_items(layout.menu_bar);
  const auto window_buttons = operations_.compute_visible_window_control_buttons(layout.menu_bar);
  for (const auto& button : window_buttons) {
    if (!Contains(WindowControlButtonHitRect(button.rect), event.button.x, event.button.y)) {
      continue;
    }
    operations_.close_menu_bar();
    switch (button.id) {
      case WorkspaceShell::WindowControlButtonId::Minimize:
        operations_.set_pending_window_action(WorkspaceShell::WindowAction::Minimize);
        break;
      case WorkspaceShell::WindowControlButtonId::Maximize:
        operations_.set_pending_window_action(WorkspaceShell::WindowAction::ToggleMaximize);
        break;
      case WorkspaceShell::WindowControlButtonId::Close:
        operations_.request_quit();
        break;
    }
    operations_.request_chrome_redraw();
    return true;
  }

  if (const auto chevron_rect = operations_.menu_overflow_chevron_rect(layout.menu_bar);
      chevron_rect.has_value() &&
      Contains(*chevron_rect, event.button.x, event.button.y)) {
    if (menu_state_.overflow_popup_open) {
      menu_state_.overflow_popup_open = false;
      menu_state_.overflow_popup_anchor_rect.reset();
    } else {
      operations_.close_menu_bar();
      menu_state_.overflow_popup_open = true;
      menu_state_.overflow_popup_anchor_rect = *chevron_rect;
      menu_state_.overflow_popup_active_index = -1;
    }
    operations_.request_chrome_redraw();
    return true;
  }

  if (menu_state_.overflow_popup_open && menu_state_.overflow_popup_anchor_rect.has_value()) {
    const auto overflow_specs = operations_.compute_overflow_menu_bar_items(layout.menu_bar);
    const SDL_FRect popup = ComputeMenuOverflowPopupRect(*menu_state_.overflow_popup_anchor_rect,
                                                          overflow_specs.size());
    if (Contains(popup, event.button.x, event.button.y)) {
      const std::size_t row = static_cast<std::size_t>(
          std::floor((event.button.y - popup.y - 4.0f) / kWorkspaceMenuPopupItemHeight));
      if (row < overflow_specs.size()) {
        const MenuId picked = overflow_specs[row];
        const SDL_FRect row_rect =
            MakeRect(popup.x + 4.0f,
                     popup.y + 4.0f + static_cast<float>(row) * kWorkspaceMenuPopupItemHeight,
                     popup.w - 8.0f, kWorkspaceMenuPopupItemHeight);
        menu_state_.overflow_popup_open = false;
        menu_state_.overflow_popup_anchor_rect.reset();
        operations_.open_anchored_menu(picked, row_rect);
        operations_.request_chrome_redraw();
      }
      return true;
    }
    menu_state_.overflow_popup_open = false;
    menu_state_.overflow_popup_anchor_rect.reset();
    operations_.request_chrome_redraw();
  }

  if (menu_state_.menu_bar_open) {
    for (const auto& item : menu_bar_items) {
      if (!Contains(item.rect, event.button.x, event.button.y)) {
        continue;
      }
      if (item.id == menu_state_.active_menu_id) {
        operations_.close_menu_bar();
      } else {
        operations_.open_menu_bar_menu(item.id);
      }
      operations_.request_chrome_redraw();
      return true;
    }

    if (const auto submenu_rect = operations_.active_submenu_rect(layout.menu_bar);
        submenu_rect.has_value() && Contains(*submenu_rect, event.button.x, event.button.y)) {
      for (const auto& item :
           operations_.compute_visible_popup_menu_items(menu_state_.active_submenu_id, *submenu_rect)) {
        if (!Contains(item.rect, event.button.x, event.button.y)) {
          continue;
        }
        menu_state_.active_submenu_item_index = item.enabled ? static_cast<int>(item.index) : -1;
        if (!item.separator && item.enabled) {
          operations_.execute_menu_item(menu_state_.active_submenu_id, item.index);
        }
        operations_.request_chrome_redraw();
        return true;
      }
      operations_.request_chrome_redraw();
      return true;
    }

    if (const auto popup_rect =
            operations_.compute_popup_menu_rect(layout.menu_bar, menu_state_.active_menu_id);
        popup_rect.has_value() && Contains(*popup_rect, event.button.x, event.button.y)) {
      for (const auto& item :
           operations_.compute_visible_popup_menu_items(menu_state_.active_menu_id, *popup_rect)) {
        if (!Contains(item.rect, event.button.x, event.button.y)) {
          continue;
        }
        menu_state_.active_menu_item_index = item.enabled ? static_cast<int>(item.index) : -1;
        if (!item.separator && item.enabled) {
          operations_.execute_menu_item(menu_state_.active_menu_id, item.index);
        }
        operations_.request_chrome_redraw();
        return true;
      }
      operations_.request_chrome_redraw();
      return true;
    }

    operations_.close_menu_bar();
    operations_.request_chrome_redraw();
    return true;
  }

  if (!Contains(layout.menu_bar, event.button.x, event.button.y)) {
    return false;
  }

  for (const auto& item : menu_bar_items) {
    if (Contains(item.rect, event.button.x, event.button.y)) {
      operations_.open_menu_bar_menu(item.id);
      operations_.request_chrome_redraw();
      return true;
    }
  }
  // A press on the empty (draggable) part of the title bar never reaches here:
  // that region is reported as SDL_HITTEST_DRAGGABLE, and SDL consumes the
  // button event to drive a compositor-side window move (see SDL's
  // pointer_handle_button_common / ProcessHitTest). There is therefore no
  // app-visible click to time, so double-click-to-maximize cannot be detected
  // this way on Wayland (or X11) with our custom client-side title bar. Use the
  // window-control Maximize button instead. Just consume any stray click.
  operations_.request_chrome_redraw();
  return true;
}

bool ChromeMouseCoordinator::HandleMenuMotion(const SDL_Event& event,
                                              const WorkspaceLayout& layout) {
  util::PerformanceTrace::Scope perf_scope("ChromeMouseCoordinator::HandleMenuMotion");
  if (!menu_state_.menu_bar_open) {
    return false;
  }

  for (const auto& item : operations_.compute_visible_menu_bar_items(layout.menu_bar)) {
    if (!Contains(item.rect, event.motion.x, event.motion.y)) {
      continue;
    }
    if (item.id != menu_state_.active_menu_id) {
      operations_.open_menu_bar_menu(item.id);
      operations_.request_chrome_redraw();
    }
    return true;
  }

  if (const auto submenu_rect = operations_.active_submenu_rect(layout.menu_bar);
      submenu_rect.has_value() && Contains(*submenu_rect, event.motion.x, event.motion.y)) {
    const int previous_hovered_index = menu_state_.hovered_submenu_row_index;
    const auto hit = operations_.hit_test_popup_row(menu_state_.active_submenu_id, *submenu_rect,
                                                     event.motion.x, event.motion.y);
    int hovered_index = -1;
    bool hovered_enabled = false;
    SDL_FRect hovered_rect{};
    if (hit.has_value()) {
      hovered_index = static_cast<int>(hit->index);
      hovered_rect = hit->rect;
      if (!hit->separator) {
        hovered_enabled =
            operations_.is_menu_item_enabled_at(menu_state_.active_submenu_id, hit->index);
      }
    }
    menu_state_.active_submenu_item_index = hovered_enabled ? hovered_index : -1;
    menu_state_.hovered_submenu_row_index = hovered_index;
    if (hovered_index != previous_hovered_index) {
      if (previous_hovered_index >= 0) {
        RequestMenuHoverRowRedraw(
            operations_.request_redraw_rect,
            operations_.popup_row_rect_by_index(menu_state_.active_submenu_id, *submenu_rect,
                                                static_cast<std::size_t>(previous_hovered_index)));
      }
      if (hovered_index >= 0) {
        RequestMenuHoverRowRedraw(operations_.request_redraw_rect, hovered_rect);
      }
    }
    return true;
  }

  if (const auto popup_rect =
          operations_.compute_popup_menu_rect(layout.menu_bar, menu_state_.active_menu_id);
      popup_rect.has_value() && Contains(*popup_rect, event.motion.x, event.motion.y)) {
    const int previous_hovered_index = menu_state_.hovered_popup_row_index;
    const MenuId previous_submenu_id = menu_state_.active_submenu_id;
    const auto previous_submenu_rect = operations_.active_submenu_rect(layout.menu_bar);
    const auto hit = operations_.hit_test_popup_row(menu_state_.active_menu_id, *popup_rect,
                                                     event.motion.x, event.motion.y);
    int hovered_index = -1;
    bool hovered_enabled = false;
    SDL_FRect hovered_rect{};
    if (hit.has_value()) {
      hovered_index = static_cast<int>(hit->index);
      hovered_rect = hit->rect;
      if (!hit->separator) {
        hovered_enabled =
            operations_.is_menu_item_enabled_at(menu_state_.active_menu_id, hit->index);
      }
    }
    const MenuSpec* menu = operations_.find_menu_spec(menu_state_.active_menu_id);
    if (hovered_enabled && menu != nullptr) {
      const auto items = operations_.menu_items(menu_state_.active_menu_id);
      const MenuItemSpec& spec = items[hovered_index];
      if (spec.submenu != MenuId::None) {
        menu_state_.active_submenu_id = spec.submenu;
        menu_state_.active_submenu_item_index = -1;
        menu_state_.hovered_submenu_row_index = -1;
        menu_state_.active_submenu_anchor_rect = hovered_rect;
      } else {
        menu_state_.active_submenu_id = MenuId::None;
        menu_state_.active_submenu_item_index = -1;
        menu_state_.hovered_submenu_row_index = -1;
        menu_state_.active_submenu_anchor_rect.reset();
      }
    } else {
      menu_state_.active_submenu_id = MenuId::None;
      menu_state_.active_submenu_item_index = -1;
      menu_state_.hovered_submenu_row_index = -1;
      menu_state_.active_submenu_anchor_rect.reset();
    }
    menu_state_.active_menu_item_index = hovered_enabled ? hovered_index : -1;
    menu_state_.hovered_popup_row_index = hovered_index;
    if (hovered_index != previous_hovered_index) {
      if (previous_hovered_index >= 0) {
        RequestMenuHoverRowRedraw(
            operations_.request_redraw_rect,
            operations_.popup_row_rect_by_index(menu_state_.active_menu_id, *popup_rect,
                                                static_cast<std::size_t>(previous_hovered_index)));
      }
      if (hovered_index >= 0) {
        RequestMenuHoverRowRedraw(operations_.request_redraw_rect, hovered_rect);
      }
    }
    const auto current_submenu_rect = operations_.active_submenu_rect(layout.menu_bar);
    if (previous_submenu_id != menu_state_.active_submenu_id ||
        !OptionalRectsEqual(previous_submenu_rect, current_submenu_rect)) {
      if (previous_submenu_rect.has_value()) {
        operations_.request_redraw_rect(MakeRect(previous_submenu_rect->x - 1.0f,
                                                 previous_submenu_rect->y - 1.0f,
                                                 previous_submenu_rect->w + 2.0f,
                                                 previous_submenu_rect->h + 2.0f));
      }
      if (current_submenu_rect.has_value()) {
        operations_.request_redraw_rect(MakeRect(current_submenu_rect->x - 1.0f,
                                                 current_submenu_rect->y - 1.0f,
                                                 current_submenu_rect->w + 2.0f,
                                                 current_submenu_rect->h + 2.0f));
      }
    }
    return true;
  }

  if (menu_state_.hovered_popup_row_index >= 0) {
    if (const auto popup_rect =
            operations_.compute_popup_menu_rect(layout.menu_bar, menu_state_.active_menu_id);
        popup_rect.has_value()) {
      RequestMenuHoverRowRedraw(
          operations_.request_redraw_rect,
          operations_.popup_row_rect_by_index(
              menu_state_.active_menu_id, *popup_rect,
              static_cast<std::size_t>(menu_state_.hovered_popup_row_index)));
    }
    menu_state_.active_menu_item_index = -1;
    menu_state_.hovered_popup_row_index = -1;
  }
  return true;
}

bool ChromeMouseCoordinator::HandleOverlayButtonDown(const SDL_Event& event,
                                                     const WorkspaceLayout& layout) {
  if (!state_.overlay.visible || event.button.button != SDL_BUTTON_LEFT) {
    return false;
  }

  // The find / replace widget is non-modal: it has no match list, and a click
  // outside its rect must NOT dismiss it — it falls through so the editor takes
  // the click (and focus) while the widget keeps floating. Field clicks were
  // already consumed earlier by HandleSingleLineInputMouseDown, so here we only
  // see button clicks, empty-widget clicks, and outside clicks.
  if (state_.overlay.mode == OverlayMode::BufferSearch ||
      state_.overlay.mode == OverlayMode::BufferReplace) {
    const bool replace_mode = state_.overlay.mode == OverlayMode::BufferReplace;
    const FindWidgetLayout fw = ComputeFindWidgetLayout(layout.editor_surface, replace_mode);
    if (!Contains(fw.widget, event.button.x, event.button.y)) {
      // Repaint the widget (it will render unfocused once the editor takes focus)
      // and let the press fall through to the editor mouse path.
      operations_.request_overlay_redraw();
      return false;
    }
    if (Contains(fw.close_button, event.button.x, event.button.y)) {
      operations_.dismiss_overlay(true);
      return true;
    }
    if (Contains(fw.prev_button, event.button.x, event.button.y)) {
      operations_.move_buffer_search_selection(-1);
    } else if (Contains(fw.next_button, event.button.x, event.button.y)) {
      operations_.move_buffer_search_selection(1);
    } else if (replace_mode &&
               Contains(fw.replace_button, event.button.x, event.button.y)) {
      operations_.replace_current_buffer_search_match();
    } else if (replace_mode &&
               Contains(fw.replace_all_button, event.button.x, event.button.y)) {
      operations_.replace_all_buffer_search_matches();
    }
    // Any in-widget click (button or chrome) focuses the widget.
    state_.surface.focus = FocusTarget::Overlay;
    operations_.request_overlay_redraw();
    return true;
  }

  const SDL_FRect overlay = operations_.compute_overlay_rect(layout.editor_area);
  if (!Contains(overlay, event.button.x, event.button.y)) {
    operations_.dismiss_overlay(false);
    operations_.request_overlay_redraw();
    return true;
  }

  operations_.clamp_overlay_scroll_row(overlay);
  const auto list_layout = operations_.compute_overlay_list_layout(overlay);
  if (list_layout.scrollbar.has_value() &&
      Contains(list_layout.scrollbar->track, event.button.x, event.button.y)) {
    interaction_state_.drag_target = DragTarget::OverlayScrollbar;
    interaction_state_.drag_scrollbar_offset =
        Contains(list_layout.scrollbar->thumb, event.button.x, event.button.y)
            ? static_cast<float>(event.button.y) - list_layout.scrollbar->thumb.y
            : list_layout.scrollbar->thumb.h * 0.5f;
    state_.overlay.scroll_row = std::clamp(
        static_cast<int>(std::lround(ScrollUnitsForPointer(
            *list_layout.scrollbar, static_cast<float>(event.button.y),
            interaction_state_.drag_scrollbar_offset))),
        0, list_layout.max_scroll);
    state_.surface.focus = FocusTarget::Overlay;
    operations_.request_overlay_redraw();
    return true;
  }

  if (const auto item_index =
          ScrollableListIndexAtY(list_layout, static_cast<float>(event.button.y));
      item_index.has_value() && *item_index >= 0 &&
      *item_index < static_cast<int>(operations_.overlay_item_count())) {
    operations_.set_overlay_selected_index(static_cast<std::size_t>(*item_index));
    operations_.reveal_overlay_selection(overlay);
    if (state_.overlay.mode == OverlayMode::CommitPicker) {
      operations_.activate_overlay_selection();
    }
  }
  state_.surface.focus = FocusTarget::Overlay;
  operations_.request_overlay_redraw();
  return true;
}

bool ChromeMouseCoordinator::HandleTreeContextMenuButtonDown(const SDL_Event& event) {
  if (!menu_state_.tree_context_menu.open) {
    return false;
  }

  if (const auto popup_rect = operations_.compute_tree_context_menu_rect();
      popup_rect.has_value() && Contains(*popup_rect, event.button.x, event.button.y)) {
    for (const auto& item : operations_.compute_visible_tree_context_menu_items(
             menu_state_.tree_context_menu.target, menu_state_.tree_context_menu.active_item_index,
             *popup_rect)) {
      if (!Contains(item.rect, event.button.x, event.button.y)) {
        continue;
      }
      menu_state_.tree_context_menu.active_item_index =
          item.enabled ? static_cast<int>(item.index) : -1;
      if (event.button.button == SDL_BUTTON_LEFT && !item.separator && item.enabled) {
        operations_.execute_tree_context_menu_item(item.index);
      }
      operations_.request_chrome_redraw();
      return true;
    }
    operations_.request_chrome_redraw();
    return true;
  }

  operations_.close_tree_context_menu();
  operations_.request_chrome_redraw();
  return false;
}

bool ChromeMouseCoordinator::HandleTreeContextMenuMotion(const SDL_Event& event) {
  if (!menu_state_.tree_context_menu.open) {
    return false;
  }

  if (const auto popup_rect = operations_.compute_tree_context_menu_rect();
      popup_rect.has_value() && Contains(*popup_rect, event.motion.x, event.motion.y)) {
    menu_state_.tree_context_menu.active_item_index = -1;
    for (const auto& item : operations_.compute_visible_tree_context_menu_items(
             menu_state_.tree_context_menu.target, menu_state_.tree_context_menu.active_item_index,
             *popup_rect)) {
      if (Contains(item.rect, event.motion.x, event.motion.y)) {
        menu_state_.tree_context_menu.active_item_index =
            item.enabled ? static_cast<int>(item.index) : -1;
        break;
      }
    }
    operations_.request_chrome_redraw();
    return true;
  }

  menu_state_.tree_context_menu.active_item_index = -1;
  operations_.request_chrome_redraw();
  return true;
}

ChromeMouseCoordinator WorkspaceShell::MakeChromeMouseCoordinator() {
  return ChromeMouseCoordinator(
      context_.current_project_state, context_.menu_state, context_.interaction_state,
      ChromeMouseCoordinator::Operations{
          .close_menu_bar = [this]() { MakeMenuCoordinator().CloseMenuBar(); },
          .open_anchored_menu =
              [this](MenuId id, const SDL_FRect& anchor_rect) {
                MakeMenuCoordinator().OpenAnchoredMenu(id, anchor_rect);
              },
          .sidebar_mode_control_rect =
              [this](const SDL_FRect& rect) { return SidebarModeControlRect(rect); },
          .request_chrome_redraw = [this]() { RequestChromeRedraw(); },
          .request_redraw_rect = [this](const SDL_FRect& rect) { RequestRedrawRect(rect); },
          .compute_visible_menu_bar_items =
              [this](const SDL_FRect& rect) { return ComputeVisibleMenuBarItems(rect); },
          .compute_overflow_menu_bar_items =
              [this](const SDL_FRect& rect) { return ComputeOverflowMenuBarItems(rect); },
          .menu_overflow_chevron_rect =
              [this](const SDL_FRect& rect) { return MenuOverflowChevronRect(rect); },
          .compute_visible_window_control_buttons =
              [this](const SDL_FRect& rect) { return ComputeVisibleWindowControlButtons(rect); },
          .set_pending_window_action =
              [this](WorkspaceShell::WindowAction action) { pending_window_action_ = action; },
          .request_quit = [this]() { RequestQuit(); },
          .open_menu_bar_menu = [this](MenuId id) { MakeMenuCoordinator().OpenMenuBarMenu(id); },
          .active_submenu_rect = [this](const SDL_FRect& rect) { return ActiveSubmenuRect(rect); },
          .compute_visible_popup_menu_items =
              [this](MenuId id, const SDL_FRect& rect) {
                return ComputeVisiblePopupMenuItems(id, rect);
              },
          .hit_test_popup_row =
              [this](MenuId id, const SDL_FRect& popup_rect, float x, float y) {
                return WorkspaceShell::HitTestPopupRow(MenuItems(id), popup_rect, x, y);
              },
          .popup_row_rect_by_index =
              [this](MenuId id, const SDL_FRect& popup_rect, std::size_t index) {
                return WorkspaceShell::PopupRowRectByIndex(MenuItems(id), popup_rect, index);
              },
          .is_menu_item_enabled_at =
              [this](MenuId id, std::size_t index) {
                const auto items = MenuItems(id);
                return index < items.size() && IsMenuItemEnabled(items[index]);
              },
          .execute_menu_item =
              [this](MenuId id, std::size_t item_index) {
                return MakeMenuCoordinator().ExecuteMenuItem(id, item_index);
              },
          .compute_popup_menu_rect =
              [this](const SDL_FRect& menu_bar, MenuId id) {
                return ComputePopupMenuRect(menu_bar, id);
              },
          .find_menu_spec = [](MenuId id) { return WorkspaceShell::FindMenuSpec(id); },
          .menu_items = [this](MenuId id) { return MenuItems(id); },
          .open_submenu =
              [this](MenuId id, const SDL_FRect& rect) {
                MakeMenuCoordinator().OpenSubmenu(id, rect);
              },
          .close_submenu = [this]() { MakeMenuCoordinator().CloseSubmenu(); },
          .move_compare_picker_selection = [this](int delta) { MoveComparePickerSelection(delta); },
          .move_buffer_search_selection = [this](int delta) { MoveBufferSearchSelection(delta); },
          .replace_current_buffer_search_match = [this]() { ReplaceCurrentBufferSearchMatch(); },
          .replace_all_buffer_search_matches = [this]() { ReplaceAllBufferSearchMatches(); },
          .move_project_search_selection = [this](int delta) { MoveProjectSearchSelection(delta); },
          .move_file_finder_selection = [this](int delta) { MoveFileFinderSelection(delta); },
          .request_overlay_redraw = [this]() { RequestOverlayRedraw(); },
          .dismiss_overlay = [this](bool focus_editor) { DismissOverlay(focus_editor); },
          .compute_overlay_rect = [this](const SDL_FRect& rect) { return ComputeOverlayRect(rect); },
          .clamp_overlay_scroll_row = [this](const SDL_FRect& rect) { ClampOverlayScrollRow(rect); },
          .compute_overlay_list_layout =
              [this](const SDL_FRect& rect) { return ComputeOverlayListLayout(rect); },
          .overlay_item_count = [this]() { return OverlayItemCount(); },
          .set_overlay_selected_index = [this](std::size_t index) { SetOverlaySelectedIndex(index); },
          .reveal_overlay_selection = [this](const SDL_FRect& rect) { RevealOverlaySelection(rect); },
          .activate_overlay_selection = [this]() { ActivateOverlaySelection(); },
          .compute_tree_context_menu_rect = [this]() { return ComputeTreeContextMenuRect(); },
          .compute_visible_tree_context_menu_items =
              [this](TreeContextTargetKind target, int active_item_index, const SDL_FRect& rect) {
                return ComputeVisiblePopupMenuItems(TreeContextMenuItems(target), active_item_index, rect);
              },
          .execute_tree_context_menu_item =
              [this](std::size_t index) {
                return MakeMenuCoordinator().ExecuteTreeContextMenuItem(index);
              },
          .close_tree_context_menu = [this]() { MakeMenuCoordinator().CloseTreeContextMenu(); },
      });
}

}  // namespace microide::workspace
