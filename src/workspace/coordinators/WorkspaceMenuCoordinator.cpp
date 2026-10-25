#include <memory>

#include "workspace/coordinators/WorkspaceMenuCoordinator.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "util/PerformanceTrace.h"
#include "workspace/actions/WorkspaceActionCoordinator.h"
#include "workspace/registries/WorkspaceMenuRegistry.h"
#include "workspace/shell/WorkspaceShell.h"

namespace microide::workspace {

MenuCoordinator::MenuCoordinator(MenuSurfaceState& menu_state, Operations operations)
    : menu_state_(menu_state), operations_(std::move(operations)) {}

namespace {

// The first item `enabled` accepts, or -1.
template <typename Items, typename Enabled>
int FirstEnabledIndex(const Items& items, const Enabled& enabled) {
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (enabled(items[i])) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// The next item `delta` steps from `current_index` that `enabled` accepts,
// wrapping; with no current item, Down starts at the top and Up at the bottom.
// `current_index` when nothing else is enabled, -1 for an empty list.
template <typename Items, typename Enabled>
int NextEnabledIndex(const Items& items, int current_index, int delta, const Enabled& enabled) {
  if (items.empty() || delta == 0) {
    return -1;
  }
  const int item_count = static_cast<int>(items.size());
  int index = current_index < 0 ? (delta > 0 ? -1 : 0) : current_index;
  for (int step = 0; step < item_count; ++step) {
    index = (index + delta + item_count) % item_count;
    if (enabled(items[static_cast<std::size_t>(index)])) {
      return index;
    }
  }
  return current_index;
}

}  // namespace

int MenuCoordinator::FirstEnabledMenuItemIndex(MenuId id) const {
  return FirstEnabledIndex(operations_.menu_items(id), operations_.is_menu_item_enabled);
}

int MenuCoordinator::NextEnabledMenuItemIndex(MenuId id, int current_index, int delta) const {
  return NextEnabledIndex(operations_.menu_items(id), current_index, delta,
                          operations_.is_menu_item_enabled);
}

void MenuCoordinator::OpenMenuBarMenu(MenuId id) { OpenMenu(id, std::nullopt); }

void MenuCoordinator::OpenAnchoredMenu(MenuId id, const SDL_FRect& anchor_rect) {
  OpenMenu(id, anchor_rect);
}

void MenuCoordinator::OpenMenu(MenuId id, std::optional<SDL_FRect> anchor_rect) {
  operations_.request_chrome_redraw();
  if (id == MenuId::None) {
    CloseMenuBar();
    return;
  }
  CloseTreeContextMenu();
  menu_state_.menu_bar_open = true;
  menu_state_.active_menu_id = id;
  menu_state_.active_menu_item_index = FirstEnabledMenuItemIndex(id);
  menu_state_.hovered_popup_row_index = -1;
  menu_state_.active_menu_anchor_rect = anchor_rect;
  CloseSubmenu();
  operations_.request_chrome_redraw();
}

std::size_t MenuCoordinator::MenuOverflowItemCount() const {
  return operations_.overflow_menu_bar_items().size();
}

bool MenuCoordinator::OpenMenuOverflowItem(std::size_t index) {
  const MenuBarOverflowIds overflow = operations_.overflow_menu_bar_items();
  if (index >= overflow.size() || !menu_state_.overflow_popup_anchor_rect.has_value()) {
    return false;
  }
  const SDL_FRect row = MenuOverflowPopupRowRect(
      ComputeMenuOverflowPopupRect(*menu_state_.overflow_popup_anchor_rect, overflow.size()),
      index);
  const MenuId picked = overflow[index];
  CloseMenuOverflowPopup();
  OpenAnchoredMenu(picked, row);
  return true;
}

void MenuCoordinator::CloseMenuOverflowPopup() {
  if (menu_state_.overflow_popup_open && menu_state_.overflow_popup_anchor_rect.has_value() &&
      operations_.request_redraw_rect) {
    operations_.request_redraw_rect(ComputeMenuOverflowPopupRect(
        *menu_state_.overflow_popup_anchor_rect, operations_.overflow_menu_bar_items().size()));
  }
  workspace::CloseMenuOverflowPopup(menu_state_);
}

void MenuCoordinator::OpenSubmenu(MenuId id, const SDL_FRect& anchor_rect) {
  operations_.request_chrome_redraw();
  menu_state_.active_submenu_id = id;
  menu_state_.active_submenu_item_index = FirstEnabledMenuItemIndex(id);
  menu_state_.hovered_submenu_row_index = -1;
  menu_state_.active_submenu_anchor_rect = anchor_rect;
  operations_.request_chrome_redraw();
}

void MenuCoordinator::CloseSubmenu() {
  operations_.request_chrome_redraw();
  menu_state_.active_submenu_id = MenuId::None;
  menu_state_.active_submenu_item_index = -1;
  menu_state_.hovered_submenu_row_index = -1;
  menu_state_.active_submenu_anchor_rect.reset();
  operations_.request_chrome_redraw();
}

void MenuCoordinator::CloseMenuBar() {
  operations_.request_chrome_redraw();
  menu_state_.menu_bar_open = false;
  menu_state_.active_menu_id = MenuId::None;
  menu_state_.active_menu_item_index = -1;
  menu_state_.hovered_popup_row_index = -1;
  menu_state_.active_menu_anchor_rect.reset();
  CloseSubmenu();
  operations_.request_chrome_redraw();
}

bool MenuCoordinator::ExecuteMenuItem(MenuId menu_id, std::size_t item_index) {
  const auto items = operations_.menu_items(menu_id);
  if (items.empty() || item_index >= items.size()) {
    return false;
  }

  const MenuItemSpec& item = items[item_index];
  if (!operations_.is_menu_item_enabled(item)) {
    return true;
  }
  if (item.submenu != MenuId::None) {
    if (const auto item_rect = operations_.menu_popup_item_rect(menu_id, item_index);
        item_rect.has_value()) {
      OpenSubmenu(item.submenu, *item_rect);
      return true;
    }
    CloseSubmenu();
    return true;
  }

  menu_state_.last_run_menu_id = menu_id;
  menu_state_.last_run_item_index = item_index;
  ++menu_state_.rows_run;
  // tools/qa clicks every row of every menu to check each click reaches its row;
  // running them would quit, close the project and open native pickers.
  static const bool qa_dry_run = util::PerformanceTrace::FlagEnabled("MICROIDE_QA_MENU_DRY_RUN");
  if (qa_dry_run) {
    CloseMenuBar();
    return true;
  }

  if (operations_.execute_custom_menu_item != nullptr &&
      operations_.execute_custom_menu_item(menu_id, item_index)) {
    CloseMenuBar();
    return true;
  }

  std::vector<std::string> args;
  args.reserve(item.arg_count);
  for (std::size_t i = 0; i < item.arg_count; ++i) {
    args.emplace_back(item.args[i]);
  }
  CloseMenuBar();
  if (!item.command_name.empty()) {
    return operations_.execute_command_name(item.command_name, args, ActionSource::Menu);
  }
  return operations_.execute_action(item.action, args, ActionSource::Menu);
}

// Left/Right walk the menu BAR, as in VS Code. It used to walk every menu spec,
// so Right from Help opened a context menu at the bar, and Right in the sidebar's
// views menu (anchored on its button) switched to the git base menu with the
// button still painted pressed (found by the QA monkey). A menu that opened
// anchored on a button or a click has no neighbours: the arrows do nothing there.
bool MenuCoordinator::SwitchMenuBarMenu(int delta) {
  if (menu_state_.active_menu_id == MenuId::None || delta == 0 ||
      menu_state_.active_menu_anchor_rect.has_value() ||
      !IsMenuBarTopLevelMenu(menu_state_.active_menu_id)) {
    return false;
  }
  util::InlineVector<MenuId, kMaxMenuBarItems> bar;
  for (const MenuSpec& spec : WorkspaceMenuSpecs()) {
    if (IsMenuBarTopLevelMenu(spec.id)) {
      bar.push_back(spec.id);
    }
  }
  const auto current = std::find(bar.begin(), bar.end(), menu_state_.active_menu_id);
  if (current == bar.end()) {
    return false;
  }
  const int count = static_cast<int>(bar.size());
  const int next_index =
      (static_cast<int>(std::distance(bar.begin(), current)) + delta % count + count) % count;
  OpenMenuBarMenu(bar[static_cast<std::size_t>(next_index)]);
  return true;
}

bool MenuCoordinator::MoveActiveMenuItem(int delta) {
  if (!menu_state_.menu_bar_open || menu_state_.active_menu_id == MenuId::None) {
    return false;
  }
  menu_state_.active_menu_item_index =
      NextEnabledMenuItemIndex(menu_state_.active_menu_id, menu_state_.active_menu_item_index, delta);
  return menu_state_.active_menu_item_index >= 0;
}

void MenuCoordinator::OpenTreeContextMenu(TreeContextTargetKind target,
                                          const std::filesystem::path& path,
                                          const SDL_FRect& anchor_rect,
                                          std::size_t line) {
  operations_.request_chrome_redraw();
  CloseMenuBar();
  menu_state_.tree_context_menu.open = true;
  menu_state_.tree_context_menu.target = target;
  menu_state_.tree_context_menu.path = path.lexically_normal();
  menu_state_.tree_context_menu.line = line;
  menu_state_.tree_context_menu.anchor_rect = anchor_rect;
  menu_state_.tree_context_menu.active_item_index = FirstEnabledTreeContextMenuItemIndex();
  operations_.request_chrome_redraw();
}

void MenuCoordinator::CloseTreeContextMenu() {
  operations_.request_chrome_redraw();
  menu_state_.tree_context_menu = TreeContextMenuState{};
  operations_.request_chrome_redraw();
}

bool MenuCoordinator::ExecuteTreeContextMenuItem(std::size_t item_index) {
  const auto items = WorkspaceTreeContextMenuItems(menu_state_.tree_context_menu.target);
  if (item_index >= items.size()) {
    return false;
  }

  const MenuItemSpec& item = items[item_index];
  if (!operations_.is_menu_item_enabled(item)) {
    return true;
  }

  std::vector<std::string> args;
  args.reserve(item.arg_count);
  for (std::size_t i = 0; i < item.arg_count; ++i) {
    args.emplace_back(item.args[i]);
  }
  const bool handled = operations_.execute_action(item.action, args, ActionSource::ContextMenu);
  CloseTreeContextMenu();
  return handled;
}

int MenuCoordinator::FirstEnabledTreeContextMenuItemIndex() const {
  return FirstEnabledIndex(WorkspaceTreeContextMenuItems(menu_state_.tree_context_menu.target),
                           operations_.is_menu_item_enabled);
}

int MenuCoordinator::NextEnabledTreeContextMenuItemIndex(int current_index, int delta) const {
  return NextEnabledIndex(WorkspaceTreeContextMenuItems(menu_state_.tree_context_menu.target),
                          current_index, delta, operations_.is_menu_item_enabled);
}

MenuCoordinator& WorkspaceShell::MakeMenuCoordinator() {
  if (glue_->menu_coordinator != nullptr) {
    return *glue_->menu_coordinator;
  }
  glue_->menu_coordinator = std::make_unique<MenuCoordinator>(
      context_.menu_state,
      MenuCoordinator::Operations{
          .request_chrome_redraw = [this]() { RequestChromeRedraw(); },
          .request_redraw_rect = [this](const SDL_FRect& rect) { RequestRedrawRect(rect); },
          .menu_items = [this](MenuId id) { return MenuItems(id); },
          .is_menu_item_enabled =
              [this](const MenuItemSpec& item) { return IsMenuItemEnabled(item); },
          .overflow_menu_bar_items =
              [this]() -> MenuBarOverflowIds {
                const auto layout = CurrentWorkspaceLayout();
                return layout.has_value() ? ComputeOverflowMenuBarItems(layout->menu_bar)
                                          : MenuBarOverflowIds{};
              },
          .menu_popup_item_rect =
              [this](MenuId id, std::size_t item_index) -> std::optional<SDL_FRect> {
                const auto layout = CurrentWorkspaceLayout();
                if (!layout.has_value()) {
                  return std::nullopt;
                }
                const auto popup_rect = ComputePopupMenuRect(layout->menu_bar, id);
                if (!popup_rect.has_value()) {
                  return std::nullopt;
                }
                // Geometry only: the value-returning form builds a row vector
                // and probes enablement for every row to answer about one.
                return WorkspaceShell::PopupRowRectByIndex(MenuItems(id), *popup_rect, item_index);
              },
          .execute_custom_menu_item =
              [this](MenuId id, std::size_t item_index) {
                return ExecuteCustomMenuItem(id, item_index);
              },
          .execute_action =
              [this](ActionId id, const std::vector<std::string>& args, ActionSource source) {
                return ActionCoordinator(MakeActionContext()).Execute(id, args, source);
              },
          .execute_command_name =
              [this](std::string_view command_name,
                     const std::vector<std::string>& args,
                     ActionSource source) {
                return ExecuteCommandName(command_name, args, source);
              },
      });
  return *glue_->menu_coordinator;
}

}  // namespace microide::workspace
