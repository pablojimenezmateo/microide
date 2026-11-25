#include "workspace/render/CompareReviewControls.h"
#include "workspace/shell/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <tuple>
#include <variant>

#include "util/PerformanceCounters.h"
#include "workspace/PluginSurfacePreview.h"
#include "workspace/ProjectSearchPanelLayout.h"
#include "workspace/WorkspaceProjectSearchPresentation.h"
#include "workspace/debug/DebugPaneRegistry.h"
#include "workspace/git/GitSidebarHeaderLayout.h"
#include "workspace/git/WorkspaceGitSidebarPresentation.h"
#include "workspace/WorkspaceUiText.h"
#include "workspace/coordinators/EditorPointerHit.h"
#include "workspace/control/ControlUiQuery.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/render/CodeLensLayout.h"
#include "workspace/render/CompareMergeRender.h"
#include "workspace/render/EditorInteractionGrid.h"
#include "workspace/render/DiffDividerGeometry.h"
#include "workspace/registries/WorkspaceCommandRegistry.h"
#include "workspace/actions/WorkspaceActionServices.h"
#include "workspace/render/RenderViewModelBuilder.h"
#include "workspace/SettingFlags.h"
#include "editor/EditorViewRenderer.h"
#include "editor/GutterMetrics.h"

namespace microide::workspace {

namespace {

bool UiRectHasArea(const SDL_FRect& rect) { return rect.w > 0.0f && rect.h > 0.0f; }

util::JsonValue TruncatedRectJson(const SDL_FRect& rect) {
  util::JsonArray array;
  array.reserve(4);
  for (const float v : {rect.x, rect.y, rect.w, rect.h}) {
    array.push_back(util::JsonValue(static_cast<std::int64_t>(v)));
  }
  return util::JsonValue(std::move(array));
}

util::JsonValue FloatJson(float value) { return util::JsonValue(static_cast<double>(value)); }

util::JsonValue IntJson(std::int64_t value) { return util::JsonValue(value); }

const char* QuickPickKindName(QuickPickKind kind) {
  switch (kind) {
    case QuickPickKind::ColorTheme: return "color-theme";
    case QuickPickKind::DocumentSymbol: return "document-symbol";
    case QuickPickKind::WorkspaceSymbol: return "workspace-symbol";
    case QuickPickKind::LanguageMode: return "language-mode";
    case QuickPickKind::RecentEditor: return "recent-editor";
    case QuickPickKind::RunCommand: return "command-choices";
    case QuickPickKind::LaunchConfig: return "launch-config";
  }
  return "launch-config";
}

const char* ActionSourceName(ActionSource source) {
  switch (source) {
    case ActionSource::Command: return "command";
    case ActionSource::Shortcut: return "shortcut";
    case ActionSource::Menu: return "menu";
    case ActionSource::ContextMenu: return "context-menu";
  }
  return "command";
}

template <typename PanelLayout>
util::JsonValue LogSurfaceRowsJson(util::JsonObject& out, const PanelLayout& panel_layout) {
  out["textX"] = FloatJson(panel_layout.text_x);
  out["firstLineY"] = FloatJson(panel_layout.text_y);
  out["lineHeight"] = FloatJson(panel_layout.line_height);
  out["firstRow"] = IntJson(std::max(0, panel_layout.scroll.vertical_scroll));
  out["visibleRows"] = IntJson(std::max(0, panel_layout.scroll.visible_rows));
  return util::JsonValue(std::move(out));
}

void AddOverflowChips(ControlUiElements& out, const std::string& strip,
                      const TabStripOverflowControls& overflow) {
  if (overflow.hidden_left > 0) {
    AddControlUiElement(out, control_ui_kind::kButton, strip + "/more-left", overflow.left_button);
  }
  if (overflow.hidden_right > 0) {
    AddControlUiElement(out, control_ui_kind::kButton, strip + "/more-right", overflow.right_button);
  }
}

const editor::SingleLineEditor* OverlayQueryEditor(const ProjectWorkspaceState& state) {
  const auto& workflow = state.overlay.workflow;
  switch (state.overlay.mode) {
    case OverlayMode::FileFinder: return &state.file_finder.query_state();
    case OverlayMode::ProjectSearch: return &workflow.project_search.query;
    case OverlayMode::CommitPicker: return &workflow.compare_picker.query;
    case OverlayMode::QuickPick: return &workflow.quick_pick.query;
    case OverlayMode::CommandPalette: return &workflow.command_palette.query;
    case OverlayMode::BufferSearch:
    case OverlayMode::BufferReplace: return &workflow.buffer_search.query;
    case OverlayMode::Completion:
    case OverlayMode::CodeActions: break;
  }
  return nullptr;
}

std::string_view DebugToolbarButtonId(DebugToolbarButton button) {
  switch (button) {
    case DebugToolbarButton::ContinuePause: return "continue";
    case DebugToolbarButton::StepOver: return "step-over";
    case DebugToolbarButton::StepInto: return "step-into";
    case DebugToolbarButton::StepOut: return "step-out";
    case DebugToolbarButton::ReverseContinue: return "reverse-continue";
    case DebugToolbarButton::StepBack: return "step-back";
    case DebugToolbarButton::Restart: return "restart";
    case DebugToolbarButton::Stop: return "stop";
    case DebugToolbarButton::Count: break;
  }
  return "unknown";
}

std::string DebugValueRowLabel(const DebugVariableRowView& row) {
  std::string label = row.display_name;
  if (!row.display_value.empty()) {
    label += " = ";
    label += row.display_value;
  }
  if (!row.display_type.empty()) {
    label += " : ";
    label += row.display_type;
  }
  return label;
}

}

ControlUiQuery::Operations WorkspaceShell::MakeControlUiOperations() {
  using namespace control_ui_kind;
  const auto cursor_name = [](CursorKind kind) { return CursorKindName(kind); };
  const auto cursor_at = [this, cursor_name](float x, float y) {
    return cursor_name(CursorKindForPosition(x, y));
  };
  return ControlUiQuery::Operations{
      .layout = [this]() { return CurrentWorkspaceLayout(); },
      .window_per_unit = [this]() { return window_presentation_.window_per_unit; },
      .cursor_at = cursor_at,
      .pointer_cursor =
      [this, cursor_name]() -> std::string_view {
        return last_mouse_position_valid_ ? cursor_name(cursor_kind_) : std::string_view{};
      },
      .menu_label =
          [](MenuId id) {
            const MenuSpec* spec = FindMenuSpec(id);
            return spec == nullptr ? std::string{} : std::string(spec->label);
          },
      .menu_row_locator =
          [this](MenuId id, std::size_t index) {
            const MenuSpec* spec = FindMenuSpec(id);
            std::string locator =
                spec == nullptr || spec->label.empty() ? std::string("menu") : std::string(spec->label);
            const auto items = MenuItems(id);
            locator += '/';
            if (index < items.size()) {
              locator += MenuItemLabel(items[index], false);
            }
            return locator;
          },
      .modal = [this]() -> std::string {
        if (context_.prompts.dirty_visible) {
          return "unsaved-prompt";
        }
        if (context_.prompts.surface_visible) {
          return "prompt";
        }
        if (settings_overlay_service_.Visible()) {
          return settings_overlay_service_.Mode() == SettingsOverlayMode::HelpAbout ? "help" : "settings";
        }
        return {};
      },
      .menus = [this](const WorkspaceLayout& layout, ControlUiElements& out) { ControlUiMenus(layout, out); },
      .tabs = [this](const WorkspaceLayout& layout, ControlUiElements& out) { ControlUiTabs(layout, out); },
      .sidebar = [this](const WorkspaceLayout& layout, ControlUiElements& out) { ControlUiSidebar(layout, out); },
      .panel = [this](const WorkspaceLayout& layout, ControlUiElements& out) { ControlUiPanel(layout, out); },
      .status_bar =
          [this](const WorkspaceLayout& layout, ControlUiElements& out) {
            const StatusBarViewModel vm =
                RenderViewModelBuilder(context_).BuildStatusBar(layout, status_bar_service_);
            ForEachStatusBarSegmentRect(
                vm, text_renderer_,
                [&](const StatusBarSegmentViewModel& segment, const SDL_FRect& rect) {
                  AddControlUiElement(out, kStatus, StatusBarSegmentName(segment.id), rect,
                                      std::string(segment.text));
                  out.back().enabled = segment.clickable;
                  if (const MenuSpec* menu = FindMenuSpec(segment.menu); menu != nullptr) {
                    out.back().menu = std::string(menu->label);
                  }
                });
            for (const VisibleStatusItem& item : ComputeVisibleStatusItems(layout.breadcrumb)) {
              std::string_view id = item.item.id;
              if (const std::string prefix = item.item.plugin_id + "."; id.starts_with(prefix)) {
                id.remove_prefix(prefix.size());
              }
              AddControlUiElement(out, kStatus, "plugin/" + item.item.plugin_id + "/" + std::string(id),
                                  item.rect, item.item.text);
              out.back().enabled = !item.item.command.empty();
            }
          },
      .buttons = [this](const WorkspaceLayout& layout, ControlUiElements& out) { ControlUiButtons(layout, out); },
      .overlay = [this](const WorkspaceLayout& layout, ControlUiElements& out) { ControlUiOverlay(layout, out); },
      .dialogs = [this](const WorkspaceLayout& layout, ControlUiElements& out) { ControlUiDialogs(layout, out); },
      .fields = [this](const WorkspaceLayout& layout, ControlUiElements& out) { ControlUiFields(layout, out); },
      .debug = [this](const WorkspaceLayout& layout, ControlUiElements& out) { ControlUiDebug(layout, out); },
      .details = [this](const WorkspaceLayout& layout, util::JsonObject& root) { ControlUiDetails(layout, root); },
      .action_counts =
          [this]() {
            const WorkspaceActionContext::ActionReceipt& receipt =
                MakeActionContext().LastActionReceipt();
            util::JsonObject commands;
            for (const ActionSpec& spec : WorkspaceCommandSpecs()) {
              const auto index = static_cast<std::size_t>(spec.id);
              if (index < receipt.counts.size() && receipt.counts[index] > 0) {
                commands[std::string(spec.command_name.empty() ? spec.label : spec.command_name)] =
                    util::JsonValue(static_cast<std::int64_t>(receipt.counts[index]));
              }
            }
            util::JsonObject plugins;
            for (const auto& [command, count] : receipt.plugin_counts) {
              plugins[command] = util::JsonValue(static_cast<std::int64_t>(count));
            }
            util::JsonObject root;
            root["dispatched"] = util::JsonValue(static_cast<std::int64_t>(receipt.dispatched));
            root["commands"] = util::JsonValue(std::move(commands));
            root["plugins"] = util::JsonValue(std::move(plugins));
            return util::JsonValue(std::move(root));
          },
  };
}

void WorkspaceShell::ControlUiMenus(const WorkspaceLayout& layout, ControlUiElements& out) {
  using namespace control_ui_kind;
  const MenuSurfaceState& menus = context_.menu_state;
  const auto menu_name = [](MenuId id) {
    const MenuSpec* spec = FindMenuSpec(id);
    return spec == nullptr || spec->label.empty() ? std::string("menu") : std::string(spec->label);
  };
  for (const auto& item : ComputeVisibleMenuBarItems(layout.menu_bar)) {
    const std::string name = menu_name(item.id);
    AddControlUiElement(out, kMenu, name, item.rect, name);
    out.back().active = menus.menu_bar_open && menus.active_menu_id == item.id;
  }
  if (const auto chevron = MenuOverflowChevronRect(layout.menu_bar); chevron.has_value()) {
    AddControlUiElement(out, kButton, "menu-bar/more", *chevron);
    out.back().active = menus.overflow_popup_open;
  }
  for (const auto& button : ComputeVisibleWindowControlButtons(layout.menu_bar)) {
    const std::string_view name = button.id == WindowControlButtonId::Minimize   ? "window/minimize"
                                  : button.id == WindowControlButtonId::Maximize ? "window/maximize"
                                                                                 : "window/close";
    AddControlUiElement(out, kButton, name, button.rect);
  }
  const auto add_rows = [&](std::span<const MenuItemSpec> items, int active_index, const SDL_FRect& popup,
                            const std::string& owner) {
    for (const VisiblePopupMenuItem& row : ComputeVisiblePopupMenuItems(items, active_index, popup)) {
      if (row.separator) {
        continue;
      }
      const std::string label(MenuItemLabel(items[row.index], false));
      AddControlUiElement(out, kMenuItem, owner + "/" + label, row.rect, label);
      out.back().enabled = row.enabled;
      out.back().active = row.hovered;
      if (items[row.index].submenu == MenuId::None) {
        for (const ActionSpec& spec : WorkspaceCommandSpecs()) {
          if (spec.id == items[row.index].action) {
            out.back().command = std::string(spec.command_name.empty() ? spec.label : spec.command_name);
            break;
          }
        }
      }
    }
  };
  if (menus.menu_bar_open) {
    if (const auto popup = ComputePopupMenuRect(layout.menu_bar, menus.active_menu_id); popup.has_value()) {
      add_rows(MenuItems(menus.active_menu_id), menus.active_menu_item_index, *popup, menu_name(menus.active_menu_id));
    }
    if (menus.active_submenu_id != MenuId::None) {
      if (const auto submenu = ActiveSubmenuRect(layout.menu_bar); submenu.has_value()) {
        add_rows(MenuItems(menus.active_submenu_id), menus.active_submenu_item_index, *submenu,
                 menu_name(menus.active_submenu_id));
      }
    }
  }
  if (menus.tree_context_menu.open) {
    if (const auto popup = ComputeTreeContextMenuRect(); popup.has_value()) {
      add_rows(WorkspaceTreeContextMenuItems(menus.tree_context_menu.target), menus.tree_context_menu.active_item_index,
               *popup, "context");
    }
  }
  if (const auto chevron = MenuOverflowChevronRect(layout.menu_bar); menus.overflow_popup_open && chevron.has_value()) {
    const auto overflow = ComputeOverflowMenuBarItems(layout.menu_bar);
    const SDL_FRect popup = ComputeMenuOverflowPopupRect(*chevron, overflow.size());
    for (std::size_t i = 0; i < overflow.size(); ++i) {
      const std::string label = menu_name(overflow[i]);
      AddControlUiElement(out, kMenuItem, "more/" + label, MenuOverflowPopupRowRect(popup, i), label);
      out.back().active = menus.overflow_popup_active_index == static_cast<int>(i);
    }
  }
}

void WorkspaceShell::ControlUiTabs(const WorkspaceLayout& layout, ControlUiElements& out) {
  using namespace control_ui_kind;
  if (UiRectHasArea(layout.project_tab_strip)) {
    const auto& project_tabs = tab_strip_chrome_.ComputeVisibleProjectTabs(layout.project_tab_strip);
    for (const VisibleStripTab& tab : project_tabs) {
      AddControlUiElement(out, kProjectTab, tab.display_title, tab.rect, tab.display_title);
      out.back().active = tab.active;
    }
    AddOverflowChips(out, "project-tabs",
                     tab_strip_chrome_.ComputeProjectTabOverflowControls(layout.project_tab_strip, project_tabs));
  }
  if (!HasActiveProjectCatalogEntry()) {
    return;
  }
  const EditorGroupRectsLayout groups = ComputeEditorGroupRectsForState(layout);
  for (std::size_t gi = 0; gi < groups.groups.size(); ++gi) {
    const SDL_FRect strip = groups.groups[gi].tab_strip;
    if (!UiRectHasArea(strip)) {
      continue;
    }
    const std::string group = std::to_string(gi);
    const auto& group_tabs = tab_strip_chrome_.ComputeVisibleTabsForGroup(gi, strip);
    for (const VisibleStripTab& tab : group_tabs) {
      const std::string locator = group + "/" + tab.display_title;
      AddControlUiElement(out, kTab, locator, tab.rect, tab.display_title);
      out.back().active = tab.active;
      if (UiRectHasArea(tab.close_rect)) {
        AddControlUiElement(out, kButton, "close-tab/" + locator, tab.close_rect);
      }
    }
    AddOverflowChips(out, "tabs/" + group, tab_strip_chrome_.ComputeTabOverflowControlsForGroup(gi, strip, group_tabs));
  }
  for (std::size_t i = 0; i < groups.dividers.size(); ++i) {
    AddControlUiElement(out, kDivider, "editor/" + std::to_string(i), groups.dividers[i].rect);
  }
  const bool line_numbers = SettingFlagEnabled(GetSettingValue("editor.line_numbers"), true);
  for (const EditorPaneLayout& pane : ComputeEditorPaneLayouts(layout)) {
    const editor::TextViewport* viewport = ViewportForPane(pane);
    if (viewport == nullptr || viewport->is_placeholder()) {
      continue;
    }
    const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
        editor_text_renderer_, *viewport, pane.rect, viewport->sticky_band_rows(), line_numbers);
    const ScrollSurfaceLayout scroll = ComputeEditorScrollLayout(pane.rect, *viewport, metrics);
    const std::string group = "editor/" + std::to_string(pane.group_index);
    if (scroll.vertical_scrollbar.has_value()) {
      AddControlUiScrollbar(out, group, *scroll.vertical_scrollbar);
    }
    if (scroll.horizontal_scrollbar.has_value()) {
      AddControlUiScrollbar(out, group + "/h", *scroll.horizontal_scrollbar);
    }
  }
}

