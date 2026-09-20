#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "workspace/actions/WorkspaceActionTypes.h"
#include "workspace/registries/WorkspaceMenuRegistry.h"
#include "workspace/state/WorkspaceMenuState.h"

namespace microide::workspace {

class MenuCoordinator {
 public:
  struct Operations {
    std::function<void()> request_chrome_redraw;
    std::function<std::span<const MenuItemSpec>(MenuId)> menu_items;
    std::function<bool(const MenuItemSpec&)> is_menu_item_enabled;
    // The menu-bar items that did not fit, in order. Depends on the live
    // menu-bar rect, which this coordinator does not see.
    std::function<MenuBarOverflowIds()> overflow_menu_bar_items;
    std::function<std::optional<SDL_FRect>(MenuId, std::size_t)> menu_popup_item_rect;
    std::function<bool(MenuId, std::size_t)> execute_custom_menu_item;
    std::function<bool(ActionId, const std::vector<std::string>&, ActionSource)> execute_action;
    std::function<bool(std::string_view, const std::vector<std::string>&, ActionSource)>
        execute_command_name;
  };

  MenuCoordinator(MenuSurfaceState& menu_state, Operations operations);

  int FirstEnabledMenuItemIndex(MenuId id) const;
  int NextEnabledMenuItemIndex(MenuId id, int current_index, int delta) const;
  void OpenMenuBarMenu(MenuId id);
  void OpenAnchoredMenu(MenuId id, const SDL_FRect& anchor_rect);
  // The menu-bar overflow popup: the menus that did not fit in compact layout.
  // `OpenMenuOverflowItem` anchors the picked menu under its own row and closes
  // the popup — the same landing the click path produces.
  std::size_t MenuOverflowItemCount() const;
  bool OpenMenuOverflowItem(std::size_t index);
  void OpenSubmenu(MenuId id, const SDL_FRect& anchor_rect);
  void CloseSubmenu();
  void CloseMenuBar();
  bool ExecuteMenuItem(MenuId menu_id, std::size_t item_index);
  bool SwitchMenuBarMenu(int delta);
  bool MoveActiveMenuItem(int delta);
  void OpenTreeContextMenu(TreeContextTargetKind target,
                           const std::filesystem::path& path,
                           const SDL_FRect& anchor_rect,
                           std::size_t line = 0);
  void CloseTreeContextMenu();
  bool ExecuteTreeContextMenuItem(std::size_t item_index);
  int FirstEnabledTreeContextMenuItemIndex() const;
  int NextEnabledTreeContextMenuItemIndex(int current_index, int delta) const;

 private:
  MenuSurfaceState& menu_state_;
  Operations operations_;
};

}  // namespace microide::workspace
