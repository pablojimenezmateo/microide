#include "workspace/WorkspaceShellRenderPrimitives.h"

namespace microide::workspace {

using namespace detail;

void WorkspaceShell::RenderMenuPopups(SDL_Renderer* renderer,
                                      const WorkspaceLayout& layout) const {
  if (context_.menu_state.menu_bar_open) {
    const auto draw_popup_menu =
        [&](MenuId menu_id, int active_item_index, const std::optional<SDL_FRect>& anchor_rect) {
          const MenuSpec* menu = FindMenuSpec(menu_id);
          if (menu == nullptr) {
            return;
          }
          const auto items = MenuItems(menu_id);
          const auto popup_rect =
              anchor_rect.has_value() ? ActiveSubmenuRect(layout.menu_bar)
                                      : ComputePopupMenuRect(layout.menu_bar, menu_id);
          if (!popup_rect.has_value()) {
            return;
          }

          DrawCardFrame(renderer, theme_, *popup_rect, CardStyle::Overlay);
          for (const VisiblePopupMenuItem& item :
               ComputeVisiblePopupMenuItems(items, active_item_index, *popup_rect)) {
            if (item.separator) {
              DrawFilledRect(renderer,
                             MakeRect(item.rect.x + 8.0f, item.rect.y + item.rect.h * 0.5f,
                                      std::max(0.0f, item.rect.w - 16.0f), 1.0f),
                             theme_.border);
              continue;
            }

            const MenuItemSpec& spec = items[item.index];
            const bool hovered = item.hovered ||
                                 (last_mouse_position_valid_ &&
                                  Contains(item.rect, last_mouse_x_, last_mouse_y_));
            DrawMenuRow(text_renderer_, renderer, theme_, item.rect, MenuItemLabel(spec),
                        MenuItemAccelerator(spec), item.enabled, hovered, item.checked);
          }
        };
    draw_popup_menu(context_.menu_state.active_menu_id, context_.menu_state.active_menu_item_index, std::nullopt);
    if (context_.menu_state.active_submenu_id != MenuId::None) {
      draw_popup_menu(context_.menu_state.active_submenu_id, context_.menu_state.active_submenu_item_index,
                      context_.menu_state.active_submenu_anchor_rect);
    }
  }

  if (context_.menu_state.overflow_popup_open &&
      context_.menu_state.overflow_popup_anchor_rect.has_value()) {
    const auto overflow = ComputeOverflowMenuBarItems(layout.menu_bar);
    if (!overflow.empty()) {
      const SDL_FRect popup = ComputeMenuOverflowPopupRect(
          *context_.menu_state.overflow_popup_anchor_rect, overflow.size());
      DrawCardFrame(renderer, theme_, popup, CardStyle::Overlay);
      for (std::size_t i = 0; i < overflow.size(); ++i) {
        const MenuSpec* spec = FindMenuSpec(overflow[i]);
        const SDL_FRect row =
            MakeRect(popup.x + 4.0f, popup.y + 4.0f + static_cast<float>(i) * kWorkspaceMenuPopupItemHeight,
                     popup.w - 8.0f, kWorkspaceMenuPopupItemHeight);
        const bool hovered = last_mouse_position_valid_ &&
                             Contains(row, last_mouse_x_, last_mouse_y_);
        DrawMenuRow(text_renderer_, renderer, theme_, row, spec ? std::string(spec->label) : std::string{},
                    {}, /*enabled=*/true, hovered, /*checked=*/false);
      }
    }
  }

  if (!context_.menu_state.tree_context_menu.open) {
    return;
  }

  const auto items = TreeContextMenuItems(context_.menu_state.tree_context_menu.target);
  const auto popup_rect = ComputeTreeContextMenuRect();
  if (items.empty() || !popup_rect.has_value()) {
    return;
  }

  DrawCardFrame(renderer, theme_, *popup_rect, CardStyle::Overlay);
  for (const VisiblePopupMenuItem& item : ComputeVisiblePopupMenuItems(
           items, context_.menu_state.tree_context_menu.active_item_index, *popup_rect)) {
    if (item.separator) {
      DrawFilledRect(renderer,
                     MakeRect(item.rect.x + 8.0f, item.rect.y + item.rect.h * 0.5f,
                              std::max(0.0f, item.rect.w - 16.0f), 1.0f),
                     theme_.border);
      continue;
    }

    const MenuItemSpec& spec = items[item.index];
    const bool hovered = item.hovered ||
                         (last_mouse_position_valid_ &&
                          Contains(item.rect, last_mouse_x_, last_mouse_y_));
    DrawMenuRow(text_renderer_, renderer, theme_, item.rect, MenuItemLabel(spec),
                MenuItemAccelerator(spec), item.enabled, hovered, item.checked);
  }
}

}  // namespace microide::workspace
