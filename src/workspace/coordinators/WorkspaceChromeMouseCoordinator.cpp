#include <memory>

#include "workspace/coordinators/WorkspaceChromeMouseCoordinator.h"
#include "util/Log.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>

#include "util/PerformanceTrace.h"
#include "workspace/ListSelection.h"
#include "workspace/MenuPopupGeometry.h"
#include "workspace/actions/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceLayout.h"
#include "workspace/coordinators/WorkspaceMenuCoordinator.h"

namespace microide::workspace {
namespace {

bool OptionalRectsEqual(const std::optional<SDL_FRect>& lhs,
                        const std::optional<SDL_FRect>& rhs) {
  if (!lhs.has_value() || !rhs.has_value()) {
    return lhs.has_value() == rhs.has_value();
  }
  return RectsEqual(*lhs, *rhs);
}

}

ChromeMouseCoordinator::ChromeMouseCoordinator(ProjectWorkspaceState& state,
                                               MenuSurfaceState& menu_state,
                                               InteractionState& interaction_state,
                                               Services services,
                                               Operations operations)
    : state_(state),
      menu_state_(menu_state),
      interaction_state_(interaction_state),
      services_(services),
      operations_(std::move(operations)) {}

bool ChromeMouseCoordinator::HandleButtonDown(const SDL_Event& event,
                                              const WorkspaceLayout& layout) {
  if (HandleTreeContextMenuButtonDown(event)) {
    return true;
  }

  if (menu_state_.menu_bar_open && event.button.button != SDL_BUTTON_LEFT) {
    services_.menus.CloseMenuBar();
  }

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
        operations_.request_frame();
        return true;
      }
    }
    if (mode_row.has_overflow && Contains(mode_row.overflow_rect, event.button.x, event.button.y)) {
      services_.menus.OpenAnchoredMenu(MenuId::SidebarMode, mode_row.overflow_rect);
      state_.surface.focus = FocusTarget::Sidebar;
      operations_.request_frame();
      return true;
    }
  }

  if (HandleOverlayButtonDown(event, layout)) {
    return true;
  }

  return false;
}

bool ChromeMouseCoordinator::HandleMotion(const SDL_Event& event, const WorkspaceLayout& layout) {
  if (menu_state_.chevron_hovered || Contains(layout.menu_bar, event.motion.x, event.motion.y)) {
    if (const auto chevron = operations_.menu_overflow_chevron_rect(layout.menu_bar);
        chevron.has_value()) {
      const bool hovered = Contains(*chevron, event.motion.x, event.motion.y);
      if (hovered != menu_state_.chevron_hovered) {
        menu_state_.chevron_hovered = hovered;
        operations_.request_frame();
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

void ChromeMouseCoordinator::CloseOverflowPopupWithRedraw() {
  if (menu_state_.overflow_popup_open) {
    operations_.request_frame();
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
  if (state_.overlay.mode == OverlayMode::BufferSearch ||
      state_.overlay.mode == OverlayMode::BufferReplace) {
    const FindWidgetLayout fw = ComputeBufferFindWidgetLayout(
        FocusedGroupSurface(layout, state_.editor_split, state_.clamped_focused_group_index()),
        state_.overlay.mode == OverlayMode::BufferReplace);
    if (!Contains(fw.widget, event.wheel.mouse_x, event.wheel.mouse_y)) {
      return false;
    }
    operations_.move_buffer_search_selection(-overlay_ticks);
    operations_.request_frame();
    return true;
  }
  const SDL_FRect overlay = operations_.compute_overlay_rect(layout.editor_area);
  const auto list_layout = operations_.compute_overlay_list_layout(overlay);
  state_.overlay.scroll_row = std::clamp(
      state_.overlay.scroll_row - overlay_ticks * kWheelScrollRows, 0, list_layout.max_scroll);
  operations_.request_frame();
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
    services_.menus.CloseMenuBar();
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
    operations_.request_frame();
    return true;
  }

  if (const auto chevron_rect = operations_.menu_overflow_chevron_rect(layout.menu_bar);
      chevron_rect.has_value() &&
      Contains(*chevron_rect, event.button.x, event.button.y)) {
    if (menu_state_.overflow_popup_open) {
      CloseOverflowPopupWithRedraw();
    } else {
      services_.menus.CloseMenuBar();
      OpenMenuOverflowPopup(menu_state_);
    }
    operations_.request_frame();
    return true;
  }

  if (const auto chevron = operations_.menu_overflow_chevron_rect(layout.menu_bar);
      menu_state_.overflow_popup_open && chevron.has_value()) {
    const auto overflow_specs = operations_.compute_overflow_menu_bar_items(layout.menu_bar);
    const SDL_FRect popup = ComputeMenuOverflowPopupRect(*chevron, overflow_specs.size());
    if (Contains(popup, event.button.x, event.button.y)) {
      if (const auto row = MenuOverflowPopupRowAt(popup, overflow_specs.size(), event.button.x,
                                                  event.button.y);
          row.has_value()) {
        const MenuId picked = overflow_specs[*row];
        const SDL_FRect row_rect = MenuOverflowPopupRowRect(popup, *row);
        CloseOverflowPopupWithRedraw();
        services_.menus.OpenAnchoredMenu(picked, row_rect);
        operations_.request_frame();
      }
      return true;
    }
    CloseOverflowPopupWithRedraw();
    operations_.request_frame();
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
        services_.menus.CloseMenuBar();
      } else {
        services_.menus.OpenMenuBarMenu(item.id);
      }
      operations_.request_frame();
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
          services_.menus.ExecuteMenuItem(menu_state_.active_submenu_id, row->index);
        }
      }
      operations_.request_frame();
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
          services_.menus.ExecuteMenuItem(menu_state_.active_menu_id, row->index);
        }
      }
      operations_.request_frame();
      return true;
    }

    services_.menus.CloseMenuBar();
    operations_.request_frame();
    return true;
  }

  if (!Contains(layout.menu_bar, event.button.x, event.button.y)) {
    return false;
  }

  for (const auto& item : menu_bar_items) {
    if (Contains(item.rect, event.button.x, event.button.y)) {
      services_.menus.OpenMenuBarMenu(item.id);
      operations_.request_frame();
      return true;
    }
  }
  return true;
}

