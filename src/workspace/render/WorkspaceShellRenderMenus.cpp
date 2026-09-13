#include "workspace/render/WorkspaceShellRenderPrimitives.h"

namespace microide::workspace {

using namespace detail;

void WorkspaceShell::RenderMenuPopups(SDL_Renderer* renderer,
                                      const WorkspaceLayout& layout) const {
  // One popup body for the menu-bar popups, their submenu and the tree context
  // menu -- the three used to carry byte-similar copies of this loop. Rows land in
  // a reused scratch vector: popups paint one at a time, so the buffer is free
  // between them, and the value-returning overload allocated one per popup per frame.
  const auto draw_popup_body = [&](std::span<const MenuItemSpec> items, int active_item_index,
                                   const SDL_FRect& popup_rect) {
    DrawCardFrame(renderer, theme_, popup_rect, CardStyle::Overlay);
    ComputeVisiblePopupMenuItemsInto(items, active_item_index, popup_rect,
                                     menu_popup_rows_scratch_);
    for (const VisiblePopupMenuItem& item : menu_popup_rows_scratch_) {
      if (item.separator) {
        DrawFilledRect(renderer,
                       MakeRect(item.rect.x + 8.0f, item.rect.y + item.rect.h * 0.5f,
                                std::max(0.0f, item.rect.w - 16.0f), 1.0f),
                       theme_.border);
        continue;
      }

      const MenuItemSpec& spec = items[item.index];
      const bool hovered = item.hovered || (last_mouse_position_valid_ &&
                                            Contains(item.rect, last_mouse_x_, last_mouse_y_));
      DrawMenuRow(text_renderer_, renderer, theme_, item.rect, MenuItemLabel(spec),
                  MenuItemAccelerator(spec), item.enabled, hovered, item.checked);
    }
  };

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
          draw_popup_body(items, active_item_index, *popup_rect);
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
        // DrawMenuRow takes std::string_view; pass directly without materializing a per-row string.
        DrawMenuRow(text_renderer_, renderer, theme_, row,
                    spec ? std::string_view(spec->label) : std::string_view{},
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

  draw_popup_body(items, context_.menu_state.tree_context_menu.active_item_index, *popup_rect);
}

}  // namespace microide::workspace