void WorkspaceShell::ControlUiSidebar(const WorkspaceLayout& layout, ControlUiElements& out) {
  using namespace control_ui_kind;
  if (!UiRectHasArea(layout.sidebar)) {
    return;
  }
  const ProjectWorkspaceState& state = context_.current_project_state;
  const SidebarModeRowLayout row = SidebarModeRow(layout.sidebar);
  for (int i = 0; i < row.tab_count; ++i) {
    const SidebarModeTab& tab = row.tabs[static_cast<std::size_t>(i)];
    AddControlUiElement(out, kSidebarView, tab.id, tab.rect, std::string(tab.id));
    out.back().active = tab.id == state.sidebar.view_id;
  }
  if (row.has_overflow) {
    AddControlUiElement(out, kButton, "sidebar/more", row.overflow_rect);
  }
  const SidebarMode mode = ActiveSidebarMode();
  if (mode == SidebarMode::Tree) {
    AddControlUiElement(out, kButton, "sidebar/Collapse", TreeSidebarCollapseButtonRect(layout.sidebar));
    out.back().enabled = state.directory_tree.CanCollapseAll();
    AddControlUiElement(out, kButton, "sidebar/Refresh", TreeSidebarRefreshButtonRect(layout.sidebar));
  }
  ScrollableListLayout list;
  std::size_t count = 0;
  std::size_t selected = 0;
  std::function<std::string(std::size_t)> label;
  std::function<std::string(std::size_t)> shown;
  std::function<bool(std::size_t)> clickable;
  if (mode == SidebarMode::Search) {
    const std::vector<int> line_map = BuildProjectSearchLineMap();
    const auto& search = state.overlay.workflow.project_search;
    count = line_map.size();
    list = ComputeProjectSearchSidebarListLayout(layout.sidebar, count);
    selected = static_cast<std::size_t>(std::max(0, FindProjectSearchResultLine(line_map, search.selected_index)));
    label = [line_map, &search](std::size_t index) -> std::string {
      const int result = line_map[index];
      if (result >= 0) {
        const auto& hit = search.results[static_cast<std::size_t>(result)];
        return hit.relative_path_string + ":" + std::to_string(hit.line + 1);
      }
      const std::size_t next = index + 1;
      return next < line_map.size() && line_map[next] >= 0
                 ? search.results[static_cast<std::size_t>(line_map[next])].relative_path_string
                 : std::string{};
    };
    shown = [line_map, &search, label](std::size_t index) -> std::string {
      const int result = line_map[index];
      return result >= 0 ? search.results[static_cast<std::size_t>(result)].preview : label(index);
    };
  } else if (mode == SidebarMode::Git) {
    const std::vector<GitSidebarLine>& lines = BuildGitSidebarLines();
    count = lines.size();
    list = ComputeGitSidebarListLayout(layout.sidebar, count);
    selected = std::numeric_limits<std::size_t>::max();
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (lines[i].kind == GitSidebarLine::Kind::Entry &&
          lines[i].entry_index == static_cast<int>(state.sidebar.git.selected_index)) {
        selected = i;
        break;
      }
    }
    label = [&lines](std::size_t index) { return lines[index].label; };
    shown = [&lines](std::size_t index) {
      return lines[index].display_label.empty() ? lines[index].label : lines[index].display_label;
    };
    clickable = [&lines](std::size_t index) {
      return lines[index].kind == GitSidebarLine::Kind::Directory ||
             (lines[index].kind == GitSidebarLine::Kind::Entry && lines[index].entry_index >= 0);
    };
  } else if (mode == SidebarMode::Problems) {
    const auto& entries = state.sidebar.problems.entries;
    count = entries.size();
    list = ComputeProblemsSidebarListLayout(layout.sidebar, count);
    selected = state.sidebar.problems.selected_index;
    label = [&entries](std::size_t index) { return entries[index].primary_label; };
    shown = [&entries](std::size_t index) { return entries[index].primary_label + "  " + entries[index].detail_label; };
  } else if (mode == SidebarMode::Tests) {
    const auto& entries = state.sidebar.tests.entries;
    count = entries.size();
    list = ComputeTestsSidebarListLayout(layout.sidebar, count);
    selected = state.sidebar.tests.selected_index;
    label = [&entries](std::size_t index) { return entries[index].label; };
    shown = [&entries](std::size_t index) { return entries[index].label + "  " + entries[index].detail_label; };
  } else if (UsesPluginItemSurface(mode)) {
    const auto& items = state.sidebar.plugin.items;
    count = items.size();
    list = ComputePluginSidebarListLayout(layout.sidebar, count);
    selected = state.sidebar.plugin.selected_index;
    label = [&items](std::size_t index) { return items[index].label; };
    shown = [&items](std::size_t index) {
      return items[index].detail.empty() ? items[index].label : items[index].label + "  " + items[index].detail;
    };
  } else {
    const auto& entries = state.directory_tree.entries();
    count = entries.size();
    list = ComputeTreeSidebarListLayout(layout.sidebar, count);
    selected = state.directory_tree.selected_index();
    label = [&entries](std::size_t index) { return entries[index].label; };
  }
  const std::string view = state.sidebar.view_id + "/";
  for (int visible = 0; visible < list.visible_rows; ++visible) {
    const std::size_t index = static_cast<std::size_t>(list.scroll_row + visible);
    if (index >= count) {
      break;
    }
    std::string text = label(index);
    AddControlUiElement(out, kRow, view + (text.empty() ? "#" + std::to_string(index) : text),
                        ScrollableListRowRect(list, visible), shown ? shown(index) : text);
    out.back().selected = index == selected;
    if (clickable) {
      out.back().enabled = clickable(index);
    }
  }
  if (const auto bar = MakeVerticalScrollbarGeometry(list.list_rect, static_cast<float>(count), list.visible_units,
                                                     static_cast<float>(list.scroll_row));
      bar.has_value()) {
    AddControlUiScrollbar(out, "sidebar", *bar);
  }
}

