#include <memory>

#include "workspace/coordinators/WorkspaceChromeMouseCoordinator.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>

#include "util/PerformanceTrace.h"
#include "workspace/ListSelection.h"
#include "workspace/actions/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/coordinators/WorkspaceMenuCoordinator.h"

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

  // An open menu takes the press first, wherever it lands: its rows are drawn over
  // the sidebar, and the view buttons used to be tested first, so a row over them
  // switched the sidebar view instead of running (File > Remote: Open Folder on
  // Host… over the Search button, with the project strip shown). A press outside
  // the menu closes it and stops there, as everywhere else.
  if (HandleMenuButtonDown(event, layout)) {
    return true;
  }

  if (event.button.button == SDL_BUTTON_LEFT && state_.sidebar.visible) {
    const SidebarModeRowLayout mode_row = operations_.sidebar_mode_row(layout.sidebar);
    for (int i = 0; i < mode_row.tab_count; ++i) {
      const SidebarModeTab& tab = mode_row.tabs[static_cast<std::size_t>(i)];
      if (Contains(tab.rect, event.button.x, event.button.y)) {
        operations_.activate_sidebar_view(tab.id);
        state_.surface.focus = FocusTarget::Sidebar;
        operations_.request_chrome_redraw();
        return true;
      }
    }
    // A second press on the button while its menu is open is a press outside that
    // menu, which closed it above.
    if (mode_row.has_overflow && Contains(mode_row.overflow_rect, event.button.x, event.button.y)) {
      operations_.open_anchored_menu(MenuId::SidebarMode, mode_row.overflow_rect);
      state_.surface.focus = FocusTarget::Sidebar;
      operations_.request_chrome_redraw();
      return true;
    }
  }

  if (HandleOverlayButtonDown(event, layout)) {
    return true;
  }

  return false;
}

bool ChromeMouseCoordinator::HandleMotion(const SDL_Event& event, const WorkspaceLayout& layout) {
  // The chevron's hover fill follows the pointer; repaint it on the way in and
  // on the way out. Before anything that may consume the motion: a popup open
  // under the pointer still leaves the chevron drawn from the pointer. The chevron
  // is in the menu bar, so away from it there is nothing to light and, unless it
  // is lit, nothing to clear -- and its rect costs the bar's whole overflow layout,
  // which every pointer move over an open menu paid (TD-2026-10-25-391).
  if (menu_state_.chevron_hovered || Contains(layout.menu_bar, event.motion.x, event.motion.y)) {
    if (const auto chevron = operations_.menu_overflow_chevron_rect(layout.menu_bar);
        chevron.has_value()) {
      const bool hovered = Contains(*chevron, event.motion.x, event.motion.y);
      if (hovered != menu_state_.chevron_hovered) {
        menu_state_.chevron_hovered = hovered;
        operations_.request_redraw_rect(*chevron);
      }
    } else {
      menu_state_.chevron_hovered = false;
    }
  }
  if (HandleTreeContextMenuMotion(event)) {
    return true;
  }

  return HandleMenuMotion(event, layout);
}