bool ChromeMouseCoordinator::HandleMenuOverflowPopupMotion(const SDL_Event& event,
                                                          const WorkspaceLayout& layout) {
  const auto chevron = operations_.menu_overflow_chevron_rect(layout.menu_bar);
  if (!menu_state_.overflow_popup_open || !chevron.has_value()) {
    return false;
  }
  const auto overflow_specs = operations_.compute_overflow_menu_bar_items(layout.menu_bar);
  const SDL_FRect popup = ComputeMenuOverflowPopupRect(*chevron, overflow_specs.size());
  const auto row =
      MenuOverflowPopupRowAt(popup, overflow_specs.size(), event.motion.x, event.motion.y);
  const int hovered = row.has_value() ? static_cast<int>(*row) : -1;
  const int previous = menu_state_.overflow_popup_active_index;
  if (hovered == previous) {
    return row.has_value();
  }
  menu_state_.overflow_popup_active_index = hovered;
  operations_.request_frame();
  return row.has_value();
}

ChromeMouseCoordinator::PopupRowHover ChromeMouseCoordinator::HoveredPopupRow(
    std::span<const MenuItemSpec> items, const SDL_FRect& popup_rect, float x, float y) const {
  PopupRowHover hover;
  const auto hit = operations_.hit_test_popup_row(items, popup_rect, x, y);
  if (hit.has_value()) {
    hover.index = static_cast<int>(hit->index);
    hover.rect = hit->rect;
    hover.enabled = !hit->separator && hit->index < items.size() &&
                    operations_.is_menu_item_enabled(items[hit->index]);
  }
  return hover;
}