void WorkspaceShell::ControlUiPanel(const WorkspaceLayout& layout, ControlUiElements& out) {
  using namespace control_ui_kind;
  if (!BottomPanelVisible() || !UiRectHasArea(layout.bottom_panel)) {
    return;
  }
  const SDL_FRect header =
      MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w, kWorkspaceBottomPanelHeaderHeight);
  const LayoutMode mode = layout_mode_service_.CurrentMode();
  const auto& panel_tabs = tab_strip_service_.ComputeVisibleBottomPanelTabs(
      context_.current_project_state, header, mode,
      [this](std::string_view text) { return text_renderer_.MeasureWidth(text); }, output_channels_.Channels());
  for (const VisibleStripTab& tab : panel_tabs) {
    AddControlUiElement(out, kPanelTab, tab.display_title, tab.rect, tab.display_title);
    out.back().active = tab.active;
    if (UiRectHasArea(tab.close_rect)) {
      AddControlUiElement(out, kButton, "close-panel-tab/" + tab.display_title, tab.close_rect);
    }
  }
  AddOverflowChips(out, "panel-tabs",
                   tab_strip_service_.ComputeBottomPanelTabOverflowControls(
                       context_.current_project_state, header, mode, panel_tabs, output_channels_.Channels()));
  if (terminal_find_service_.visible() && prepare_cached_bottom_panel_vm_.has_value() &&
      prepare_cached_bottom_panel_vm_->find_visible) {
    const FindWidgetLayout& fw = prepare_cached_bottom_panel_vm_->find.fw;
    AddControlUiElement(out, kField, "terminal-find/query", fw.search_field, terminal_find_service_.query().text());
    out.back().active = terminal_find_service_.focused();
    AddControlUiElement(out, kButton, "terminal-find/match-case", fw.toggle_buttons[0]);
    AddControlUiElement(out, kButton, "terminal-find/whole-word", fw.toggle_buttons[1]);
    AddControlUiElement(out, kButton, "terminal-find/previous", fw.prev_button);
    AddControlUiElement(out, kButton, "terminal-find/next", fw.next_button);
    AddControlUiElement(out, kButton, "terminal-find/close", fw.close_button);
  }
  if (context_.current_project_state.panel.content == PanelContentKind::Terminal) {
    AddControlUiElement(out, kButton, "panel/new-terminal",
                        tab_strip_service_.BottomPanelTerminalNewTabRect(mode, header));
    if (const auto* tab = context_.current_project_state.active_terminal_tab(); tab != nullptr) {
      for (const TerminalPaneDividerRect& divider : tab->PaneRects(BottomPanelContentRect(layout)).dividers) {
        AddControlUiElement(out, kDivider, "terminal/" + std::to_string(divider.boundary), divider.rect);
      }
    }
  }
  AddControlUiElement(out, kButton, "panel/maximize", tab_strip_service_.BottomPanelMaximizeButtonRect(mode, header));
  const ProjectWorkspaceState& ps = context_.current_project_state;
  if (ps.panel.content == PanelContentKind::Terminal) {
    if (const TerminalTabState* tab = ps.active_terminal_tab(); tab != nullptr) {
      const TerminalPaneRectsLayout rects = tab->PaneRects(BottomPanelContentRect(layout));
      for (std::size_t i = 0; i < tab->panes.size() && i < rects.panes.size(); ++i) {
        if (const TerminalPaneState* pane = tab->panes[i].get(); pane != nullptr) {
          const LogSurfaceLayout pane_layout =
              ComputeTerminalPaneLogLayout(rects.panes[i], layout.bottom_panel.h, *pane, pane->frame_line_count);
          if (pane_layout.scroll.vertical_scrollbar.has_value()) {
            AddControlUiScrollbar(out, "terminal/" + std::to_string(i), *pane_layout.scroll.vertical_scrollbar);
          }
        }
      }
    }
  } else if (ps.panel.content == PanelContentKind::Output) {
    const auto* entries = OutputChannelEntries(ps.panel.output.channel_id);
    const LogSurfaceLayout panel_layout = ComputeBottomPanelLogLayout(layout, entries != nullptr ? entries->size() : 0);
    if (panel_layout.scroll.vertical_scrollbar.has_value()) {
      AddControlUiScrollbar(out, "panel", *panel_layout.scroll.vertical_scrollbar);
    }
  } else if (ps.panel.content == PanelContentKind::PluginSurface) {
    const auto* pres = ps.plugin_presentation_if_present();
    if (const editor::SurfaceContent* content =
            pres != nullptr ? pres->surfaces.Find(ps.panel.surface_owner, ps.panel.surface_id) : nullptr;
        content != nullptr && content->has_body()) {
      const SDL_FRect body = BottomPanelContentRect(layout);
      if (const auto bar = MakeVerticalScrollbarGeometry(body, PluginSurfacePreviewContentHeight(*content), body.h,
                                                         static_cast<float>(ps.panel.surface_scroll_y));
          bar.has_value()) {
        AddControlUiScrollbar(out, "panel", *bar);
      }
    }
  }
}

