#include "workspace/WorkspaceShellRenderPrimitives.h"

namespace microide::workspace {

using namespace detail;

void WorkspaceShell::RenderMenuPopups(SDL_Renderer* renderer,
                                      const WorkspaceLayout& layout) const {
  if (menu_state_.menu_bar_open) {
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

          DrawFilledRect(renderer, *popup_rect, theme_.overlay_background);
          DrawRect(renderer, *popup_rect, theme_.border);
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
            const SDL_Color background =
                item.hovered && item.enabled ? theme_.row_highlight : theme_.overlay_background;
            const SDL_Color text_color =
                !item.enabled ? theme_.text_disabled
                              : item.hovered ? theme_.text_primary : theme_.text_secondary;
            const SDL_Color accel_color =
                !item.enabled ? theme_.text_disabled : theme_.text_muted;
            DrawFilledRect(renderer, item.rect, background);
            if (item.checked) {
              DrawCenteredTextOn(text_renderer_, renderer,
                                 MakeRect(item.rect.x + 8.0f, item.rect.y, 10.0f, item.rect.h),
                                 item.enabled ? theme_.accent : theme_.text_disabled, background,
                                 "x");
            }
            const std::string accelerator = MenuItemAccelerator(spec);
            const float accelerator_width = text_renderer_.MeasureWidth(accelerator);
            const float label_width = std::max(0.0f, item.rect.w - 42.0f - accelerator_width);
            DrawVCenteredTextOn(text_renderer_, renderer,
                                MakeRect(item.rect.x + 24.0f, item.rect.y, label_width, item.rect.h),
                                0.0f, text_color, background,
                                TruncateLabel(MenuItemLabel(spec), label_width));
            if (!accelerator.empty()) {
              DrawVCenteredTextOn(
                  text_renderer_, renderer,
                  MakeRect(item.rect.x + item.rect.w - accelerator_width - 10.0f, item.rect.y,
                           accelerator_width, item.rect.h),
                  0.0f, accel_color, background, accelerator);
            }
          }
        };
    draw_popup_menu(menu_state_.active_menu_id, menu_state_.active_menu_item_index, std::nullopt);
    if (menu_state_.active_submenu_id != MenuId::None) {
      draw_popup_menu(menu_state_.active_submenu_id, menu_state_.active_submenu_item_index,
                      menu_state_.active_submenu_anchor_rect);
    }
  }

  if (!menu_state_.tree_context_menu.open) {
    return;
  }

  const auto items = TreeContextMenuItems(menu_state_.tree_context_menu.target);
  const auto popup_rect = ComputeTreeContextMenuRect();
  if (items.empty() || !popup_rect.has_value()) {
    return;
  }

  DrawFilledRect(renderer, *popup_rect, theme_.overlay_background);
  DrawRect(renderer, *popup_rect, theme_.border);
  for (const VisiblePopupMenuItem& item : ComputeVisiblePopupMenuItems(
           items, menu_state_.tree_context_menu.active_item_index, *popup_rect)) {
    if (item.separator) {
      DrawFilledRect(renderer,
                     MakeRect(item.rect.x + 8.0f, item.rect.y + item.rect.h * 0.5f,
                              std::max(0.0f, item.rect.w - 16.0f), 1.0f),
                     theme_.border);
      continue;
    }

    const MenuItemSpec& spec = items[item.index];
    const SDL_Color background =
        item.hovered && item.enabled ? theme_.row_highlight : theme_.overlay_background;
    const SDL_Color text_color =
        !item.enabled ? theme_.text_disabled
                      : item.hovered ? theme_.text_primary : theme_.text_secondary;
    const SDL_Color accel_color = !item.enabled ? theme_.text_disabled : theme_.text_muted;
    DrawFilledRect(renderer, item.rect, background);
    if (item.checked) {
      DrawCenteredTextOn(text_renderer_, renderer,
                         MakeRect(item.rect.x + 8.0f, item.rect.y, 10.0f, item.rect.h),
                         item.enabled ? theme_.accent : theme_.text_disabled, background, "x");
    }
    const std::string accelerator = MenuItemAccelerator(spec);
    const float accelerator_width = text_renderer_.MeasureWidth(accelerator);
    const float label_width = std::max(0.0f, item.rect.w - 42.0f - accelerator_width);
    DrawVCenteredTextOn(text_renderer_, renderer,
                        MakeRect(item.rect.x + 24.0f, item.rect.y, label_width, item.rect.h), 0.0f,
                        text_color, background, TruncateLabel(MenuItemLabel(spec), label_width));
    if (!accelerator.empty()) {
      DrawVCenteredTextOn(
          text_renderer_, renderer,
          MakeRect(item.rect.x + item.rect.w - accelerator_width - 10.0f, item.rect.y,
                   accelerator_width, item.rect.h),
          0.0f, accel_color, background, accelerator);
    }
  }
}

}  // namespace microide::workspace
