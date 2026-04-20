#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "workspace/WorkspaceActionTypes.h"
#include "workspace/WorkspaceMenuState.h"

namespace microide::workspace {

class MenuCoordinator {
 public:
  struct Operations {
    std::function<void()> request_chrome_redraw;
    std::function<std::span<const MenuItemSpec>(MenuId)> menu_items;
    std::function<bool(const MenuItemSpec&)> is_menu_item_enabled;
    std::function<std::optional<SDL_FRect>(MenuId)> menu_popup_rect;
    std::function<std::optional<SDL_FRect>(MenuId, std::size_t)> menu_popup_item_rect;
    std::function<bool(ActionId, const std::vector<std::string>&, ActionSource)> execute_action;
  };

  MenuCoordinator(MenuSurfaceState& menu_state, Operations operations);

  int FirstEnabledMenuItemIndex(MenuId id) const;
  int NextEnabledMenuItemIndex(MenuId id, int current_index, int delta) const;
  void OpenMenuBarMenu(MenuId id);
  void OpenAnchoredMenu(MenuId id, const SDL_FRect& anchor_rect);
  void OpenSubmenu(MenuId id, const SDL_FRect& anchor_rect);
  void CloseSubmenu();
  void CloseMenuBar();
  bool ExecuteMenuItem(MenuId menu_id, std::size_t item_index);
  bool SwitchMenuBarMenu(int delta);
  bool MoveActiveMenuItem(int delta);
  void OpenTreeContextMenu(TreeContextTargetKind target,
                           const std::filesystem::path& path,
                           const SDL_FRect& anchor_rect);
  void CloseTreeContextMenu();
  bool ExecuteTreeContextMenuItem(std::size_t item_index);
  int FirstEnabledTreeContextMenuItemIndex() const;
  int NextEnabledTreeContextMenuItemIndex(int current_index, int delta) const;

 private:
  MenuSurfaceState& menu_state_;
  Operations operations_;
};

}  // namespace microide::workspace