namespace {

void AppendCompareControls(ControlUiElements& out, render::TextRenderer& text_renderer,
                           const CompareTabState& compare_tab, const SDL_FRect& surface_rect,
                           const CompareSurfaceLayout& surface) {
  using namespace control_ui_kind;
  AddControlUiElement(out, kDivider, "compare", CompareDividerHitRect(surface_rect, surface));
  const auto compare_scroll = ComputeCompareScrollLayout(surface_rect, surface, compare_tab);
  if (compare_scroll.vertical_scrollbar.has_value()) {
    AddControlUiScrollbar(out, "compare", *compare_scroll.vertical_scrollbar);
  }
  if (compare_scroll.horizontal_scrollbar.has_value()) {
    AddControlUiScrollbar(out, "compare/h", *compare_scroll.horizontal_scrollbar);
  }
  const CompareControlGeometry controls = MakeCompareControlGeometry(surface_rect, surface);
  const CompareHeaderButtonRects header =
      BuildCompareHeaderButtonRects(text_renderer, controls, compare_tab);
  for (const auto& [rect, name] :
       {std::pair{header.previous, "compare/previous-change"}, std::pair{header.next, "compare/next-change"},
        std::pair{header.file_reviewed, "compare/file-reviewed"}}) {
    if (rect.has_value()) {
      AddControlUiElement(out, kButton, name, *rect);
    }
  }
  for (int row = 0; row < surface.visible_rows; ++row) {
    const std::optional<CompareControlRow> control_row = CompareControlRowAt(compare_tab, row);
    if (!control_row.has_value()) {
      break;
    }
    if (control_row->note) {
      const int note_hunk = control_row->row->hunk_index;
      const CompareNoteActionRects note = BuildCompareNoteActionRects(text_renderer, controls, row);
      const std::string prefix = "compare/note-" +
                                 (note_hunk >= 0 ? std::to_string(note_hunk)
                                  : compare::IsCompareDetachedNoteHunk(note_hunk)
                                      ? "detached-" + std::to_string(compare::CompareDetachedNoteIndex(note_hunk))
                                      : std::string("file")) +
                                 "/";
      AddControlUiElement(out, kButton, prefix + "edit", note.edit, control_row->row->summary_text);
      AddControlUiElement(out, kButton, prefix + "delete", note.remove);
      continue;
    }
    const int hunk = control_row->controls_hunk;
    if (hunk < 0) {
      continue;
    }
    const CompareHunkReviewRects hunk_controls =
        BuildCompareHunkReviewRects(text_renderer, controls, compare_tab, hunk, row);
    const std::string prefix = "compare/hunk-" + std::to_string(hunk) + "/";
    if (hunk_controls.toggle.has_value()) {
      AddControlUiElement(out, kButton, prefix + "toggle-reviewed", *hunk_controls.toggle,
                          std::string(CompareHunkToggleLabel(CompareHunkReviewStatus(compare_tab, hunk))));
    }
    if (hunk_controls.comment.has_value()) {
      AddControlUiElement(out, kButton, prefix + "comment", *hunk_controls.comment);
    }
  }
  int block = 0;
  const float bottom = surface_rect.y + surface_rect.h;
  for (float y = surface.rows_y + surface.line_height / 2.0f; surface.line_height > 0.0f && y < bottom;
       y += surface.line_height) {
    const auto hit = CompareCollapsedContextRowAt(compare_tab, surface_rect, surface.rows_y, surface.line_height,
                                                  surface.show_vertical, y);
    if (!hit.has_value()) {
      continue;
    }
    const CollapsedContextActionRects rects = BuildCollapsedContextActionRects(
        text_renderer, hit->block_rect, hit->row->previous_hunk_index >= 0, hit->row->next_hunk_index >= 0);
    const std::string prefix = "compare/context-" + std::to_string(block++) + "/";
    const auto on_surface = [&](const SDL_FRect& rect) -> std::optional<SDL_FRect> {
      SDL_FRect visible{};
      if (!SDL_GetRectIntersectionFloat(&rect, &surface_rect, &visible)) {
        return std::nullopt;
      }
      return visible;
    };
    if (rects.previous_rect.has_value()) {
      if (const auto visible = on_surface(*rects.previous_rect)) {
        AddControlUiElement(out, kButton, prefix + "previous", *visible);
      }
    }
    if (const auto visible = on_surface(rects.all_rect)) {
      AddControlUiElement(out, kButton, prefix + "all", *visible, std::to_string(hit->row->collapsed_line_count));
    }
    if (rects.next_rect.has_value()) {
      if (const auto visible = on_surface(*rects.next_rect)) {
        AddControlUiElement(out, kButton, prefix + "next", *visible);
      }
    }
  }
}

}