// Closes the overflow popup and repaints what it covered. The generic chrome
// redraw is computed from the state AFTER the close, when the popup is no
// longer chrome, so a close that relied on it left the popup's pixels behind
// (found by the QA menu-clicks sweep in the compact layout).
void ChromeMouseCoordinator::CloseOverflowPopupWithRedraw(const WorkspaceLayout& layout) {
  if (menu_state_.overflow_popup_open && menu_state_.overflow_popup_anchor_rect.has_value()) {
    const auto overflow_specs = operations_.compute_overflow_menu_bar_items(layout.menu_bar);
    operations_.request_redraw_rect(
        ComputeMenuOverflowPopupRect(*menu_state_.overflow_popup_anchor_rect, overflow_specs.size()));
  }
  CloseMenuOverflowPopup(menu_state_);
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
    const FindWidgetLayout fw = ComputeBufferFindWidgetLayout(
        layout.editor_area, state_.overlay.mode == OverlayMode::BufferReplace);
    if (!Contains(fw.widget, event.wheel.mouse_x, event.wheel.mouse_y)) {
      return false;
    }
    operations_.move_buffer_search_selection(-overlay_ticks);
    operations_.request_overlay_redraw();
    return true;
  }
  // Every list overlay (file finder, project search, command palette, commit and
  // launch pickers, completion, code actions) scrolls *detached*: the wheel pans the
  // rows and leaves the selection where it is, so the highlighted row may scroll out
  // of view instead of the list dragging the selection along. This is what VS Code
  // does, and it means a wheel nudge can never change what Enter is about to run.
  // Every mode shares one `overlay.scroll_row` and one list geometry, so this is a
  // single clamp rather than the per-mode selection arithmetic it replaces.
  const SDL_FRect overlay = operations_.compute_overlay_rect(layout.editor_area);
  const auto list_layout = operations_.compute_overlay_list_layout(overlay);
  state_.overlay.scroll_row = std::clamp(
      state_.overlay.scroll_row - overlay_ticks * kWheelScrollRows, 0, list_layout.max_scroll);
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
      CloseOverflowPopupWithRedraw(layout);
    } else {
      operations_.close_menu_bar();
      OpenMenuOverflowPopup(menu_state_, *chevron_rect);
    }
    operations_.request_chrome_redraw();
    return true;
  }

  if (menu_state_.overflow_popup_open && menu_state_.overflow_popup_anchor_rect.has_value()) {
    const auto overflow_specs = operations_.compute_overflow_menu_bar_items(layout.menu_bar);
    const SDL_FRect popup = ComputeMenuOverflowPopupRect(*menu_state_.overflow_popup_anchor_rect,
                                                          overflow_specs.size());
    if (Contains(popup, event.button.x, event.button.y)) {
      if (const auto row = MenuOverflowPopupRowAt(popup, overflow_specs.size(), event.button.x,
                                                  event.button.y);
          row.has_value()) {
        const MenuId picked = overflow_specs[*row];
        const SDL_FRect row_rect = MenuOverflowPopupRowRect(popup, *row);
        CloseOverflowPopupWithRedraw(layout);
        operations_.open_anchored_menu(picked, row_rect);
        operations_.request_chrome_redraw();
      }
      return true;
    }
    // A press outside the popup closes it and stops there, like a menu's; one on
    // the menu bar goes on to open what it landed on.
    CloseOverflowPopupWithRedraw(layout);
    operations_.request_chrome_redraw();
    if (!Contains(layout.menu_bar, event.button.x, event.button.y)) {
      return true;
    }
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
      const auto items = operations_.menu_items(menu_state_.active_submenu_id);
      if (const auto row =
              operations_.hit_test_popup_row(items, *submenu_rect, event.button.x, event.button.y);
          row.has_value()) {
        const bool enabled = !row->separator && row->index < items.size() &&
                             operations_.is_menu_item_enabled(items[row->index]);
        menu_state_.active_submenu_item_index = enabled ? static_cast<int>(row->index) : -1;
        if (enabled) {
          operations_.execute_menu_item(menu_state_.active_submenu_id, row->index);
        }
      }
      operations_.request_chrome_redraw();
      return true;
    }

    if (const auto popup_rect =
            operations_.compute_popup_menu_rect(layout.menu_bar, menu_state_.active_menu_id);
        popup_rect.has_value() && Contains(*popup_rect, event.button.x, event.button.y)) {
      const auto items = operations_.menu_items(menu_state_.active_menu_id);
      if (const auto row =
              operations_.hit_test_popup_row(items, *popup_rect, event.button.x, event.button.y);
          row.has_value()) {
        const bool enabled = !row->separator && row->index < items.size() &&
                             operations_.is_menu_item_enabled(items[row->index]);
        menu_state_.active_menu_item_index = enabled ? static_cast<int>(row->index) : -1;
        if (enabled) {
          operations_.execute_menu_item(menu_state_.active_menu_id, row->index);
        }
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

bool ChromeMouseCoordinator::HandleMenuOverflowPopupMotion(const SDL_Event& event,
                                                          const WorkspaceLayout& layout) {
  if (!menu_state_.overflow_popup_open || !menu_state_.overflow_popup_anchor_rect.has_value()) {
    return false;
  }
  const auto overflow_specs = operations_.compute_overflow_menu_bar_items(layout.menu_bar);
  const SDL_FRect popup =
      ComputeMenuOverflowPopupRect(*menu_state_.overflow_popup_anchor_rect, overflow_specs.size());
  const auto row =
      MenuOverflowPopupRowAt(popup, overflow_specs.size(), event.motion.x, event.motion.y);
  const int hovered = row.has_value() ? static_cast<int>(*row) : -1;
  const int previous = menu_state_.overflow_popup_active_index;
  if (hovered == previous) {
    // Still inside the popup: consume the motion so the surface underneath does
    // not hover through an open popup.
    return row.has_value();
  }
  menu_state_.overflow_popup_active_index = hovered;
  if (previous >= 0) {
    RequestMenuHoverRowRedraw(operations_.request_redraw_rect,
                              MenuOverflowPopupRowRect(popup, static_cast<std::size_t>(previous)));
  }
  if (hovered >= 0) {
    RequestMenuHoverRowRedraw(operations_.request_redraw_rect,
                              MenuOverflowPopupRowRect(popup, static_cast<std::size_t>(hovered)));
  }
  return row.has_value();
}

// A popup row is painted highlighted when it is the ACTIVE item (the keyboard's,
// which a menu opened from the bar starts on) and tracked for hover separately, so
// a hover change repaints every row that was or is either one. Repainting only the
// previously HOVERED row left the first row highlighted beside the hovered one when
// the pointer entered a freshly opened menu (found by the QA redraw verifier).
void ChromeMouseCoordinator::RedrawChangedPopupRows(std::span<const MenuItemSpec> items,
                                                    const SDL_FRect& popup_rect,
                                                    PopupRowPair previous,
                                                    PopupRowPair current,
                                                    const SDL_FRect& hovered_rect) {
  if (previous.hovered == current.hovered && previous.active == current.active) {
    return;
  }
  for (const int index : {previous.hovered, previous.active}) {
    if (index >= 0 && index != current.hovered) {
      RequestMenuHoverRowRedraw(
          operations_.request_redraw_rect,
          operations_.popup_row_rect_by_index(items, popup_rect, static_cast<std::size_t>(index)));
    }
  }
  if (current.hovered >= 0) {
    RequestMenuHoverRowRedraw(operations_.request_redraw_rect, hovered_rect);
  }
}

bool ChromeMouseCoordinator::HandleMenuMotion(const SDL_Event& event,
                                              const WorkspaceLayout& layout) {
  util::PerformanceTrace::Scope perf_scope("ChromeMouseCoordinator::HandleMenuMotion");
  if (HandleMenuOverflowPopupMotion(event, layout)) {
    return true;
  }
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
    const int previous_active_index = menu_state_.active_submenu_item_index;
    const auto items = operations_.menu_items(menu_state_.active_submenu_id);
    const auto hit =
        operations_.hit_test_popup_row(items, *submenu_rect, event.motion.x, event.motion.y);
    int hovered_index = -1;
    bool hovered_enabled = false;
    SDL_FRect hovered_rect{};
    if (hit.has_value()) {
      hovered_index = static_cast<int>(hit->index);
      hovered_rect = hit->rect;
      if (!hit->separator && hit->index < items.size()) {
        hovered_enabled = operations_.is_menu_item_enabled(items[hit->index]);
      }
    }
    menu_state_.active_submenu_item_index = hovered_enabled ? hovered_index : -1;
    menu_state_.hovered_submenu_row_index = hovered_index;
    RedrawChangedPopupRows(items, *submenu_rect,
                           {previous_hovered_index, previous_active_index},
                           {hovered_index, menu_state_.active_submenu_item_index}, hovered_rect);
    return true;
  }

  if (const auto popup_rect =
          operations_.compute_popup_menu_rect(layout.menu_bar, menu_state_.active_menu_id);
      popup_rect.has_value() && Contains(*popup_rect, event.motion.x, event.motion.y)) {
    const int previous_hovered_index = menu_state_.hovered_popup_row_index;
    const int previous_active_index = menu_state_.active_menu_item_index;
    const MenuId previous_submenu_id = menu_state_.active_submenu_id;
    const auto previous_submenu_rect = operations_.active_submenu_rect(layout.menu_bar);
    const auto items = operations_.menu_items(menu_state_.active_menu_id);
    const auto hit =
        operations_.hit_test_popup_row(items, *popup_rect, event.motion.x, event.motion.y);
    int hovered_index = -1;
    bool hovered_enabled = false;
    SDL_FRect hovered_rect{};
    if (hit.has_value()) {
      hovered_index = static_cast<int>(hit->index);
      hovered_rect = hit->rect;
      if (!hit->separator && hit->index < items.size()) {
        hovered_enabled = operations_.is_menu_item_enabled(items[hit->index]);
      }
    }
    const MenuSpec* menu = operations_.find_menu_spec(menu_state_.active_menu_id);
    if (hovered_enabled && menu != nullptr) {
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
    RedrawChangedPopupRows(items, *popup_rect,
                           {previous_hovered_index, previous_active_index},
                           {hovered_index, menu_state_.active_menu_item_index}, hovered_rect);
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
    // Leaving the popup clears the hovered row AND the active one, which a key
    // may have moved elsewhere since the pointer arrived (Up under a resting
    // pointer): repaint both, or the key's row keeps its highlight on screen (QA
    // monkey seed 15).
    if (const auto popup_rect =
            operations_.compute_popup_menu_rect(layout.menu_bar, menu_state_.active_menu_id);
        popup_rect.has_value()) {
      RedrawChangedPopupRows(operations_.menu_items(menu_state_.active_menu_id), *popup_rect,
                             {menu_state_.hovered_popup_row_index,
                              menu_state_.active_menu_item_index},
                             {-1, -1}, SDL_FRect{});
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
    const FindWidgetLayout fw = ComputeBufferFindWidgetLayout(layout.editor_surface, replace_mode);
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
    const auto hovered_toggle = [&]() -> std::optional<BufferFindToggle> {
      for (std::size_t index = 0; index < fw.toggle_count; ++index) {
        if (Contains(fw.toggle_buttons[index], event.button.x, event.button.y)) {
          return static_cast<BufferFindToggle>(index);
        }
      }
      return std::nullopt;
    }();
    if (hovered_toggle.has_value()) {
      operations_.toggle_buffer_search_option(*hovered_toggle);
    } else if (Contains(fw.prev_button, event.button.x, event.button.y)) {
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
    // The completion list floats at the caret, inline with typing: a press
    // elsewhere closes it and still lands where it was aimed (VS Code's suggest
    // widget). Dismissed like a modal picker, the click was eaten and focus went
    // to the sidebar -- the caret stayed put and the next key typed into the
    // file tree.
    if (state_.overlay.mode == OverlayMode::Completion) {
      operations_.dismiss_overlay(true);
      return false;
    }
    operations_.dismiss_overlay(false);
    operations_.request_overlay_redraw();
    return true;
  }

  operations_.clamp_overlay_scroll_row(overlay);
  const auto list_layout = operations_.compute_overlay_list_layout(overlay);
  if (list_layout.scrollbar.has_value() &&
      Contains(list_layout.scrollbar->track, event.button.x, event.button.y)) {
    interaction_state_.drag_target = DragTarget::OverlayScrollbar;
    interaction_state_.drag_scrollbar_offset = ScrollbarGrabOffset(
        *list_layout.scrollbar, static_cast<float>(event.button.y), /*vertical=*/true);
    state_.overlay.scroll_row = std::clamp(
        static_cast<int>(std::lround(ScrollUnitsForPointer(
            *list_layout.scrollbar, static_cast<float>(event.button.y),
            interaction_state_.drag_scrollbar_offset))),
        0, list_layout.max_scroll);
    state_.surface.focus = FocusTarget::Overlay;
    operations_.request_overlay_redraw();
    return true;
  }

  if (const auto item_index = ScrollableListItemAtY(
          list_layout, static_cast<float>(event.button.y), operations_.overlay_item_count());
      item_index.has_value()) {
    operations_.set_overlay_selected_index(*item_index);
    operations_.reveal_overlay_selection(overlay);
    // A click on a row runs it, in every list overlay (VS Code's quick open and
    // suggest widget both do): the file finder, the project-search overlay and
    // the completion list used to only SELECT the clicked row, so a person who
    // clicked a Go to File result got nothing until they pressed Enter.
    if (event.button.button == SDL_BUTTON_LEFT) {
      operations_.activate_overlay_selection();
      operations_.request_overlay_redraw();
      return true;
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
    if (const auto row = operations_.hit_test_tree_context_menu_row(
            menu_state_.tree_context_menu.target, *popup_rect, event.button.x, event.button.y);
        row.has_value()) {
      const bool enabled =
          !row->separator && operations_.is_tree_context_menu_item_enabled_at(
                                 menu_state_.tree_context_menu.target, row->index);
      menu_state_.tree_context_menu.active_item_index = enabled ? static_cast<int>(row->index) : -1;
      if (event.button.button == SDL_BUTTON_LEFT && enabled) {
        operations_.execute_tree_context_menu_item(row->index);
      }
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
    if (const auto row = operations_.hit_test_tree_context_menu_row(
            menu_state_.tree_context_menu.target, *popup_rect, event.motion.x, event.motion.y);
        row.has_value() && !row->separator &&
        operations_.is_tree_context_menu_item_enabled_at(menu_state_.tree_context_menu.target,
                                                         row->index)) {
      menu_state_.tree_context_menu.active_item_index = static_cast<int>(row->index);
    }
    operations_.request_chrome_redraw();
    return true;
  }

  menu_state_.tree_context_menu.active_item_index = -1;
  operations_.request_chrome_redraw();
  return true;
}

ChromeMouseCoordinator& WorkspaceShell::MakeChromeMouseCoordinator() {
  if (glue_->chrome_mouse_coordinator != nullptr) {
    return *glue_->chrome_mouse_coordinator;
  }
  glue_->chrome_mouse_coordinator = std::make_unique<ChromeMouseCoordinator>(
      context_.current_project_state, context_.menu_state, context_.interaction_state,
      ChromeMouseCoordinator::Operations{
          .close_menu_bar = [this]() { MakeMenuCoordinator().CloseMenuBar(); },
          .open_anchored_menu =
              [this](MenuId id, const SDL_FRect& anchor_rect) {
                MakeMenuCoordinator().OpenAnchoredMenu(id, anchor_rect);
              },
          .sidebar_mode_row = [this](const SDL_FRect& rect) { return SidebarModeRow(rect); },
          .activate_sidebar_view =
              [this](std::string_view id) {
                ActionCoordinator(MakeActionContext())
                    .Execute(ActionId::SidebarShow, {std::string(id)}, ActionSource::Menu);
              },
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
          .hit_test_popup_row =
              [](std::span<const MenuItemSpec> items, const SDL_FRect& popup_rect, float x,
                 float y) { return WorkspaceShell::HitTestPopupRow(items, popup_rect, x, y); },
          .popup_row_rect_by_index =
              [](std::span<const MenuItemSpec> items, const SDL_FRect& popup_rect,
                 std::size_t index) {
                return WorkspaceShell::PopupRowRectByIndex(items, popup_rect, index);
              },
          .is_menu_item_enabled =
              [this](const MenuItemSpec& item) { return IsMenuItemEnabled(item); },
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
          .move_buffer_search_selection = [this](int delta) { MoveBufferSearchSelection(delta); },
          .replace_current_buffer_search_match = [this]() { ReplaceCurrentBufferSearchMatch(); },
          .replace_all_buffer_search_matches = [this]() { ReplaceAllBufferSearchMatches(); },
          .toggle_buffer_search_option =
              [this](BufferFindToggle toggle) { ToggleBufferSearchOption(toggle); },
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
          .hit_test_tree_context_menu_row =
              [](TreeContextTargetKind target, const SDL_FRect& popup_rect, float x, float y) {
                return WorkspaceShell::HitTestPopupRow(TreeContextMenuItems(target), popup_rect, x,
                                                       y);
              },
          .is_tree_context_menu_item_enabled_at =
              [this](TreeContextTargetKind target, std::size_t index) {
                const auto items = TreeContextMenuItems(target);
                return index < items.size() && IsMenuItemEnabled(items[index]);
              },
          .execute_tree_context_menu_item =
              [this](std::size_t index) {
                return MakeMenuCoordinator().ExecuteTreeContextMenuItem(index);
              },
          .close_tree_context_menu = [this]() { MakeMenuCoordinator().CloseTreeContextMenu(); },
      });
  return *glue_->chrome_mouse_coordinator;
}

}  // namespace microide::workspace