void ChromeMouseCoordinator::RequestFrameIfPopupRowsChanged(PopupRowPair previous, PopupRowPair current) {
  if (previous.hovered != current.hovered || previous.active != current.active) {
    operations_.request_frame();
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
      services_.menus.OpenMenuBarMenu(item.id);
      operations_.request_frame();
    }
    return true;
  }

  if (const auto submenu_rect = operations_.active_submenu_rect(layout.menu_bar);
      submenu_rect.has_value() && Contains(*submenu_rect, event.motion.x, event.motion.y)) {
    const int previous_hovered_index = menu_state_.hovered_submenu_row_index;
    const int previous_active_index = menu_state_.active_submenu_item_index;
    const auto items = operations_.menu_items(menu_state_.active_submenu_id);
    const auto [hovered_index, hovered_enabled, hovered_rect] =
        HoveredPopupRow(items, *submenu_rect, event.motion.x, event.motion.y);
    menu_state_.active_submenu_item_index = hovered_enabled ? hovered_index : -1;
    menu_state_.hovered_submenu_row_index = hovered_index;
    RequestFrameIfPopupRowsChanged({previous_hovered_index, previous_active_index},
                                   {hovered_index, menu_state_.active_submenu_item_index});
    return true;
  }

  if (const auto popup_rect =
          operations_.compute_popup_menu_rect(layout.menu_bar, menu_state_.active_menu_id);
      popup_rect.has_value() && Contains(*popup_rect, event.motion.x, event.motion.y)) {
    const int previous_hovered_index = menu_state_.hovered_popup_row_index;
    const int previous_active_index = menu_state_.active_menu_item_index;
    const MenuId previous_submenu_id = menu_state_.active_submenu_id;
    const std::optional<SDL_FRect> previous_submenu_anchor = menu_state_.active_submenu_anchor_rect;
    const auto items = operations_.menu_items(menu_state_.active_menu_id);
    const auto [hovered_index, hovered_enabled, hovered_rect] =
        HoveredPopupRow(items, *popup_rect, event.motion.x, event.motion.y);
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
    RequestFrameIfPopupRowsChanged({previous_hovered_index, previous_active_index},
                                   {hovered_index, menu_state_.active_menu_item_index});
    if (previous_submenu_id != menu_state_.active_submenu_id ||
        !OptionalRectsEqual(previous_submenu_anchor, menu_state_.active_submenu_anchor_rect)) {
      operations_.request_frame();
    }
    return true;
  }

  if (menu_state_.hovered_popup_row_index >= 0) {
    RequestFrameIfPopupRowsChanged({menu_state_.hovered_popup_row_index, menu_state_.active_menu_item_index},
                                   {-1, -1});
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

  if (state_.overlay.mode == OverlayMode::BufferSearch ||
      state_.overlay.mode == OverlayMode::BufferReplace) {
    const bool replace_mode = state_.overlay.mode == OverlayMode::BufferReplace;
    const FindWidgetLayout fw = ComputeBufferFindWidgetLayout(
        FocusedGroupSurface(layout, state_.editor_split, state_.clamped_focused_group_index()),
        replace_mode);
    if (!Contains(fw.widget, event.button.x, event.button.y)) {
      operations_.request_frame();
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
    state_.surface.focus = FocusTarget::Overlay;
    operations_.request_frame();
    return true;
  }

  const SDL_FRect overlay = operations_.compute_overlay_rect(layout.editor_area);
  if (!Contains(overlay, event.button.x, event.button.y)) {
    if (state_.overlay.mode == OverlayMode::Completion) {
      operations_.dismiss_overlay(true);
      return false;
    }
    operations_.dismiss_overlay(false);
    operations_.request_frame();
    return true;
  }

  operations_.clamp_overlay_scroll_row(overlay);
  const auto list_layout = operations_.compute_overlay_list_layout(overlay);
  if (list_layout.scrollbar.has_value() &&
      Contains(list_layout.scrollbar->track, event.button.x, event.button.y)) {
    interaction_state_.drag_target = DragTarget::OverlayScrollbar;
    interaction_state_.drag_scrollbar_offset = ScrollbarGrabOffset(
        *list_layout.scrollbar, static_cast<float>(event.button.y), true);
    state_.overlay.scroll_row = std::clamp(
        static_cast<int>(std::lround(ScrollUnitsForPointer(
            *list_layout.scrollbar, static_cast<float>(event.button.y),
            interaction_state_.drag_scrollbar_offset))),
        0, list_layout.max_scroll);
    state_.surface.focus = FocusTarget::Overlay;
    operations_.request_frame();
    return true;
  }

  if (const auto item_index = ScrollableListItemAtY(
          list_layout, static_cast<float>(event.button.y), operations_.overlay_item_count());
      item_index.has_value()) {
    operations_.set_overlay_selected_index(*item_index);
    operations_.reveal_overlay_selection(overlay);
    if (event.button.button == SDL_BUTTON_LEFT) {
      operations_.activate_overlay_selection();
      operations_.request_frame();
      return true;
    }
  }
  state_.surface.focus = FocusTarget::Overlay;
  operations_.request_frame();
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
        services_.menus.ExecuteTreeContextMenuItem(row->index);
      }
    }
    operations_.request_frame();
    return true;
  }

  services_.menus.CloseTreeContextMenu();
  operations_.request_frame();
  return false;
}

bool ChromeMouseCoordinator::HandleTreeContextMenuMotion(const SDL_Event& event) {
  if (!menu_state_.tree_context_menu.open) {
    return false;
  }

  int active = -1;
  if (const auto popup_rect = operations_.compute_tree_context_menu_rect();
      popup_rect.has_value() && Contains(*popup_rect, event.motion.x, event.motion.y)) {
    if (const auto row = operations_.hit_test_tree_context_menu_row(
            menu_state_.tree_context_menu.target, *popup_rect, event.motion.x, event.motion.y);
        row.has_value() && !row->separator &&
        operations_.is_tree_context_menu_item_enabled_at(menu_state_.tree_context_menu.target,
                                                         row->index)) {
      active = static_cast<int>(row->index);
    }
  }
  if (active != menu_state_.tree_context_menu.active_item_index) {
    menu_state_.tree_context_menu.active_item_index = active;
    operations_.request_frame();
  }
  return true;
}