void WorkspaceShell::ControlUiButtons(const WorkspaceLayout& layout, ControlUiElements& out) {
  using namespace control_ui_kind;
  if (const auto& panel = context_.current_project_state.panel;
      BottomPanelVisible() && panel.content == PanelContentKind::PluginSurface) {
    const auto* presentation = context_.current_project_state.plugin_presentation_if_present();
    const editor::SurfaceContent* content =
        presentation != nullptr ? presentation->surfaces.Find(panel.surface_owner, panel.surface_id) : nullptr;
    if (content != nullptr && content->has_body()) {
      const SDL_FRect body = BottomPanelContentRect(layout);
      for (const editor::SurfaceHitRegion& region : content->hit_regions) {
        SDL_FRect rect = PluginSurfacePreviewHitRegionRect(body, static_cast<float>(panel.surface_scroll_y), region);
        SDL_FRect shown{};
        if (region.rect.w > 0.0f && region.rect.h > 0.0f && SDL_GetRectIntersectionFloat(&rect, &body, &shown)) {
          AddControlUiElement(out, kButton, "surface/" + region.command, shown, region.command);
        }
      }
    }
  }
  if (const editor::PluginDecorationStore* store =
          RenderViewModelBuilder(context_).BuildHoverTargets().decoration_store;
      store != nullptr && ActiveTabIsEditor()) {
    const bool line_numbers = SettingFlagEnabled(GetSettingValue("editor.line_numbers"), true);
    std::vector<editor::EolDecorationSegment> segments;
    for (const EditorPaneLayout& pane : ComputeEditorPaneLayouts(layout)) {
      const editor::TextViewport* viewport = ViewportForPane(pane);
      if (viewport == nullptr || viewport->is_placeholder() || viewport->path().empty() || viewport->dirty()) {
        continue;
      }
      const editor::FileDecorations* decorations = store->FindByPathKey(viewport->path_key());
      if (decorations == nullptr) {
        continue;
      }
      const TextGridInteractionLayout grid =
          BuildEditorInteractionGrid(editor_text_renderer_, *viewport, pane.rect, line_numbers);
      for (std::size_t row = 0; row < grid.visible_rows; ++row) {
        const std::size_t line = grid.scroll_line + row;
        ForEachLineCodeLens(editor_text_renderer_, *viewport, grid, *decorations, line, segments,
                            [&](const editor::CodeLensDecoration& lens, const SDL_FRect& rect) {
                              AddControlUiElement(out, kButton, "lens/" + lens.text, rect, lens.text);
                            });
      }
    }
  }
  if (const EditorBannerState* banner = ActiveEditorBannerForTab(context_.current_project_state); banner != nullptr) {
    if (const SDL_FRect strip =
            ActiveEditorBannerStripRect(context_.current_project_state, ComputeEditorGroupRectsForState(layout));
        strip.h > 0.0f) {
      const EditorBannerButtonLayout buttons =
          ComputeEditorBannerButtonRects(strip, EditorBannerButtonsFor(banner->kind));
      for (std::size_t i = 0; i < buttons.buttons.size(); ++i) {
        const std::string label(buttons.buttons[i].label);
        AddControlUiElement(out, kButton, "banner/" + label, buttons.rects[i], label);
      }
      AddControlUiElement(out, kButton, "banner/dismiss", buttons.dismiss);
    }
  }
  if (MergeTabState* merge_tab = ActiveTabIsMerge() ? ActiveMergeTab() : nullptr;
      merge_tab != nullptr && ActiveTabSurface(layout).w > 0.0f && ActiveTabSurface(layout).h > 0.0f) {
    const SDL_FRect surface_rect = ActiveTabSurface(layout);
    const MergeSurfaceLayout surface = ComputeMergeSurfaceLayout(surface_rect, *merge_tab);
    const MergeToolbarLayout toolbar = ComputeMergeToolbarLayout(surface_rect, surface);
    for (const auto& [name, rect, label] :
         {std::tuple{"merge/prev", toolbar.prev_rect, "Previous"}, std::tuple{"merge/next", toolbar.next_rect, "Next"},
          std::tuple{"merge/open", toolbar.open_rect, "Open Result"},
          std::tuple{"merge/save", toolbar.save_rect, "Save"}}) {
      if (UiRectHasArea(rect)) {
        AddControlUiElement(out, kButton, name, rect, label);
      }
    }
    for (const std::string_view label : {"Unresolved", "Toggle Base", "Mark Resolved"}) {
      if (const auto rect = ComputeMergeSecondaryToolbarButtonRect(surface_rect, surface, label);
          rect.has_value() && UiRectHasArea(*rect)) {
        AddControlUiElement(out, kButton, "merge/" + std::string(label), *rect, std::string(label));
      }
    }
    const auto dividers = MergeDividerHitRects(surface_rect, surface);
    for (std::size_t i = 0; i < dividers.size(); ++i) {
      AddControlUiElement(out, kDivider, "merge/" + std::to_string(i), dividers[i]);
    }
    const auto merge_scroll = ComputeMergeScrollLayout(surface_rect, surface, *merge_tab);
    if (merge_scroll.vertical_scrollbar.has_value()) {
      AddControlUiScrollbar(out, "merge", *merge_scroll.vertical_scrollbar);
    }
    if (merge_scroll.horizontal_scrollbar.has_value()) {
      AddControlUiScrollbar(out, "merge/h", *merge_scroll.horizontal_scrollbar);
    }
    const MergeInteractionLayout interaction = BuildMergeInteractionLayout(surface_rect, surface, *merge_tab);
    const auto visible = [&](const SDL_FRect& rect) {
      return UiRectHasArea(rect) && rect.y >= surface.rows_y && rect.y + rect.h <= surface_rect.y + surface_rect.h;
    };
    constexpr std::array<std::string_view, 4> kResultActions = {"base", "incoming", "current", "both"};
    constexpr std::array<compare::MergeChoice, 4> kResultChoices = {
        compare::MergeChoice::Base, compare::MergeChoice::Incoming, compare::MergeChoice::Current,
        compare::MergeChoice::Both};
    const auto resolves_to = [&](const SDL_FRect& rect, std::size_t conflict_index, MergeHoverState::Kind kind,
                                 compare::MergeChoice choice) {
      const auto hover =
          ClassifyMergeHoverState(surface, interaction, *merge_tab, rect.x + rect.w * 0.5f, rect.y + rect.h * 0.5f);
      return hover.has_value() && hover->conflict_index == conflict_index && hover->kind == kind &&
             hover->preview_choice == choice;
    };
    for (std::size_t i = 0; i < merge_tab->conflicts.size(); ++i) {
      const MergeTrackedConflict& conflict = merge_tab->conflicts[i];
      if (!conflict.valid) {
        continue;
      }
      const std::string prefix = "merge/" + std::to_string(i) + "/";
      for (const bool incoming : {true, false}) {
        const SDL_FRect rect = BuildMergeSourceActionButtonRect(surface, interaction, conflict, incoming);
        if (visible(rect) &&
            resolves_to(rect, i,
                        incoming ? MergeHoverState::Kind::IncomingAccept : MergeHoverState::Kind::CurrentAccept,
                        incoming ? compare::MergeChoice::Incoming : compare::MergeChoice::Current)) {
          AddControlUiElement(out, kButton, prefix + (incoming ? "accept-incoming" : "accept-current"), rect);
        }
      }
      const std::array<SDL_FRect, 4> actions = BuildMergeResultActionButtonRects(surface, interaction, conflict);
      for (std::size_t a = 0; a < actions.size(); ++a) {
        if (visible(actions[a]) && resolves_to(actions[a], i, MergeHoverState::Kind::ResultAction, kResultChoices[a])) {
          AddControlUiElement(out, kButton, prefix + "result-" + std::string(kResultActions[a]), actions[a]);
        }
      }
    }
  }
  if (CompareTabState* compare_tab = ActiveTabIsCompare() ? ActiveCompareTab() : nullptr; compare_tab != nullptr) {
    const SDL_FRect surface_rect = ActiveTabSurface(layout);
    AppendCompareControls(out, editor_text_renderer_, *compare_tab, surface_rect,
                          ComputeCompareSurfaceLayout(surface_rect, *compare_tab));
  }
  const NotificationsViewModel toasts =
      RenderViewModelBuilder(context_).BuildNotifications(notification_service_, layout.status_bar, text_renderer_);
  for (const NotificationEntryViewModel& entry : toasts.entries) {
    AddControlUiElement(out, kNotification, std::to_string(entry.index), entry.layout.rect, std::string(entry.message));
    out.back().active = entry.focused;
    for (const NotificationButtonViewModel& button : entry.buttons) {
      const std::string label(button.label);
      AddControlUiElement(out, kButton, "notification/" + label, button.rect, label);
    }
  }
}

