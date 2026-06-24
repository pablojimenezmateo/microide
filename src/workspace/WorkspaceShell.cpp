#include "workspace/WorkspaceShell.h"

#include <array>
#include <string_view>

#include "editor/GutterIconRegistry.h"

#include "workspace/WorkspaceActionCoordinator.h"
#include "workspace/WorkspaceCommandRegistry.h"
#include "workspace/WorkspaceShellBootstrapper.h"
#include "workspace/WorkspaceSidebarRegistry.h"
#include "workspace/WorkspaceSettingsRegistry.h"

namespace microide::workspace {

namespace {

std::size_t MenuSlotIndex(WorkspaceShell::MenuId id) {
  return static_cast<std::size_t>(id);
}

}  // namespace

std::span<const WorkspaceShell::ActionSpec> WorkspaceShell::ActionSpecs() {
  return WorkspaceCommandSpecs();
}

void WorkspaceShell::InvalidateEditorBlamePath(const std::filesystem::path& path) {
  if (context_.current_project_state.root.empty() || path.empty()) {
    return;
  }
  git_blame_service_.InvalidatePath(context_.current_project_state.root, path.lexically_normal());
}

void WorkspaceShell::ClearEditorBlame() {
  editor_blame_overlay_service_.ClearVisibleOverlay();
  active_editor_hover_target_.reset();
  git_blame_service_.Clear();
}

void WorkspaceShell::SetStartupOptions(WorkspaceStartupOptions options) {
  startup_options_ = std::move(options);
  if (startup_options_.plugins_disabled()) {
    plugin_runtime_.Host().SetStartupPluginsEnabled(false);
  }
}

WorkspaceShell::~WorkspaceShell() {
  // Drain project background work before member teardown to avoid races on
  // git sidebar refresh state during shell destruction.
  project_background_executor_.Shutdown();
  // Shut down the plugin runtime while every shell member it still calls back
  // into (e.g. the pending redraw invalidation) is alive. Reverse-order member
  // destruction would otherwise tear down `pending_render_invalidation_`
  // before `plugin_runtime_`, and plugin teardown invokes
  // RequestEditorSurfaceRedraw via the shell callbacks.
  plugin_runtime_.Shutdown();
}

WorkspaceShell::SidebarMode WorkspaceShell::SidebarModeForViewId(std::string_view view_id) const {
  if (view_id.empty()) {
    return SidebarMode::None;
  }
  const auto view = FindSidebarView(view_id, plugin_runtime_.Host());
  return view.has_value() ? view->mode : SidebarMode::None;
}

WorkspaceShell::SidebarMode WorkspaceShell::ActiveSidebarMode() const {
  return SidebarModeForViewId(context_.current_project_state.sidebar.view_id);
}

const WorkspaceShell::ActionSpec* WorkspaceShell::FindActionSpec(ActionId id) {
  return FindWorkspaceActionSpec(id);
}

const WorkspaceShell::ActionSpec* WorkspaceShell::FindActionByCommand(std::string_view command_name) {
  return FindWorkspaceActionByCommand(command_name);
}

const std::vector<std::string>& WorkspaceShell::CommandNames() {
  return WorkspaceCommandNames();
}

std::vector<std::string> WorkspaceShell::DocumentedCommandUsages() {
  return WorkspaceDocumentedCommandUsages();
}

const std::vector<ResolvedKeybinding>& WorkspaceShell::ResolvedKeybindings() const {
  if (resolved_keybindings_reload_generation_ != reload_plugins_invocation_count_ ||
      resolved_keybindings_disabled_ids_snapshot_ != context_.disabled_keybinding_ids) {
    resolved_keybindings_cache_ =
        ResolveKeybindings(plugin_runtime_.Host(), context_.disabled_keybinding_ids);
    resolved_keybindings_disabled_ids_snapshot_ = context_.disabled_keybinding_ids;
    resolved_keybindings_reload_generation_ = reload_plugins_invocation_count_;
  }
  return resolved_keybindings_cache_;
}

ActionAvailability WorkspaceShell::MakeActionAvailability() const {
  return Bootstrapper(*const_cast<WorkspaceShell*>(this)).BuildActionAvailability();
}

std::span<const WorkspaceShell::MenuSpec> WorkspaceShell::MenuSpecs() {
  return WorkspaceMenuSpecs();
}

const WorkspaceShell::MenuSpec* WorkspaceShell::FindMenuSpec(MenuId id) {
  return FindWorkspaceMenuSpec(id);
}

std::span<const WorkspaceShell::MenuItemSpec> WorkspaceShell::MenuItems(MenuId id) const {
  if (id == MenuId::SidebarMode) {
    const auto views =
        OrderedSidebarViews(plugin_runtime_.Host(),
                            context_.current_project_state.sidebar_policies);
    sidebar_mode_menu_items_.clear();
    sidebar_mode_menu_entries_.clear();
    sidebar_mode_menu_items_.reserve(views.size());
    sidebar_mode_menu_entries_.reserve(views.size());

    for (const SidebarViewInfo& view : views) {
      if (view.id == "chat" || view.id == "problems" || view.id == "tests" ||
          view.id == "outline") {
        continue;
      }
      sidebar_mode_menu_entries_.push_back(
          SidebarModeMenuEntry{.label = std::string(view.label), .id = std::string(view.id)});
      const auto& entry = sidebar_mode_menu_entries_.back();
      sidebar_mode_menu_items_.push_back(MenuItemSpec{
          .action = ActionId::SidebarShow,
          .label = entry.label,
          .accelerator = {},
          .args = std::array<std::string_view, 2>{entry.id, {}},
          .arg_count = 1,
          .separator = false,
          .checkable = true,
          .submenu = MenuId::None,
          .command_name = {},
      });
    }

    return sidebar_mode_menu_items_;
  }

  if (id == MenuId::GitOutgoingBase) {
    static const auto kItems = std::to_array<MenuItemSpec>({
        MenuItemSpec{
            .action = ActionId::Colorscheme,
            .label = "Auto (base branch)",
            .accelerator = {},
            .args = {},
            .arg_count = 0,
            .separator = false,
            .checkable = false,
            .submenu = MenuId::None,
            .command_name = {},
        },
        MenuItemSpec{
            .action = ActionId::Colorscheme,
            .label = "Previous commit (HEAD~1)",
            .accelerator = {},
            .args = {},
            .arg_count = 0,
            .separator = false,
            .checkable = false,
            .submenu = MenuId::None,
            .command_name = {},
        },
        MenuItemSpec{
            .action = ActionId::Colorscheme,
            .label = "Branch or commit...",
            .accelerator = {},
            .args = {},
            .arg_count = 0,
            .separator = false,
            .checkable = false,
            .submenu = MenuId::None,
            .command_name = {},
        },
    });
    return kItems;
  }

  const MenuSpec* menu = FindWorkspaceMenuSpec(id);
  if (menu == nullptr) {
    return {};
  }

  const auto contributed = ContributedMenuItems(id, plugin_runtime_.Host());
  if (contributed.empty()) {
    return menu->items;
  }

  auto& entries = dynamic_menu_entries_[MenuSlotIndex(id)];
  auto& items = dynamic_menu_items_[MenuSlotIndex(id)];
  entries.clear();
  items.clear();
  entries.reserve(contributed.size());
  items.reserve(menu->items.size() + contributed.size() * 2);

  for (const MenuItemSpec& item : menu->items) {
    items.push_back(item);
  }
  for (const auto& contributed_item : contributed) {
    if (contributed_item.separator_before &&
        (items.empty() || !items.back().separator)) {
      items.push_back(MenuItemSpec{
          .action = ActionId::Colorscheme,
          .label = {},
          .accelerator = {},
          .args = {},
          .arg_count = 0,
          .separator = true,
          .checkable = false,
          .submenu = MenuId::None,
          .command_name = {},
      });
    }

    entries.push_back(DynamicMenuEntryStorage{
        .label = contributed_item.label,
        .accelerator = contributed_item.accelerator,
        .command_name = contributed_item.action,
    });
    const auto& entry = entries.back();
    const ActionSpec* contributed_action = FindActionByCommand(entry.command_name);
    items.push_back(MenuItemSpec{
        .action = contributed_action != nullptr ? contributed_action->id : ActionId::Colorscheme,
        .label = entry.label,
        .accelerator = entry.accelerator,
        .args = {},
        .arg_count = 0,
        .separator = false,
        .checkable = false,
        .submenu = MenuId::None,
        .command_name = entry.command_name,
    });
  }

  return items;
}

bool WorkspaceShell::ExecuteCustomMenuItem(MenuId id, std::size_t item_index) {
  if (id != MenuId::GitOutgoingBase) {
    return false;
  }

  switch (item_index) {
    case 0:
      SetGitOutgoingBaseChoice(OutgoingBaseChoice{
          .kind = OutgoingBaseChoice::Kind::Auto,
          .custom_ref = {},
      });
      return true;
    case 1:
      SetGitOutgoingBaseChoice(OutgoingBaseChoice{
          .kind = OutgoingBaseChoice::Kind::PreviousCommit,
          .custom_ref = {},
      });
      return true;
    case 2:
      OpenOutgoingBaseRefPicker();
      return true;
    default:
      return false;
  }
}

bool WorkspaceShell::ExecuteCommandName(std::string_view command_name,
                                        const std::vector<std::string>& args,
                                        ActionSource source,
                                        std::string* error_message) {
  if (const ActionSpec* action = FindActionByCommand(command_name); action != nullptr) {
    if (error_message != nullptr) {
      error_message->clear();
    }
    return ActionCoordinator(MakeActionContext()).Execute(action->id, args, source);
  }
  return plugin_runtime_.Host().ExecuteCommand(command_name, args, error_message);
}

std::optional<std::string> WorkspaceShell::GetSettingValue(std::string_view id) const {
  if (id == "ui.scale") {
    return SerializeSettingValue(ui_scale_);
  }
  if (id == "editor.tab_size") {
    return SerializeSettingValue(static_cast<int>(context_.current_project_state.editor_preferences.tab_size));
  }
  if (id == "editor.indent_width") {
    return SerializeSettingValue(
        static_cast<int>(context_.current_project_state.editor_preferences.indent_width));
  }
  if (id == "editor.soft_tabs") {
    return SerializeSettingValue(context_.current_project_state.editor_preferences.soft_tabs);
  }
  if (id == "editor.wrap") {
    return SerializeSettingValue(
        context_.current_project_state.editor_preferences.soft_wrap ? std::string("word")
                                                                    : std::string("off"));
  }
  if (id == "editor.colorscheme") {
    return SerializeSettingValue(context_.current_project_state.active_colorscheme_name);
  }

  // Layered overrides (user wins over project), resolved in O(1) via the store's
  // index rather than two linear scans — this is read 10+ times per frame.
  if (const std::string* resolved = settings_store_.Resolve(id); resolved != nullptr) {
    return *resolved;
  }

  if (const auto info = FindSettingInfo(id, plugin_runtime_.Host()); info.has_value()) {
    return SerializeSettingValue(info->default_value);
  }
  return std::nullopt;
}

std::vector<std::string> WorkspaceShell::OrderedSidebarViewIds() const {
  std::vector<std::string> ids;
  const auto views =
      OrderedSidebarViews(plugin_runtime_.Host(),
                          context_.current_project_state.sidebar_policies);
  ids.reserve(views.size());
  for (const SidebarViewInfo& view : views) {
    ids.emplace_back(view.id);
  }
  return ids;
}

void WorkspaceShell::NormalizeSidebarViewSelection() {
  auto& sidebar = context_.current_project_state.sidebar;
  const auto visible_views =
      OrderedSidebarViews(plugin_runtime_.Host(),
                          context_.current_project_state.sidebar_policies);
  const auto contains_visible_view = [&](std::string_view id) {
    return std::any_of(visible_views.begin(), visible_views.end(),
                       [id](const SidebarViewInfo& view) { return view.id == id; });
  };
  const std::string fallback_view =
      visible_views.empty() ? std::string{} : std::string(visible_views.front().id);

  if (!sidebar.view_id.empty() && !contains_visible_view(sidebar.view_id)) {
    sidebar.view_id = fallback_view;
    sidebar.scroll_row = 0;
  }
  if (!sidebar.prev_view_id.empty() && !contains_visible_view(sidebar.prev_view_id)) {
    sidebar.prev_view_id.clear();
  }
  if (!sidebar.visible) {
    return;
  }
  if (!sidebar.view_id.empty()) {
    return;
  }

  sidebar.visible = false;
  sidebar.temporary = false;
  sidebar.prev_view_id.clear();
  if (context_.current_project_state.surface.focus == FocusTarget::Sidebar) {
    context_.current_project_state.surface.focus = FocusTarget::Editor;
  }
}

std::vector<WorkspaceShell::VisibleStatusItem> WorkspaceShell::ComputeVisibleStatusItems(
    const SDL_FRect& breadcrumb) const {
  static constexpr float kItemPadding = 8.0f;
  static constexpr float kItemGap = 6.0f;
  static constexpr float kInset = 12.0f;
  static constexpr float kIconSlot = 16.0f;
  static constexpr float kProgressSlot = 34.0f;
  std::vector<StatusItemView> items = ResolveStatusItems(plugin_runtime_.Host());

  std::vector<VisibleStatusItem> visible;
  visible.reserve(items.size());

  // Item width = text + padding, plus a fixed slot for a leading icon and/or a
  // trailing progress bar when present. Kept in sync with the render layout in
  // WorkspaceShellRenderChrome.cpp.
  const auto item_width = [&](const StatusItemView& item) {
    float width = text_renderer_.MeasureWidth(item.text) + kItemPadding * 2.0f;
    if (!item.icon.empty() && editor::GutterIconRegistry::ResolveShape(item.icon)) {
      width += kIconSlot;
    }
    if (item.progress >= 0.0f) {
      width += kProgressSlot;
    }
    return width;
  };

  float right_x = breadcrumb.x + breadcrumb.w - kInset;
  for (const StatusItemView& item : items) {
    if (item.alignment != StatusAlignment::Right || item.text.empty()) {
      continue;
    }
    const float width = item_width(item);
    const float next_x = right_x - width;
    if (next_x < breadcrumb.x + kInset) {
      continue;
    }
    visible.push_back(VisibleStatusItem{
        .item = item,
        .rect = MakeRect(next_x, breadcrumb.y + 3.0f, width,
                         std::max(18.0f, breadcrumb.h - 6.0f)),
        .hovered = last_mouse_position_valid_ &&
                   Contains(MakeRect(next_x, breadcrumb.y + 3.0f, width,
                                     std::max(18.0f, breadcrumb.h - 6.0f)),
                            last_mouse_x_, last_mouse_y_),
    });
    right_x = next_x - kItemGap;
  }

  float left_x = breadcrumb.x + kInset;
  const float left_limit = right_x;
  for (const StatusItemView& item : items) {
    if (item.alignment != StatusAlignment::Left || item.text.empty()) {
      continue;
    }
    const float width = item_width(item);
    if (left_x + width > left_limit) {
      break;
    }
    visible.push_back(VisibleStatusItem{
        .item = item,
        .rect = MakeRect(left_x, breadcrumb.y + 3.0f, width,
                         std::max(18.0f, breadcrumb.h - 6.0f)),
        .hovered = last_mouse_position_valid_ &&
                   Contains(MakeRect(left_x, breadcrumb.y + 3.0f, width,
                                     std::max(18.0f, breadcrumb.h - 6.0f)),
                            last_mouse_x_, last_mouse_y_),
    });
    left_x += width + kItemGap;
  }

  return visible;
}

std::string WorkspaceShell::HoveredStatusTooltip(const SDL_FRect& breadcrumb) const {
  if (MenuSurfaceCapturingMouse()) {
    return {};
  }
  for (const VisibleStatusItem& item : ComputeVisibleStatusItems(breadcrumb)) {
    if (item.hovered && !item.item.tooltip.empty()) {
      return item.item.tooltip;
    }
  }
  return {};
}

const project::TreeEntry* WorkspaceShell::SelectedTreeEntry() const {
  if (ActiveSidebarMode() != SidebarMode::Tree) {
    return nullptr;
  }
  const auto& entries = context_.current_project_state.directory_tree.entries();
  if (context_.current_project_state.directory_tree.selected_index() >= entries.size()) {
    return nullptr;
  }
  return &entries[context_.current_project_state.directory_tree.selected_index()];
}

WorkspaceShell::TreeContextTargetKind WorkspaceShell::SelectedTreeTargetKind() const {
  const project::TreeEntry* entry = SelectedTreeEntry();
  if (entry == nullptr) {
    return TreeContextTargetKind::None;
  }
  if (!entry->is_directory) {
    return TreeContextTargetKind::File;
  }
  return entry->path == context_.current_project_state.root ? TreeContextTargetKind::Root
                                      : TreeContextTargetKind::Directory;
}

}  // namespace microide::workspace