ChromeMouseCoordinator& WorkspaceShell::MakeChromeMouseCoordinator() {
  if (glue_->chrome_mouse_coordinator != nullptr) {
    return *glue_->chrome_mouse_coordinator;
  }
  glue_->chrome_mouse_coordinator = std::make_unique<ChromeMouseCoordinator>(
      context_.current_project_state, context_.menu_state, context_.interaction_state,
      ChromeMouseCoordinator::Services{
          .menus = MakeMenuCoordinator(),
      },
      ChromeMouseCoordinator::Operations{
          .sidebar_mode_row = [this](const SDL_FRect& rect) { return SidebarModeRow(rect); },
          .activate_sidebar_view =
              [this](std::string_view id) {
                ActionCoordinator(MakeActionContext())
                    .Execute(ActionId::SidebarShow, {std::string(id)}, ActionSource::Menu);
              },
          .request_frame = [this]() { RequestPreparedFrame(); },
          .compute_visible_menu_bar_items = [this](const SDL_FRect& rect) { return ComputeVisibleMenuBarItems(rect); },
          .compute_overflow_menu_bar_items =
              [this](const SDL_FRect& rect) { return ComputeOverflowMenuBarItems(rect); },
          .menu_overflow_chevron_rect = [this](const SDL_FRect& rect) { return MenuOverflowChevronRect(rect); },
          .compute_visible_window_control_buttons =
              [this](const SDL_FRect& rect) { return ComputeVisibleWindowControlButtons(rect); },
          .set_pending_window_action = [this](WorkspaceShell::WindowAction action) { pending_window_action_ = action; },
          .request_quit =
              [this]() {
                util::Log("microide quit: the title bar's close button");
                RequestQuit();
              },
          .active_submenu_rect = [this](const SDL_FRect& rect) { return ActiveSubmenuRect(rect); },
          .hit_test_popup_row = [](std::span<const MenuItemSpec> items, const SDL_FRect& popup_rect, float x,
                                   float y) { return HitTestPopupRow(items, popup_rect, x, y); },
          .is_menu_item_enabled = [this](const MenuItemSpec& item) { return IsMenuItemEnabled(item); },
          .compute_popup_menu_rect = [this](const SDL_FRect& menu_bar,
                                            MenuId id) { return ComputePopupMenuRect(menu_bar, id); },
          .find_menu_spec = [](MenuId id) { return WorkspaceShell::FindMenuSpec(id); },
          .menu_items = [this](MenuId id) { return MenuItems(id); },
          .move_buffer_search_selection = [this](int delta) { MoveBufferSearchSelection(delta); },
          .replace_current_buffer_search_match = [this]() { ReplaceCurrentBufferSearchMatch(); },
          .replace_all_buffer_search_matches = [this]() { ReplaceAllBufferSearchMatches(); },
          .toggle_buffer_search_option = [this](BufferFindToggle toggle) { ToggleBufferSearchOption(toggle); },
          .dismiss_overlay = [this](bool focus_editor) { DismissOverlay(focus_editor); },
          .compute_overlay_rect = [this](const SDL_FRect& rect) { return ComputeOverlayRect(rect); },
          .clamp_overlay_scroll_row = [this](const SDL_FRect& rect) { ClampOverlayScrollRow(rect); },
          .compute_overlay_list_layout = [this](const SDL_FRect& rect) { return ComputeOverlayListLayout(rect); },
          .overlay_item_count = [this]() { return OverlayItemCount(); },
          .set_overlay_selected_index = [this](std::size_t index) { SetOverlaySelectedIndex(index); },
          .reveal_overlay_selection = [this](const SDL_FRect& rect) { RevealOverlaySelection(rect); },
          .activate_overlay_selection = [this]() { ActivateOverlaySelection(); },
          .compute_tree_context_menu_rect = [this]() { return ComputeTreeContextMenuRect(); },
          .hit_test_tree_context_menu_row =
              [](TreeContextTargetKind target, const SDL_FRect& popup_rect, float x, float y) {
                return HitTestPopupRow(WorkspaceTreeContextMenuItems(target), popup_rect, x, y);
              },
          .is_tree_context_menu_item_enabled_at =
              [this](TreeContextTargetKind target, std::size_t index) {
                const auto items = WorkspaceTreeContextMenuItems(target);
                return index < items.size() && IsMenuItemEnabled(items[index]);
              },
      });
  return *glue_->chrome_mouse_coordinator;
}

}