void WorkspaceShell::ControlUiOverlay(const WorkspaceLayout& layout, ControlUiElements& out) {
  using namespace control_ui_kind;
  if (!context_.current_project_state.overlay.visible) {
    return;
  }
  OverlaySurfaceViewModel vm;
  RenderViewModelBuilder(context_).BuildOverlaySurfaceInto(vm, layout, ComputeOverlayRect(layout.editor_area),
                                                           text_renderer_);
  if (vm.mode == OverlayMode::BufferSearch || vm.mode == OverlayMode::BufferReplace) {
    const FindWidgetLayout& fw = vm.find_widget.fw;
    const auto& search = context_.current_project_state.overlay.workflow.buffer_search;
    AddControlUiElement(out, kField, "find/search", fw.search_field, search.query.text());
    out.back().active = vm.find_widget.search_focused;
    if (fw.replace_mode) {
      AddControlUiElement(out, kField, "find/replace", fw.replace_field, search.replace_text.text());
      out.back().active = vm.find_widget.replace_focused;
      AddControlUiElement(out, kButton, "find/replace", fw.replace_button);
      AddControlUiElement(out, kButton, "find/replace-all", fw.replace_all_button);
    }
    static constexpr std::array<std::string_view, kBufferFindToggleCount> kToggles = {"match-case", "whole-word",
                                                                                      "regex", "in-selection"};
    for (std::size_t i = 0; i < fw.toggle_count && i < kToggles.size(); ++i) {
      AddControlUiElement(out, kButton, std::string("find/") + std::string(kToggles[i]), fw.toggle_buttons[i]);
      out.back().active = vm.find_widget.toggles[i].active;
    }
    AddControlUiElement(out, kButton, "find/previous", fw.prev_button);
    AddControlUiElement(out, kButton, "find/next", fw.next_button);
    AddControlUiElement(out, kButton, "find/close", fw.close_button);
    return;
  }
  for (std::size_t i = 0; i < vm.rows.size(); ++i) {
    const std::size_t index = static_cast<std::size_t>(vm.list_layout.scroll_row) + i;
    const std::string label(vm.rows[i].primary);
    AddControlUiElement(out, kOverlayRow, label.empty() ? "#" + std::to_string(index) : label,
                        ScrollableListRowRect(vm.list_layout, static_cast<int>(i)), label);
    out.back().selected = static_cast<int>(index) == vm.selected_row;
  }
  if (const auto bar = MakeVerticalScrollbarGeometry(vm.list_layout.list_rect, static_cast<float>(vm.total_rows),
                                                     vm.list_layout.visible_units, static_cast<float>(vm.scroll_row));
      bar.has_value()) {
    AddControlUiScrollbar(out, "overlay", *bar);
  }
}

void WorkspaceShell::ControlUiDialogs(const WorkspaceLayout& layout, ControlUiElements& out) {
  using namespace control_ui_kind;
  if (context_.prompts.dirty_visible) {
    const SDL_FRect dialog = ComputeDirtyPromptRect(layout.full);
    const auto buttons = ComputeDirtyPromptButtonRects(dialog);
    const auto labels = DirtyPromptActionLabels();
    for (std::size_t i = 0; i < buttons.size() && i < labels.size(); ++i) {
      const std::string label(labels[i]);
      AddControlUiElement(out, kButton, "prompt/" + label, buttons[i], label);
      out.back().active = context_.prompts.dirty.selected_action == static_cast<int>(i);
    }
    return;
  }
  if (context_.prompts.surface_visible) {
    const PromptSurfaceState& prompt = context_.prompts.surface;
    const SDL_FRect dialog = ComputePromptSurfaceRect(layout.full);
    if (prompt.kind == PromptSurfaceState::Kind::TextInput) {
      AddControlUiElement(out, kField, "prompt", ComputePromptSurfaceInputRect(dialog), prompt.input.text());
    }
    const auto buttons = ComputePromptSurfaceButtonRects(dialog, prompt.button_count);
    const auto labels = PromptSurfaceActionLabels();
    for (std::size_t i = 0; i < buttons.size() && i < labels.size(); ++i) {
      const std::string label(labels[i]);
      AddControlUiElement(out, kButton, "prompt/" + label, buttons[i], label);
      out.back().active = prompt.selected_button == static_cast<int>(i);
    }
    return;
  }
  if (!settings_overlay_service_.Visible() || settings_overlay_service_.Mode() != SettingsOverlayMode::Settings) {
    return;
  }
  const SettingsOverlayViewModel vm =
      RenderViewModelBuilder(context_).BuildSettingsOverlay(layout, settings_overlay_service_, text_renderer_);
  AddControlUiElement(out, kField, "settings/filter", vm.filter_rect, std::string(vm.query));
  for (const SettingsCategoryViewModel& category : vm.categories) {
    if (!SettingsRowShownIn(vm.left_pane_rect, category.rect)) {
      continue;
    }
    const std::string label(category.label);
    AddControlUiElement(out, kSettingsCategory, label, category.rect, label);
    out.back().selected = category.selected;
  }
  const auto has_area = [](const SDL_FRect& rect) { return rect.w > 0.0f && rect.h > 0.0f; };
  for (const SettingsRowViewModel& row : vm.rows) {
    if (!SettingsRowShownIn(vm.right_pane_rect, row.row_rect)) {
      continue;
    }
    const std::string id(row.id);
    std::string value = row.control.kind == SettingsControlKind::Checkbox
                            ? std::string(row.control.checkbox_on ? "on" : "off")
                            : std::string(row.control.display_value);
    AddControlUiElement(out, kSetting, id, SettingsRowVisibleRect(vm.right_pane_rect, row.row_rect), std::move(value));
    out.back().selected = row.selected;
    const auto add_control = [&](std::string_view part, const SDL_FRect& rect) {
      if (has_area(rect) && SettingsRowShownIn(vm.right_pane_rect, rect)) {
        AddControlUiElement(out, kButton, "setting/" + id + "/" + std::string(part), rect);
      }
    };
    add_control("checkbox", row.control.checkbox_rect);
    add_control("decrement", row.control.dec_rect);
    add_control("increment", row.control.inc_rect);
    add_control("value", row.control.value_rect);
    if (row.resettable) {
      add_control("reset", row.reset_rect);
    }
    add_control("scope", row.scope_rect);
  }
  if (vm.scrollbar.has_value()) {
    AddControlUiScrollbar(out, "settings", *vm.scrollbar);
  }
  if (vm.category_scrollbar.has_value()) {
    AddControlUiScrollbar(out, "settings/categories", *vm.category_scrollbar);
  }
  if (vm.value_picker.visible) {
    for (const SettingsPickerItemViewModel& item : vm.value_picker.items) {
      if (!has_area(item.rect)) {
        continue;
      }
      AddControlUiElement(out, kOverlayRow, std::string(item.text), item.rect, std::string(item.text));
      out.back().selected = item.highlighted;
    }
  }
}

void WorkspaceShell::ControlUiFields(const WorkspaceLayout& layout, ControlUiElements& out) {
  using namespace control_ui_kind;
  if (!context_.current_project_state.sidebar.visible || !UiRectHasArea(layout.sidebar)) {
    return;
  }
  const SidebarMode mode = ActiveSidebarMode();
  if (mode == SidebarMode::Search) {
    const auto& search = context_.current_project_state.overlay.workflow.project_search;
    static constexpr std::array<std::string_view, 4> kNames = {"query", "replace", "include", "exclude"};
    const std::array<const editor::SingleLineEditor*, 4> committed = {&search.query, &search.replace_text,
                                                                      &search.include_globs, &search.exclude_globs};
    const auto slots = project_search_panel::SidebarSearchFieldRects(layout.sidebar, search.scope_expanded);
    for (std::size_t i = 0; i < slots.size(); ++i) {
      if (slots[i].rect.w <= 0.0f) {
        continue;
      }
      const bool editing = search.editing && search.edit_field == slots[i].field;
      AddControlUiElement(out, kField, "search/" + std::string(kNames[i]), slots[i].rect,
                          editing ? search.edit_buffer.text() : committed[i]->text());
      out.back().active = editing;
    }
    AddControlUiElement(out, kButton, "search/mode", project_search_panel::ModeButtonRect(layout.sidebar));
    AddControlUiElement(out, kButton, "search/case", project_search_panel::CaseButtonRect(layout.sidebar));
    AddControlUiElement(out, kButton, "search/hidden", project_search_panel::HiddenButtonRect(layout.sidebar));
    AddControlUiElement(out, kButton, "search/scope", project_search_panel::ScopeButtonRect(layout.sidebar));
    AddControlUiElement(out, kButton, "search/replace-all", project_search_panel::ReplaceAllButtonRect(layout.sidebar));
    return;
  }
  if (mode != SidebarMode::Git) {
    return;
  }
  const SDL_FRect sidebar = layout.sidebar;
  const GitSidebarViewModel& git_vm =
      CachedGitSidebarPresentation(context_.current_project_state.sidebar.git, context_.current_project_state.root,
                                   context_.current_project_state.branch_review)
          .view_model;
  AddControlUiElement(out, kButton, "git/stage-all", git_sidebar_header::StageAllButtonRect(sidebar), "Stage All");
  out.back().enabled = CanStageAllGitSidebarEntries();
  AddControlUiElement(out, kButton, "git/discard-all", git_sidebar_header::DiscardAllButtonRect(sidebar),
                      "Discard All");
  out.back().enabled = CanDiscardAllGitSidebarEntries();
  AddControlUiElement(out, kButton, "git/refresh", git_sidebar_header::RefreshButtonRect(sidebar), "Refresh");
  AddControlUiElement(out, kButton, "git/branch", git_sidebar_header::BranchButtonRect(sidebar),
                      git_vm.branch_button_label);
  AddControlUiElement(out, kButton, "git/sync", git_sidebar_header::SyncButtonRect(sidebar), git_vm.sync_button_label);
  if (const auto outgoing_base = GitSidebarOutgoingBaseButtonRect(sidebar); outgoing_base.has_value()) {
    AddControlUiElement(out, kButton, "git/outgoing-base", *outgoing_base);
  }
  const auto& workflow = context_.current_project_state.sidebar.git.commit_workflow;
  if (!workflow.open) {
    AddControlUiElement(out, kButton, "git/commit", git_sidebar_header::CommitButtonRect(sidebar), "Commit");
    out.back().enabled = git_vm.commit_ready;
    return;
  }
  AddControlUiElement(out, kField, "git/subject", workflow.subject_field_rect, workflow.subject.text());
  out.back().active = workflow.focus_field == CommitWorkflowFocusField::Subject;
  AddControlUiElement(out, kField, "git/body", workflow.body_field_rect, workflow.BodyText());
  out.back().active = workflow.focus_field == CommitWorkflowFocusField::Body;
  AddControlUiElement(out, kButton, "git/confirm-commit", workflow.commit_button_rect);
}

