#include "workspace/WorkspaceShell.h"

#include <algorithm>

#include "workspace/WorkspaceSidebarRegistry.h"

namespace microide::workspace {

std::span<const WorkspaceShell::MenuItemSpec> WorkspaceShell::TreeContextMenuItems(
    TreeContextTargetKind target) {
  return WorkspaceTreeContextMenuItems(target);
}

namespace {

bool IsMenuBarTopLevelMenu(MenuId id) {
  return id != MenuId::SidebarMode && id != MenuId::GitOutgoingBase &&
         id != MenuId::EditorContext && id != MenuId::EditorTabContext &&
         id != MenuId::TerminalContext && id != MenuId::TerminalTabContext &&
         id != MenuId::ProjectTabContext;
}

// A bool-typed setting value reads as "on" unless it is one of the falsey tokens.
bool SettingTruthy(const std::optional<std::string>& value) {
  return value.has_value() && !(*value == "false" || *value == "0" || *value == "off");
}

}  // namespace

bool WorkspaceShell::IsMenuBarMenuVisible(MenuId id) const {
  // Every top-level menu (including Debug) is always visible. The Debug menu's
  // first item is the master enable/disable toggle; the remaining debug items
  // grey out via IsMenuItemEnabled when `debug.enabled` is off.
  return IsMenuBarTopLevelMenu(id);
}

std::vector<WorkspaceShell::VisibleMenuBarItem> WorkspaceShell::ComputeVisibleMenuBarItems(
    const SDL_FRect& menu_bar) const {
  std::vector<VisibleMenuBarItem> items;
  if (layout_mode_service_.CurrentMode() == LayoutMode::Compact) {
    return items;  // hamburger only — every top-level menu reaches the popup
  }
  float x = menu_bar.x + 8.0f;
  const float y = menu_bar.y + 3.0f;
  const float height = std::max(18.0f, menu_bar.h - 6.0f);
  const auto window_buttons = ComputeVisibleWindowControlButtons(menu_bar);
  const float chevron_reserve = kWorkspaceMenuOverflowChevronWidth + 4.0f;
  const float available_right = window_buttons.empty()
                                    ? menu_bar.x + menu_bar.w - 8.0f
                                    : window_buttons.front().rect.x - 8.0f;

  // First pass: measure widths and determine whether all items fit.
  // MenuSpec::label is a `string_view` over static storage, so the measured
  // width is stable for the lifetime of the process (until the font reloads).
  // Cache by label-data pointer to avoid 8 width-cache map lookups per
  // ComputeVisibleMenuBarItems call — `menu_hover_switch` hits this path many
  // times per frame (round-4 Finding 6).
  static thread_local std::unordered_map<const char*, float> spec_label_width_cache;
  std::vector<std::pair<MenuId, float>> measured;
  measured.reserve(8);
  float total_x = x;
  bool any_overflow = false;
  for (const MenuSpec& spec : MenuSpecs()) {
    if (!IsMenuBarMenuVisible(spec.id)) {
      continue;
    }
    const char* key = spec.label.data();
    auto cached = spec_label_width_cache.find(key);
    float raw_width;
    if (cached != spec_label_width_cache.end()) {
      raw_width = cached->second;
    } else {
      raw_width = text_renderer_.MeasureWidth(spec.label);
      spec_label_width_cache.emplace(key, raw_width);
    }
    const float width = std::clamp(raw_width + 28.0f, 56.0f, 116.0f);
    measured.emplace_back(spec.id, width);
    total_x += width + 4.0f;
  }
  any_overflow = total_x > available_right;
  const float max_x = any_overflow ? available_right - chevron_reserve : available_right;

  for (const auto& [id, width] : measured) {
    if (x + width > max_x) {
      break;
    }
    items.push_back(VisibleMenuBarItem{
        .id = id,
        .rect = MakeRect(x, y, width, height),
        .active = context_.menu_state.menu_bar_open && id == context_.menu_state.active_menu_id,
    });
    x += width + 4.0f;
  }
  return items;
}

std::vector<MenuId> WorkspaceShell::ComputeOverflowMenuBarItems(
    const SDL_FRect& menu_bar) const {
  const auto visible = ComputeVisibleMenuBarItems(menu_bar);
  std::vector<MenuId> overflow;
  std::size_t consumed = 0;
  for (const MenuSpec& spec : MenuSpecs()) {
    if (!IsMenuBarMenuVisible(spec.id)) {
      continue;
    }
    if (consumed < visible.size() && visible[consumed].id == spec.id) {
      ++consumed;
      continue;
    }
    overflow.push_back(spec.id);
  }
  return overflow;
}

std::optional<SDL_FRect> WorkspaceShell::MenuOverflowChevronRect(
    const SDL_FRect& menu_bar) const {
  const auto overflow = ComputeOverflowMenuBarItems(menu_bar);
  if (overflow.empty()) {
    return std::nullopt;
  }
  const auto window_buttons = ComputeVisibleWindowControlButtons(menu_bar);
  const float available_right = window_buttons.empty()
                                    ? menu_bar.x + menu_bar.w - 8.0f
                                    : window_buttons.front().rect.x - 8.0f;
  const float y = menu_bar.y + 3.0f;
  const float height = std::max(18.0f, menu_bar.h - 6.0f);
  const float x = available_right - kWorkspaceMenuOverflowChevronWidth;
  return MakeRect(x, y, kWorkspaceMenuOverflowChevronWidth, height);
}

std::vector<WorkspaceShell::VisibleWindowControlButton>
WorkspaceShell::ComputeVisibleWindowControlButtons(const SDL_FRect& menu_bar) const {
  std::vector<VisibleWindowControlButton> buttons;
  if (!CurrentWindowChromeState().custom_enabled) {
    return buttons;
  }

  const float button_size = std::max(18.0f, menu_bar.h - 6.0f);
  const float total_width =
      button_size * 3.0f + kWorkspaceWindowControlButtonGap * 2.0f;
  const float start_x =
      menu_bar.x + std::max(0.0f, menu_bar.w - total_width -
                                      kWorkspaceWindowControlButtonRightInset);
  const float y = menu_bar.y + (menu_bar.h - button_size) * 0.5f;
  static constexpr auto kButtonIds = std::to_array<WindowControlButtonId>({
      WindowControlButtonId::Minimize,
      WindowControlButtonId::Maximize,
      WindowControlButtonId::Close,
  });

  float x = start_x;
  for (WindowControlButtonId id : kButtonIds) {
    const SDL_FRect rect = MakeRect(x, y, button_size, button_size);
    buttons.push_back(VisibleWindowControlButton{
        .id = id,
        .rect = rect,
        .hovered =
            last_mouse_position_valid_ &&
            Contains(WindowControlButtonHitRect(rect), last_mouse_x_, last_mouse_y_),
    });
    x += button_size + kWorkspaceWindowControlButtonGap;
  }
  return buttons;
}

std::optional<SDL_FRect> WorkspaceShell::ComputePopupMenuRect(
    const SDL_FRect& anchor_rect,
    std::span<const MenuItemSpec> items,
    const SDL_FRect& bounds) const {
  if (items.empty()) {
    return std::nullopt;
  }

  // Popup width is expensive (one width-cache query per label + accelerator on every
  // item) and is recomputed many times per frame: render, redraw planner, mouse
  // hit-test, cursor manager, and the submenu / overflow paths all call this. Cache
  // the per-span result keyed by the items pointer + size + LSP readiness state, since
  // only LSP-driven labels (GoToDefinition, FindReferences) depend on LSP state.
  struct PopupWidthCacheEntry {
    const MenuItemSpec* items_data = nullptr;
    std::size_t items_size = 0;
    std::uint8_t lsp_state = 0;
    int lsp_indexed_count = 0;
    float raw_width = 0.0f;
    float total_height = 0.0f;
  };
  static thread_local std::array<PopupWidthCacheEntry, 6> popup_width_cache;
  static thread_local std::size_t popup_width_cache_next = 0;

  const auto readiness =
      const_cast<WorkspaceShell*>(this)->ActiveLspReadinessSnapshot();
  const std::uint8_t lsp_state = static_cast<std::uint8_t>(readiness.state);

  float raw_width = 172.0f;
  float total_height = 12.0f;
  bool found_cached = false;
  for (const auto& entry : popup_width_cache) {
    if (entry.items_data == items.data() && entry.items_size == items.size() &&
        entry.lsp_state == lsp_state &&
        entry.lsp_indexed_count == readiness.indexed_count) {
      raw_width = entry.raw_width;
      total_height = entry.total_height;
      found_cached = true;
      break;
    }
  }
  if (!found_cached) {
    for (const MenuItemSpec& item : items) {
      if (item.separator) {
        total_height += kWorkspaceMenuPopupSeparatorHeight;
        continue;
      }
      raw_width = std::max(raw_width,
                           text_renderer_.MeasureWidth(MenuItemLabel(item)) +
                               text_renderer_.MeasureWidth(MenuItemAccelerator(item)) + 68.0f);
      total_height += kWorkspaceMenuPopupItemHeight;
    }
    popup_width_cache[popup_width_cache_next] = PopupWidthCacheEntry{
        .items_data = items.data(),
        .items_size = items.size(),
        .lsp_state = lsp_state,
        .lsp_indexed_count = readiness.indexed_count,
        .raw_width = raw_width,
        .total_height = total_height,
    };
    popup_width_cache_next = (popup_width_cache_next + 1) % popup_width_cache.size();
  }

  float width = raw_width;
  const float height = total_height;
  const float max_width = std::max(172.0f, bounds.w - 8.0f);
  width = std::clamp(width, 172.0f, max_width);
  float x = std::clamp(anchor_rect.x, bounds.x + 4.0f,
                       bounds.x + std::max(4.0f, bounds.w - width - 4.0f));
  float y = anchor_rect.y + std::max(0.0f, anchor_rect.h) - 1.0f;
  if (y + height > bounds.y + bounds.h - 4.0f) {
    y = anchor_rect.y - height + 1.0f;
  }
  y = std::clamp(y, bounds.y + 4.0f, bounds.y + std::max(4.0f, bounds.h - height - 4.0f));
  return MakeRect(x, y, width, height);
}

std::optional<SDL_FRect> WorkspaceShell::ComputePopupMenuRect(const SDL_FRect& menu_bar,
                                                              MenuId id) const {
  const MenuSpec* menu = FindMenuSpec(id);
  if (menu == nullptr) {
    return std::nullopt;
  }
  const auto items = MenuItems(id);

  const SDL_FRect bounds = CurrentWindowRect().value_or(
      MakeRect(0.0f, 0.0f, menu_bar.w, std::max(menu_bar.y + menu_bar.h + 320.0f, menu_bar.h)));
  if (context_.menu_state.active_menu_anchor_rect.has_value() && id == context_.menu_state.active_menu_id) {
    return ComputePopupMenuRect(*context_.menu_state.active_menu_anchor_rect, items, bounds);
  }

  const auto menu_bar_items = ComputeVisibleMenuBarItems(menu_bar);
  const auto bar_it =
      std::find_if(menu_bar_items.begin(), menu_bar_items.end(),
                   [id](const VisibleMenuBarItem& item) { return item.id == id; });
  if (bar_it == menu_bar_items.end()) {
    return std::nullopt;
  }
  return ComputePopupMenuRect(bar_it->rect, items, bounds);
}

std::optional<WorkspaceShell::PopupRowGeometry> WorkspaceShell::HitTestPopupRow(
    std::span<const MenuItemSpec> items, const SDL_FRect& popup_rect, float x, float y) {
  if (x < popup_rect.x || x >= popup_rect.x + popup_rect.w ||
      y < popup_rect.y || y >= popup_rect.y + popup_rect.h) {
    return std::nullopt;
  }
  const float row_x = popup_rect.x + 6.0f;
  const float row_w = std::max(0.0f, popup_rect.w - 12.0f);
  float row_y = popup_rect.y + 6.0f;
  for (std::size_t i = 0; i < items.size(); ++i) {
    const float height = items[i].separator ? kWorkspaceMenuPopupSeparatorHeight
                                             : kWorkspaceMenuPopupItemHeight;
    if (y < row_y + height) {
      if (y < row_y || x < row_x || x >= row_x + row_w) {
        return std::nullopt;
      }
      return PopupRowGeometry{
          .index = i, .rect = MakeRect(row_x, row_y, row_w, height),
          .separator = items[i].separator,
      };
    }
    row_y += height;
  }
  return std::nullopt;
}

std::optional<SDL_FRect> WorkspaceShell::PopupRowRectByIndex(
    std::span<const MenuItemSpec> items, const SDL_FRect& popup_rect, std::size_t index) {
  if (index >= items.size()) {
    return std::nullopt;
  }
  const float row_x = popup_rect.x + 6.0f;
  const float row_w = std::max(0.0f, popup_rect.w - 12.0f);
  float row_y = popup_rect.y + 6.0f;
  for (std::size_t i = 0; i < index; ++i) {
    row_y += items[i].separator ? kWorkspaceMenuPopupSeparatorHeight
                                : kWorkspaceMenuPopupItemHeight;
  }
  const float height = items[index].separator ? kWorkspaceMenuPopupSeparatorHeight
                                              : kWorkspaceMenuPopupItemHeight;
  return MakeRect(row_x, row_y, row_w, height);
}

std::vector<WorkspaceShell::VisiblePopupMenuItem> WorkspaceShell::ComputeVisiblePopupMenuItems(
    std::span<const MenuItemSpec> items,
    int active_item_index,
    const SDL_FRect& popup_rect) const {
  std::vector<VisiblePopupMenuItem> visible_items;
  float y = popup_rect.y + 6.0f;
  for (std::size_t i = 0; i < items.size(); ++i) {
    const MenuItemSpec& item = items[i];
    const float height = item.separator ? kWorkspaceMenuPopupSeparatorHeight
                                        : kWorkspaceMenuPopupItemHeight;
    const SDL_FRect rect =
        MakeRect(popup_rect.x + 6.0f, y, std::max(0.0f, popup_rect.w - 12.0f), height);
    visible_items.push_back(VisiblePopupMenuItem{
        .index = i,
        .rect = rect,
        .enabled = IsMenuItemEnabled(item),
        .checked = IsMenuItemChecked(item),
        .hovered = static_cast<int>(i) == active_item_index,
        .separator = item.separator,
    });
    y += height;
  }
  return visible_items;
}

std::vector<WorkspaceShell::VisiblePopupMenuItem> WorkspaceShell::ComputeVisiblePopupMenuItems(
    MenuId id,
    const SDL_FRect& popup_rect) const {
  const auto items = MenuItems(id);
  return items.empty() ? std::vector<VisiblePopupMenuItem>{}
                       : ComputeVisiblePopupMenuItems(items, context_.menu_state.active_menu_item_index,
                                                      popup_rect);
}

std::string WorkspaceShell::MenuItemLabel(const MenuItemSpec& item) const {
  const ActionSpec* command_action =
      item.command_name.empty() ? nullptr : FindActionByCommand(item.command_name);
  const ActionId effective_action =
      command_action != nullptr ? command_action->id : item.action;

  if (!item.label.empty()) {
    if (IsLspDrivenMenuAction(effective_action)) {
      return LspDrivenMenuActionLabel(
          effective_action, item.label,
          const_cast<WorkspaceShell*>(this)->ActiveLspReadinessSnapshot());
    }
    return std::string(item.label);
  }
  if (!item.command_name.empty()) {
    if (const ActionSpec* action = FindActionByCommand(item.command_name);
        action != nullptr && !action->label.empty()) {
      return IsLspDrivenMenuAction(effective_action)
                 ? LspDrivenMenuActionLabel(
                       effective_action, action->label,
                       const_cast<WorkspaceShell*>(this)->ActiveLspReadinessSnapshot())
                 : std::string(action->label);
    }
    if (const ActionSpec* action = FindActionByCommand(item.command_name);
        action != nullptr && !action->command_name.empty()) {
      return std::string(action->command_name);
    }
    return std::string(item.command_name);
  }
  if (const ActionSpec* action = FindActionSpec(item.action);
      action != nullptr && !action->label.empty()) {
    return IsLspDrivenMenuAction(effective_action)
               ? LspDrivenMenuActionLabel(effective_action, action->label,
                                          const_cast<WorkspaceShell*>(this)->ActiveLspReadinessSnapshot())
               : std::string(action->label);
  }
  if (const ActionSpec* action = FindActionSpec(item.action);
      action != nullptr && !action->command_name.empty()) {
    return std::string(action->command_name);
  }
  return {};
}

std::string WorkspaceShell::MenuItemAccelerator(const MenuItemSpec& item) const {
  if (item.submenu != MenuId::None) {
    return ">";
  }
  if (!item.accelerator.empty()) {
    return std::string(item.accelerator);
  }
  if (!item.command_name.empty()) {
    if (const ActionSpec* action = FindActionByCommand(item.command_name);
        action != nullptr && !action->accelerator.empty()) {
      return std::string(action->accelerator);
    }
  }
  if (const ActionSpec* action = FindActionSpec(item.action);
      action != nullptr && !action->accelerator.empty()) {
    return std::string(action->accelerator);
  }
  return {};
}

bool WorkspaceShell::IsMenuItemEnabled(const MenuItemSpec& item) const {
  if (item.separator) {
    return false;
  }

  const ActionSpec* command_action =
      item.command_name.empty() ? nullptr : FindActionByCommand(item.command_name);
  const ActionId effective_action =
      command_action != nullptr ? command_action->id : item.action;

  if (!item.command_name.empty() && command_action == nullptr) {
    const auto& plugin_commands = plugin_runtime_.Host().CommandNames();
    return std::find(plugin_commands.begin(), plugin_commands.end(),
                     std::string(item.command_name)) != plugin_commands.end();
  }

  if (IsLspDrivenMenuAction(effective_action) &&
      !IsLspMenuActionReady(const_cast<WorkspaceShell*>(this)->ActiveLspReadinessSnapshot())) {
    return false;
  }

  if (effective_action == ActionId::Files) {
    return !context_.current_project_state.root.empty();
  }
  if (effective_action == ActionId::Focus && item.arg_count > 0) {
    if (item.args[0] == "sidebar") {
      return context_.current_project_state.sidebar.visible;
    }
    if (item.args[0] == "panel") {
      return context_.current_project_state.panel.command_mode || ActiveTerminalTab() != nullptr;
    }
    return true;
  }
  if ((effective_action == ActionId::SidebarShow || effective_action == ActionId::SidebarToggle) &&
      item.arg_count > 0) {
    if (FindBuiltinSidebarView(item.args[0]) != nullptr) {
      return !context_.current_project_state.root.empty();
    }
    return FindSidebarView(item.args[0], plugin_runtime_.Host()).has_value();
  }

  return MakeActionAvailability().IsEnabled(effective_action);
}

bool WorkspaceShell::IsMenuItemChecked(const MenuItemSpec& item) const {
  if (!item.checkable) {
    return false;
  }

  if (item.action == ActionId::SidebarToggle) {
    return context_.current_project_state.sidebar.visible;
  }
  if (item.action == ActionId::DebugPaneToggle) {
    return context_.current_project_state.debug_pane.visible;
  }
  if (item.action == ActionId::DebugToggleEnabled) {
    return SettingTruthy(GetSettingValue("debug.enabled"));
  }
  if (item.action == ActionId::Wrap) {
    return context_.current_project_state.editor_preferences.soft_wrap;
  }
  if (item.action == ActionId::ToggleStatusBar) {
    return layout_mode_service_.StatusBarVisible();
  }
  if (item.action == ActionId::ToggleLayoutMode) {
    return layout_mode_service_.CurrentMode() == LayoutMode::Compact;
  }
  if (item.action == ActionId::SidebarShow && item.arg_count > 0) {
    const std::optional<SidebarViewInfo> view = FindSidebarView(item.args[0], plugin_runtime_.Host());
    if (view.has_value()) {
      if (view->mode == SidebarMode::Search && context_.current_project_state.sidebar.temporary) {
        return false;
      }
      return context_.current_project_state.sidebar.visible && context_.current_project_state.sidebar.view_id == view->id;
    }
  }
  if (const char* setting_key = EditorEssentialsCapabilitySettingKey(item.action);
      setting_key != nullptr) {
    const auto value = GetSettingValue(setting_key);
    if (!value.has_value()) {
      return false;
    }
    return !(*value == "false" || *value == "0" || *value == "off");
  }
  return false;
}

std::optional<SDL_FRect> WorkspaceShell::ActiveSubmenuRect(const SDL_FRect& menu_bar) const {
  if (context_.menu_state.active_submenu_id == MenuId::None ||
      !context_.menu_state.active_submenu_anchor_rect.has_value()) {
    return std::nullopt;
  }
  const MenuSpec* submenu = FindMenuSpec(context_.menu_state.active_submenu_id);
  if (submenu == nullptr) {
    return std::nullopt;
  }
  const auto submenu_items = MenuItems(context_.menu_state.active_submenu_id);
  const SDL_FRect bounds = CurrentWindowRect().value_or(
      MakeRect(0.0f, 0.0f, menu_bar.w, std::max(menu_bar.y + menu_bar.h + 320.0f, menu_bar.h)));
  SDL_FRect anchor = *context_.menu_state.active_submenu_anchor_rect;
  anchor.x += anchor.w - 1.0f;
  return ComputePopupMenuRect(anchor, submenu_items, bounds);
}

std::filesystem::path WorkspaceShell::SelectedTreePath() const {
  const project::TreeEntry* entry = SelectedTreeEntry();
  return entry == nullptr ? std::filesystem::path{} : entry->path.lexically_normal();
}

std::filesystem::path WorkspaceShell::ResolveTreeActionPath(ActionSource source) const {
  if (source == ActionSource::ContextMenu && context_.menu_state.tree_context_menu.open &&
      !context_.menu_state.tree_context_menu.path.empty()) {
    return context_.menu_state.tree_context_menu.path.lexically_normal();
  }
  return SelectedTreePath();
}

std::optional<SDL_FRect> WorkspaceShell::ComputeTreeContextMenuRect() const {
  const auto window_rect = CurrentWindowRect();
  if (!context_.menu_state.tree_context_menu.open || !window_rect.has_value()) {
    return std::nullopt;
  }
  return ComputePopupMenuRect(context_.menu_state.tree_context_menu.anchor_rect,
                              TreeContextMenuItems(context_.menu_state.tree_context_menu.target),
                              *window_rect);
}

}  // namespace microide::workspace