void WorkspaceShell::ControlUiDebug(const WorkspaceLayout& layout, ControlUiElements& out) {
  using namespace control_ui_kind;
  const ProjectWorkspaceState& ps = context_.current_project_state;
  if (DebugToolbarVisible()) {
    const DebugToolbarLayout tb =
        ComputeDebugToolbarLayout(ActiveTabSurface(layout), DebugToolbarAvoidBelowY(layout), debug_service_.SupportsStepBack());
    const bool stopped = IsDebugSessionStopped();
    for (std::size_t i = 0; i < tb.button_count; ++i) {
      const DebugToolbarButton button = tb.kinds[i];
      AddControlUiElement(out, kButton, "debug-toolbar/" + std::string(DebugToolbarButtonId(button)), tb.buttons[i],
                          std::string(DebugToolbarButtonTooltip(button, stopped)));
      const bool step = button != DebugToolbarButton::ContinuePause && button != DebugToolbarButton::Restart &&
                        button != DebugToolbarButton::Stop;
      out.back().enabled = !step || stopped;
    }
  }
  if (!ps.debug_pane.visible || !UiRectHasArea(layout.right_pane)) {
    return;
  }
  const DebugPaneModeRowLayout modes = DebugPaneModeRow(layout.right_pane);
  for (int i = 0; i < modes.tab_count; ++i) {
    const DebugPaneModeTab& tab = modes.tabs[static_cast<std::size_t>(i)];
    AddControlUiElement(out, kDebugTab, tab.id, tab.rect, std::string(tab.label));
    out.back().active = tab.mode == ps.debug_pane.mode;
  }
  const DebugPaneSurfaceSpec* spec = FindDebugPaneSurface(ps.debug_pane.mode);
  const std::string mode_id = spec != nullptr ? std::string(spec->id) : "debug";
  const std::size_t count = DebugPaneActiveRowCount();
  const LogSurfaceLayout list = ComputeDebugPaneListLayout(layout, count);
  if (list.scroll.vertical_scrollbar.has_value()) {
    AddControlUiScrollbar(out, "debug", *list.scroll.vertical_scrollbar);
  }
  for (int r = 0; r < list.scroll.visible_rows; ++r) {
    const std::size_t index = static_cast<std::size_t>(list.scroll.vertical_scroll + r);
    if (index >= count) {
      break;
    }
    std::string label;
    bool selected = false;
    switch (ps.debug_pane.mode) {
      case DebugPaneMode::CallStack: {
        const DebugExecutionView& exec = ps.debug_execution;
        const auto ref = exec.PanelRowAt(index);
        using Kind = DebugExecutionView::PanelRowRef::Kind;
        if (ref.kind == Kind::Session && ref.index < exec.sessions.size()) {
          label = exec.sessions[ref.index].display;
          selected = exec.sessions[ref.index].id == exec.focused_session_id;
        } else if (ref.kind == Kind::Thread && ref.index < exec.threads.size()) {
          label = exec.threads[ref.index].display;
          selected = exec.threads[ref.index].id == exec.focused_thread_id;
        } else if (ref.kind == Kind::Frame && ref.index < exec.frames.size()) {
          const DebugStackFrameView& frame = exec.frames[ref.index];
          label = frame.display_primary;
          if (!frame.display_secondary.empty()) {
            label += "  ";
            label += frame.display_secondary;
          }
          selected = ref.index == exec.focused_frame_index;
        }
        break;
      }
      case DebugPaneMode::Variables:
        if (index < ps.debug_variables.Rows().size()) {
          label = DebugValueRowLabel(ps.debug_variables.Rows()[index]);
          selected = ps.debug_variables.SelectedRow() == index;
        }
        break;
      case DebugPaneMode::Watch:
        if (index < ps.debug_watch.Rows().size()) {
          label = DebugValueRowLabel(ps.debug_watch.Rows()[index]);
          selected = ps.debug_watch.SelectedRow() == index;
        }
        break;
      case DebugPaneMode::Breakpoints:
        if (index < ps.debug_breakpoints_panel.Rows().size()) {
          const DebugBreakpointRowView& row = ps.debug_breakpoints_panel.Rows()[index];
          label = row.display;
          if (!row.secondary.empty()) {
            label += "  ";
            label += row.secondary;
          }
          selected = ps.debug_pane.breakpoints_selected_row == static_cast<int>(index);
        }
        break;
    }
    const float y = DebugPaneRowBandTop(list.text_y, list.line_height, r);
    AddControlUiElement(out, kDebugRow, mode_id + "/" + std::to_string(index),
                        MakeRect(list.content_rect.x, y, list.content_rect.w, list.line_height), std::move(label));
    out.back().selected = selected;
  }
}

void WorkspaceShell::ControlUiDetails(const WorkspaceLayout& layout, util::JsonObject& root) {
  const ProjectWorkspaceState& state = context_.current_project_state;
  root["windowTitle"] = util::JsonValue(frame_state_.window_title);
  util::JsonObject git;
  git["settled"] =
      util::JsonValue(!state.sidebar.git.refreshing && !automatic_git_refresh_pacer_.pending() &&
                      !git_repository_service_.IsRefreshing() && !git_repository_service_.CurrentSummary().stale);
  root["git"] = util::JsonValue(std::move(git));
  if (state.overlay.visible) {
    util::JsonObject& overlay = std::get<util::JsonObject>(root["overlay"].v);
    overlay["count"] = util::JsonValue(static_cast<std::int64_t>(OverlayItemCount()));
    if (state.overlay.mode == OverlayMode::QuickPick) {
      const QuickPickKind kind = state.overlay.workflow.quick_pick.kind;
      overlay["kind"] = util::JsonValue(std::string(QuickPickKindName(kind)));
    }
    overlay["selected"] = util::JsonValue(static_cast<std::int64_t>(OverlaySelectedIndex()));
    if (state.overlay.mode == OverlayMode::QuickPick) {
      overlay["title"] = util::JsonValue(state.overlay.workflow.quick_pick.title);
    } else if (state.overlay.mode == OverlayMode::FileFinder &&
               !state.overlay.workflow.file_finder_compare_title.empty()) {
      overlay["title"] = util::JsonValue(state.overlay.workflow.file_finder_compare_title);
    }
    if (state.overlay.mode == OverlayMode::FileFinder) {
      util::JsonObject index;
      index["files"] = util::JsonValue(static_cast<std::int64_t>(state.file_finder.indexed_file_count()));
      index["version"] = util::JsonValue(static_cast<std::int64_t>(state.file_index.version()));
      index["truncated"] = util::JsonValue(state.file_index.truncated());
      index["root"] = util::JsonValue(state.file_index.root().string());
      index["scansSuperseded"] = util::JsonValue(
          static_cast<std::int64_t>(util::ReadPerformanceCounter(util::PerfCounterId::FileIndexScanSupersededByBatch)));
      overlay["index"] = util::JsonValue(std::move(index));
    }
    if (const editor::SingleLineEditor* query = OverlayQueryEditor(state); query != nullptr) {
      overlay["query"] = util::JsonValue(query->text());
    }
    if (const auto& scope = state.overlay.workflow.buffer_search.scope;
        scope.has_value() &&
        (state.overlay.mode == OverlayMode::BufferSearch || state.overlay.mode == OverlayMode::BufferReplace)) {
      const auto position = [](const editor::TextPosition& at) {
        util::JsonArray pair;
        pair.push_back(util::JsonValue(static_cast<std::int64_t>(at.line + 1)));
        pair.push_back(util::JsonValue(static_cast<std::int64_t>(at.column + 1)));
        return util::JsonValue(std::move(pair));
      };
      util::JsonObject range;
      range["start"] = position(scope->range.start);
      range["end"] = position(scope->range.end);
      overlay["scope"] = util::JsonValue(std::move(range));
    }
  }
  if (context_.prompts.surface_visible || context_.prompts.dirty_visible) {
    util::JsonObject prompt;
    const bool dirty = context_.prompts.dirty_visible;
    prompt["kind"] = util::JsonValue(std::string(dirty ? "unsaved"
                                                 : context_.prompts.surface.kind == PromptSurfaceState::Kind::TextInput
                                                     ? "input"
                                                     : "confirm"));
    prompt["title"] = util::JsonValue(std::string(dirty ? DirtyPromptTitle() : PromptSurfaceTitle()));
    prompt["message"] = util::JsonValue(dirty ? DirtyPromptMessage() : PromptSurfaceMessage());
    if (!dirty) {
      prompt["detail"] = util::JsonValue(std::string(PromptSurfaceDetail()));
      prompt["input"] = util::JsonValue(context_.prompts.surface.input.text());
    }
    root["prompt"] = util::JsonValue(std::move(prompt));
  }
  util::JsonObject popups;
  popups["hover"] = util::JsonValue(nullptr);
  if (const auto hover = ActiveEditorHoverPopupLayout(); hover.has_value()) {
    util::JsonObject entry;
    std::string text;
    std::string kind;
    switch (hover->kind) {
      case EditorHoverTarget::Kind::Blame:
        kind = "blame";
        if (const editor::EditorBlameLine* blame = editor_blame_overlay_service_.VisibleLine(hover->blame_line_index);
            blame != nullptr) {
          text = blame->author + "\n" + blame->date + "\n" + blame->summary;
        }
        break;
      case EditorHoverTarget::Kind::Diagnostic:
        kind = "diagnostic";
        text = hover->diagnostic.has_value() ? hover->diagnostic->message : std::string{};
        if (hover->plugin_hover.has_value()) {
          for (const std::string* block : {&hover->plugin_hover->title, &hover->plugin_hover->content}) {
            if (!block->empty()) {
              text += "\n" + *block;
            }
          }
        }
        break;
      case EditorHoverTarget::Kind::Plugin:
        kind = "symbol";
        if (hover->plugin_hover.has_value()) {
          text = hover->plugin_hover->title.empty() ? hover->plugin_hover->content
                                                    : hover->plugin_hover->title + "\n" + hover->plugin_hover->content;
        }
        break;
      case EditorHoverTarget::Kind::DebugValue:
        kind = "debug-value";
        text = hover->debug_value.has_value() ? hover->debug_value->value : std::string{};
        break;
    }
    entry["kind"] = util::JsonValue(std::move(kind));
    entry["text"] = util::JsonValue(std::move(text));
    entry["rect"] = TruncatedRectJson(hover->rect);
    if (hover->primary_action_rect.has_value()) {
      util::JsonObject action;
      action["label"] =
          util::JsonValue(std::string(hover->kind == EditorHoverTarget::Kind::Diagnostic ? "Quick Fix" : "Copy SHA"));
      action["rect"] = TruncatedRectJson(*hover->primary_action_rect);
      entry["action"] = util::JsonValue(std::move(action));
    }
    popups["hover"] = util::JsonValue(std::move(entry));
  }
  popups["signatureHelp"] = active_signature_help_.has_value() ? util::JsonValue(active_signature_help_->signature)
                                                               : util::JsonValue(nullptr);
  popups["signatureHelpDetail"] = active_signature_help_.has_value()
                                      ? util::JsonValue(active_signature_help_->documentation)
                                      : util::JsonValue(nullptr);
  root["popups"] = util::JsonValue(std::move(popups));

  {
    const WorkspaceActionContext::ActionReceipt& receipt = MakeActionContext().LastActionReceipt();
    util::JsonObject actions;
    actions["dispatched"] = util::JsonValue(static_cast<std::int64_t>(receipt.dispatched));
    actions["last"] =
        receipt.plugin_command.empty() ? util::JsonValue(nullptr) : util::JsonValue(receipt.plugin_command);
    if (receipt.last.has_value()) {
      for (const ActionSpec& spec : WorkspaceCommandSpecs()) {
        if (spec.id == *receipt.last) {
          actions["last"] = util::JsonValue(std::string(spec.command_name.empty() ? spec.label : spec.command_name));
          break;
        }
      }
    }
    actions["source"] = util::JsonValue(std::string(ActionSourceName(receipt.source)));
    actions["rejected"] = util::JsonValue(receipt.rejected);
    root["actions"] = util::JsonValue(std::move(actions));
  }

  util::JsonArray editors;
  const bool line_numbers = SettingFlagEnabled(GetSettingValue("editor.line_numbers"), true);
  for (const EditorPaneLayout& pane : ComputeEditorPaneLayouts(layout)) {
    const editor::TextViewport* viewport = ViewportForPane(pane);
    if (viewport == nullptr || viewport->is_placeholder()) {
      continue;
    }
    const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
        editor_text_renderer_, *viewport, pane.rect, viewport->sticky_band_rows(), line_numbers);
    const auto num = FloatJson;
    util::JsonObject entry;
    entry["group"] = util::JsonValue(static_cast<std::int64_t>(pane.group_index));
    entry["focused"] = util::JsonValue(pane.active);
    entry["rect"] = TruncatedRectJson(pane.rect);
    entry["textX"] = num(metrics.text_x);
    entry["firstLineY"] = num(metrics.first_line_y);
    entry["lineHeight"] = num(metrics.line_height);
    entry["charWidth"] = num(editor_text_renderer_.CharWidth());
    entry["gutterX"] = num(pane.rect.x);
    entry["gutterWidth"] = num(metrics.gutter_width);
    entry["foldHitX"] = num(editor::kGutterFoldHitLeft(pane.rect.x, metrics.gutter_width));
    entry["stickyRows"] = util::JsonValue(static_cast<std::int64_t>(metrics.sticky_scroll_rows));
    entry["scrollColumn"] = util::JsonValue(static_cast<std::int64_t>(viewport->horizontal_scroll()));
    util::JsonArray rows;
    const std::size_t visual_count = viewport->visual_line_count();
    std::vector<float> row_tops;
    VisibleRowTops(
        context_.current_project_state, *viewport, metrics, [this](std::string_view id) { return GetSettingValue(id); },
        row_tops);
    for (std::size_t i = 0; i < metrics.visible_rows && i < row_tops.size(); ++i) {
      const std::size_t visual = viewport->scroll_line() + i;
      if (visual >= visual_count) {
        break;
      }
      util::JsonObject row;
      row["line"] = util::JsonValue(static_cast<std::int64_t>(viewport->VisualRowLineIndex(visual) + 1));
      row["y"] = num(row_tops[i]);
      rows.push_back(util::JsonValue(std::move(row)));
    }
    entry["rows"] = util::JsonValue(std::move(rows));
    editors.push_back(util::JsonValue(std::move(entry)));
  }
  root["editors"] = util::JsonValue(std::move(editors));

  if (const TerminalPaneState* pane = ActiveTerminalPane(); pane != nullptr && BottomPanelVisible()) {
    const auto panel_layout = ComputeBottomPanelLogLayout(layout, pane->session.LineCount());
    util::JsonObject terminal;
    terminal["rect"] = TruncatedRectJson(panel_layout.content_rect);
    terminal["charWidth"] = FloatJson(std::max(1.0f, terminal_text_renderer_.CharWidth()));
    root["terminal"] = LogSurfaceRowsJson(terminal, panel_layout);
  }
  if (BottomPanelVisible() && context_.current_project_state.panel.content == PanelContentKind::Output) {
    const auto* entries = OutputChannelEntries(context_.current_project_state.panel.output.channel_id);
    const std::size_t count = entries != nullptr ? entries->size() : 0;
    const auto panel_layout = ComputeBottomPanelLogLayout(layout, count);
    util::JsonObject output;
    output["channel"] = util::JsonValue(context_.current_project_state.panel.output.channel_id);
    root["output"] = LogSurfaceRowsJson(output, panel_layout);
  }
}

}
