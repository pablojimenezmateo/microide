// WorkspaceShell::MakeControlUiOperations: the shell's side of the `ui` control
// query (control/ControlUiQuery.h). It lives with the control channel rather than
// in a WorkspaceShell*.cpp companion because what it does is answer the query --
// every element source asks the geometry the matching mouse path hit-tests and
// reports it by name -- and it grows with every surface a QA driver must reach.
#include "workspace/shell/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <variant>

#include "workspace/ProjectSearchPanelLayout.h"
#include "workspace/WorkspaceProjectSearchPresentation.h"
#include "workspace/debug/DebugPaneRegistry.h"
#include "workspace/git/GitSidebarHeaderLayout.h"
#include "workspace/git/WorkspaceGitSidebarPresentation.h"
#include "workspace/WorkspaceUiText.h"
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

// The text field the open overlay types into, per mode (the editor the overlay
// view model draws its query row from); null for the caret-anchored lists.
const editor::SingleLineEditor* OverlayQueryEditor(const ProjectWorkspaceState& state) {
  const auto& workflow = state.overlay.workflow;
  switch (state.overlay.mode) {
    case OverlayMode::FileFinder: return &state.file_finder.query_state();
    case OverlayMode::ProjectSearch: return &workflow.project_search.query;
    case OverlayMode::CommitPicker: return &workflow.compare_picker.query;
    case OverlayMode::LaunchConfigPicker: return &workflow.launch_config_picker.query;
    case OverlayMode::CommandPalette: return &workflow.command_palette.query;
    case OverlayMode::BufferSearch:
    case OverlayMode::BufferReplace: return &workflow.buffer_search.query;
    case OverlayMode::Completion:
    case OverlayMode::CodeActions: break;
  }
  return nullptr;
}

// The debug toolbar button's locator (`button:debug-toolbar/<id>`).
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

// A value-tree row as the pane draws it: name, then value, then type.
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

}  // namespace

// The `ui` control query's view of the window (control/ControlUiQuery.h). Every
// element source asks the geometry the matching mouse path hit-tests -- the menu
// bar and popup rows, the tab strips, the sidebar header and list layouts, the
// bottom-panel header, the status-bar segment walk, the banner and toast buttons --
// so a reported rect is where a click lands. Read-only: nothing here requests a
// redraw or changes state.
ControlUiQuery::Operations WorkspaceShell::MakeControlUiOperations() {
  using namespace control_ui_kind;
  return ControlUiQuery::Operations{
      .layout = [this]() { return CurrentWorkspaceLayout(); },
      .window_per_unit = [this]() { return window_presentation_.window_per_unit; },
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
              locator += MenuItemLabel(items[index]);
            }
            return locator;
          },
      .modal =
          [this]() -> std::string {
            if (context_.prompts.dirty_visible) {
              return "unsaved-prompt";
            }
            if (context_.prompts.surface_visible) {
              return "prompt";
            }
            if (settings_overlay_service_.Visible()) {
              return settings_overlay_service_.Mode() == SettingsOverlayMode::HelpAbout ? "help"
                                                                                        : "settings";
            }
            return {};
          },
      .menus =
          [this](const WorkspaceLayout& layout, ControlUiElements& out) {
            const MenuSurfaceState& menus = context_.menu_state;
            const auto menu_name = [](MenuId id) {
              const MenuSpec* spec = FindMenuSpec(id);
              return spec == nullptr || spec->label.empty() ? std::string("menu")
                                                            : std::string(spec->label);
            };
            for (const auto& item : ComputeVisibleMenuBarItems(layout.menu_bar)) {
              const std::string name = menu_name(item.id);
              AddControlUiElement(out, kMenu, name, item.rect, name);
              out.back().active = menus.menu_bar_open && menus.active_menu_id == item.id;
            }
            // The chevron that stands in for the menus the bar could not fit (all of
            // them in compact mode): a driver walking "every menu" in a layout with
            // no `menu:` elements must find the door to them here, or it sweeps
            // nothing and reports green.
            if (const auto chevron = MenuOverflowChevronRect(layout.menu_bar); chevron.has_value()) {
              AddControlUiElement(out, kButton, "menu-bar/more", *chevron);
              out.back().active = menus.overflow_popup_open;
            }
            for (const auto& button : ComputeVisibleWindowControlButtons(layout.menu_bar)) {
              const std::string_view name =
                  button.id == WindowControlButtonId::Minimize   ? "window/minimize"
                  : button.id == WindowControlButtonId::Maximize ? "window/maximize"
                                                                 : "window/close";
              AddControlUiElement(out, kButton, name, button.rect);
            }
            const auto add_rows = [&](std::span<const MenuItemSpec> items, int active_index,
                                      const SDL_FRect& popup, const std::string& owner) {
              for (const VisiblePopupMenuItem& row :
                   ComputeVisiblePopupMenuItems(items, active_index, popup)) {
                if (row.separator) {
                  continue;
                }
                const std::string label(MenuItemLabel(items[row.index]));
                AddControlUiElement(out, kMenuItem, owner + "/" + label, row.rect, label);
                out.back().enabled = row.enabled;
                out.back().active = row.hovered;
                if (items[row.index].submenu == MenuId::None) {
                  for (const ActionSpec& spec : WorkspaceCommandSpecs()) {
                    if (spec.id == items[row.index].action) {
                      out.back().command =
                          std::string(spec.command_name.empty() ? spec.label : spec.command_name);
                      break;
                    }
                  }
                }
              }
            };
            if (menus.menu_bar_open) {
              if (const auto popup = ComputePopupMenuRect(layout.menu_bar, menus.active_menu_id);
                  popup.has_value()) {
                add_rows(MenuItems(menus.active_menu_id), menus.active_menu_item_index, *popup,
                         menu_name(menus.active_menu_id));
              }
              if (menus.active_submenu_id != MenuId::None) {
                if (const auto submenu = ActiveSubmenuRect(layout.menu_bar); submenu.has_value()) {
                  add_rows(MenuItems(menus.active_submenu_id), menus.active_submenu_item_index,
                           *submenu, menu_name(menus.active_submenu_id));
                }
              }
            }
            if (menus.tree_context_menu.open) {
              if (const auto popup = ComputeTreeContextMenuRect(); popup.has_value()) {
                add_rows(WorkspaceTreeContextMenuItems(menus.tree_context_menu.target),
                         menus.tree_context_menu.active_item_index, *popup, "context");
              }
            }
            if (menus.overflow_popup_open && menus.overflow_popup_anchor_rect.has_value()) {
              const auto overflow = ComputeOverflowMenuBarItems(layout.menu_bar);
              const SDL_FRect popup =
                  ComputeMenuOverflowPopupRect(*menus.overflow_popup_anchor_rect, overflow.size());
              for (std::size_t i = 0; i < overflow.size(); ++i) {
                const std::string label = menu_name(overflow[i]);
                AddControlUiElement(out, kMenuItem, "more/" + label,
                                    MenuOverflowPopupRowRect(popup, i), label);
                out.back().active = menus.overflow_popup_active_index == static_cast<int>(i);
              }
            }
          },
      .tabs =
          [this](const WorkspaceLayout& layout, ControlUiElements& out) {
            if (UiRectHasArea(layout.project_tab_strip)) {
              for (const VisibleStripTab& tab :
                   tab_strip_chrome_.ComputeVisibleProjectTabs(layout.project_tab_strip)) {
                AddControlUiElement(out, kProjectTab, tab.display_title, tab.rect,
                                    tab.display_title);
                out.back().active = tab.active;
              }
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
              for (const VisibleStripTab& tab : tab_strip_chrome_.ComputeVisibleTabsForGroup(gi, strip)) {
                const std::string locator = group + "/" + tab.display_title;
                AddControlUiElement(out, kTab, locator, tab.rect, tab.display_title);
                out.back().active = tab.active;
                if (UiRectHasArea(tab.close_rect)) {
                  AddControlUiElement(out, kButton, "close-tab/" + locator, tab.close_rect);
                }
              }
            }
            for (std::size_t i = 0; i < groups.dividers.size(); ++i) {
              AddControlUiElement(out, kDivider, "editor/" + std::to_string(i),
                                  groups.dividers[i].rect);
            }
          },
      .sidebar =
          [this](const WorkspaceLayout& layout, ControlUiElements& out) {
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
              AddControlUiElement(out, kButton, "sidebar/Collapse",
                                  TreeSidebarCollapseButtonRect(layout.sidebar));
              AddControlUiElement(out, kButton, "sidebar/Refresh",
                                  TreeSidebarRefreshButtonRect(layout.sidebar));
            }
            // The list layout and the row label per view, as the sidebar mouse
            // coordinator's CurrentListLayout picks them.
            ScrollableListLayout list;
            std::size_t count = 0;
            std::size_t selected = 0;
            std::function<std::string(std::size_t)> label;
            std::function<std::string(std::size_t)> shown;  // the painted text, when it differs
            if (mode == SidebarMode::Search) {
              const std::vector<int> line_map = BuildProjectSearchLineMap();
              const auto& search = state.overlay.workflow.project_search;
              count = line_map.size();
              list = ComputeProjectSearchSidebarListLayout(layout.sidebar, count);
              selected = static_cast<std::size_t>(
                  std::max(0, FindProjectSearchResultLine(line_map, search.selected_index)));
              label = [line_map, &search](std::size_t index) -> std::string {
                const int result = line_map[index];
                if (result >= 0) {
                  const auto& hit = search.results[static_cast<std::size_t>(result)];
                  return hit.relative_path_string + ":" + std::to_string(hit.line + 1);
                }
                const std::size_t next = index + 1;
                return next < line_map.size() && line_map[next] >= 0
                           ? search.results[static_cast<std::size_t>(line_map[next])]
                                 .relative_path_string
                           : std::string{};
              };
            } else if (mode == SidebarMode::Git) {
              const std::vector<GitSidebarLine>& lines = BuildGitSidebarLines();
              count = lines.size();
              list = ComputeGitSidebarListLayout(layout.sidebar, count);
              // The selection is an ENTRY index (what the keys move and the
              // actions act on); the rows are lines, with headers and directories
              // among them. Report the line that holds the selected entry, as the
              // paint highlights it -- reporting the entry index as a line index
              // named the row above the real one (QA git-view-keys).
              selected = std::numeric_limits<std::size_t>::max();
              for (std::size_t i = 0; i < lines.size(); ++i) {
                if (lines[i].kind == GitSidebarLine::Kind::Entry &&
                    lines[i].entry_index == static_cast<int>(state.sidebar.git.selected_index)) {
                  selected = i;
                  break;
                }
              }
              label = [&lines](std::size_t index) { return lines[index].label; };
              // The row as painted, its review marker included ("[R] r1.txt"); the
              // name stays the bare label so a locator survives marking.
              shown = [&lines](std::size_t index) {
                return lines[index].display_label.empty() ? lines[index].label
                                                          : lines[index].display_label;
              };
            } else if (mode == SidebarMode::Problems) {
              const auto& entries = state.sidebar.problems.entries;
              count = entries.size();
              list = ComputeProblemsSidebarListLayout(layout.sidebar, count);
              selected = state.sidebar.problems.selected_index;
              label = [&entries](std::size_t index) { return entries[index].primary_label; };
              // The row as painted: the message, then its location column.
              shown = [&entries](std::size_t index) {
                return entries[index].primary_label + "  " + entries[index].detail_label;
              };
            } else if (mode == SidebarMode::Tests) {
              const auto& entries = state.sidebar.tests.entries;
              count = entries.size();
              list = ComputeTestsSidebarListLayout(layout.sidebar, count);
              selected = state.sidebar.tests.selected_index;
              label = [&entries](std::size_t index) { return entries[index].label; };
              // The row as painted: the name, then its status and location column.
              shown = [&entries](std::size_t index) {
                return entries[index].label + "  " + entries[index].detail_label;
              };
            } else if (UsesPluginItemSurface(mode)) {
              const auto& items = state.sidebar.plugin.items;
              count = items.size();
              list = ComputePluginSidebarListLayout(layout.sidebar, count);
              selected = state.sidebar.plugin.selected_index;
              label = [&items](std::size_t index) { return items[index].label; };
            } else {
              const auto& entries = state.directory_tree.entries();
              count = entries.size();
              list = ComputeTreeSidebarListLayout(layout.sidebar, count);
              selected = state.directory_tree.selected_index();
              label = [&entries](std::size_t index) { return entries[index].label; };
            }
            const std::string view = state.sidebar.view_id + "/";
            // `visible_rows` is how many rows FIT; the rows on screen stop at the
            // last item.
            for (int visible = 0; visible < list.visible_rows; ++visible) {
              const std::size_t index = static_cast<std::size_t>(list.scroll_row + visible);
              if (index >= count) {
                break;
              }
              std::string text = label(index);
              AddControlUiElement(out, kRow,
                                  view + (text.empty() ? "#" + std::to_string(index) : text),
                                  ScrollableListRowRect(list, visible), shown ? shown(index) : text);
              out.back().selected = index == selected;
            }
          },
      .panel =
          [this](const WorkspaceLayout& layout, ControlUiElements& out) {
            if (!BottomPanelVisible() || !UiRectHasArea(layout.bottom_panel)) {
              return;
            }
            const SDL_FRect header = MakeRect(layout.bottom_panel.x, layout.bottom_panel.y,
                                              layout.bottom_panel.w,
                                              kWorkspaceBottomPanelHeaderHeight);
            const LayoutMode mode = layout_mode_service_.CurrentMode();
            for (const VisibleStripTab& tab : tab_strip_service_.ComputeVisibleBottomPanelTabs(
                     context_.current_project_state, header, mode,
                     [this](std::string_view text) { return text_renderer_.MeasureWidth(text); },
                     output_channels_.Channels())) {
              AddControlUiElement(out, kPanelTab, tab.display_title, tab.rect, tab.display_title);
              out.back().active = tab.active;
              if (UiRectHasArea(tab.close_rect)) {
                AddControlUiElement(out, kButton, "close-panel-tab/" + tab.display_title, tab.close_rect);
              }
            }
            if (context_.current_project_state.panel.content == PanelContentKind::Terminal) {
              AddControlUiElement(out, kButton, "panel/new-terminal",
                                  tab_strip_service_.BottomPanelTerminalNewTabRect(mode, header));
              // The dividers between a split terminal's panes, by the boundary
              // they move (the same index the pane layout and the drag use).
              if (const auto* tab = context_.current_project_state.active_terminal_tab(); tab != nullptr) {
                for (const TerminalPaneDividerRect& divider :
                     tab->PaneRects(BottomPanelContentRect(layout)).dividers) {
                  AddControlUiElement(out, kDivider, "terminal/" + std::to_string(divider.boundary),
                                      divider.rect);
                }
              }
            }
            AddControlUiElement(out, kButton, "panel/maximize",
                                tab_strip_service_.BottomPanelMaximizeButtonRect(mode, header));
          },
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
                });
            // Plugin status items (drawn in the breadcrumb row, right-aligned):
            // `status:plugin/<plugin>/<item>`, the item's text as label, enabled when
            // a click runs a command.
            for (const VisibleStatusItem& item : ComputeVisibleStatusItems(layout.breadcrumb)) {
              // Registry ids are qualified with the plugin id; the name carries it once.
              std::string_view id = item.item.id;
              if (const std::string prefix = item.item.plugin_id + "."; id.starts_with(prefix)) {
                id.remove_prefix(prefix.size());
              }
              AddControlUiElement(out, kStatus, "plugin/" + item.item.plugin_id + "/" + std::string(id),
                                  item.rect, item.item.text);
              out.back().enabled = !item.item.command.empty();
            }
          },
      .buttons =
          [this](const WorkspaceLayout& layout, ControlUiElements& out) {
            // Each pane's clickable code lenses (`lens/<title>`), from the layout
            // the click hit-tests. A lens shows only on a clean buffer.
            if (const editor::PluginDecorationStore* store =
                    RenderViewModelBuilder(context_).BuildHoverTargets().decoration_store;
                store != nullptr && ActiveTabIsEditor()) {
              const bool line_numbers = SettingFlagEnabled(GetSettingValue("editor.line_numbers"), true);
              std::vector<editor::EolDecorationSegment> segments;
              for (const EditorPaneLayout& pane : ComputeEditorPaneLayouts(layout)) {
                const editor::TextViewport* viewport = ViewportForPane(pane);
                if (viewport == nullptr || viewport->is_placeholder() || viewport->path().empty() ||
                    viewport->dirty()) {
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
            if (const EditorBannerState* banner = ActiveEditorBannerForTab(context_.current_project_state);
                banner != nullptr) {
              const SDL_FRect strip = ComputeEditorBannerStripRect(layout.editor_surface);
              const EditorBannerButtonLayout buttons =
                  ComputeEditorBannerButtonRects(strip, EditorBannerButtonsFor(banner->kind));
              for (std::size_t i = 0; i < buttons.buttons.size(); ++i) {
                const std::string label(buttons.buttons[i].label);
                AddControlUiElement(out, kButton, "banner/" + label, buttons.rects[i], label);
              }
              AddControlUiElement(out, kButton, "banner/dismiss", buttons.dismiss);
            }
            // The merge editor's buttons, from the geometry its mouse path tests:
            // the toolbar, and each visible conflict's accept buttons (shown while
            // the pointer is over that conflict, which hovering the button does).
            if (MergeTabState* merge_tab = ActiveTabIsMerge() ? ActiveMergeTab() : nullptr;
                merge_tab != nullptr) {
              const SDL_FRect surface_rect = layout.editor_surface;
              const MergeSurfaceLayout surface = ComputeMergeSurfaceLayout(surface_rect, *merge_tab);
              const MergeToolbarLayout toolbar = ComputeMergeToolbarLayout(surface_rect, surface);
              AddControlUiElement(out, kButton, "merge/prev", toolbar.prev_rect, "Previous");
              AddControlUiElement(out, kButton, "merge/next", toolbar.next_rect, "Next");
              AddControlUiElement(out, kButton, "merge/open", toolbar.open_rect, "Open Result");
              AddControlUiElement(out, kButton, "merge/save", toolbar.save_rect, "Save");
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
              const MergeInteractionLayout interaction =
                  BuildMergeInteractionLayout(surface_rect, surface, *merge_tab);
              const auto visible = [&](const SDL_FRect& rect) {
                return UiRectHasArea(rect) && rect.y >= surface.rows_y &&
                       rect.y + rect.h <= surface_rect.y + surface_rect.h;
              };
              constexpr std::array<std::string_view, 4> kResultActions = {"base", "incoming", "current",
                                                                          "both"};
              for (std::size_t i = 0; i < merge_tab->conflicts.size(); ++i) {
                const MergeTrackedConflict& conflict = merge_tab->conflicts[i];
                if (!conflict.valid) {
                  continue;
                }
                const std::string prefix = "merge/" + std::to_string(i) + "/";
                for (const bool incoming : {true, false}) {
                  const SDL_FRect rect = BuildMergeSourceActionButtonRect(surface, interaction, conflict, incoming);
                  if (visible(rect)) {
                    AddControlUiElement(out, kButton, prefix + (incoming ? "accept-incoming" : "accept-current"),
                                        rect);
                  }
                }
                const std::array<SDL_FRect, 4> actions =
                    BuildMergeResultActionButtonRects(surface, interaction, conflict);
                for (std::size_t a = 0; a < actions.size(); ++a) {
                  if (visible(actions[a])) {
                    AddControlUiElement(out, kButton, prefix + "result-" + std::string(kResultActions[a]),
                                        actions[a]);
                  }
                }
              }
            }
            // The compare surface: its divider, and each collapsed-context block's
            // buttons, numbered top to bottom on screen and resolved through the
            // helpers the click and the cursor shape use.
            if (CompareTabState* compare_tab = ActiveTabIsCompare() ? ActiveCompareTab() : nullptr;
                compare_tab != nullptr) {
              const SDL_FRect surface_rect = layout.editor_surface;
              const CompareSurfaceLayout surface = ComputeCompareSurfaceLayout(surface_rect, *compare_tab);
              AddControlUiElement(out, kDivider, "compare", CompareDividerHitRect(surface_rect, surface));
              int block = 0;
              const float bottom = surface_rect.y + surface_rect.h;
              for (float y = surface.rows_y + surface.line_height / 2.0f;
                   surface.line_height > 0.0f && y < bottom; y += surface.line_height) {
                const auto hit = CompareCollapsedContextRowAt(*compare_tab, surface_rect, surface.rows_y,
                                                              surface.line_height, surface.show_vertical, y);
                if (!hit.has_value()) {
                  continue;
                }
                const CollapsedContextActionRects rects = BuildCollapsedContextActionRects(
                    editor_text_renderer_, hit->block_rect, hit->row->previous_hunk_index >= 0,
                    hit->row->next_hunk_index >= 0);
                const std::string prefix = "compare/context-" + std::to_string(block++) + "/";
                if (rects.previous_rect.has_value()) {
                  AddControlUiElement(out, kButton, prefix + "previous", *rects.previous_rect);
                }
                // Labelled with the count the row shows: a partial reveal keeps the
                // block where it was (the view scrolls with it), so the count is
                // what tells a reveal happened.
                AddControlUiElement(out, kButton, prefix + "all", rects.all_rect,
                                    std::to_string(hit->row->collapsed_line_count));
                if (rects.next_rect.has_value()) {
                  AddControlUiElement(out, kButton, prefix + "next", *rects.next_rect);
                }
              }
            }
            const NotificationsViewModel toasts = RenderViewModelBuilder(context_).BuildNotifications(
                notification_service_, layout.status_bar, text_renderer_);
            for (const NotificationEntryViewModel& entry : toasts.entries) {
              // The card itself (a click dismisses it), by its index in the
              // `notifications` query, with its message as the label.
              AddControlUiElement(out, kNotification, std::to_string(entry.index), entry.layout.rect,
                                  std::string(entry.message));
              out.back().active = entry.focused;
              for (const NotificationButtonViewModel& button : entry.buttons) {
                const std::string label(button.label);
                AddControlUiElement(out, kButton, "notification/" + label, button.rect, label);
              }
            }
          },
      .overlay =
          [this](const WorkspaceLayout& layout, ControlUiElements& out) {
            if (!context_.current_project_state.overlay.visible) {
              return;
            }
            OverlaySurfaceViewModel vm;
            RenderViewModelBuilder(context_).BuildOverlaySurfaceInto(
                vm, layout, ComputeOverlayRect(layout.editor_area), text_renderer_);
            if (vm.mode == OverlayMode::BufferSearch || vm.mode == OverlayMode::BufferReplace) {
              const FindWidgetLayout& fw = vm.find_widget.fw;
              const auto& search = context_.current_project_state.overlay.workflow.buffer_search;
              AddControlUiElement(out, kField, "find/search", fw.search_field,
                                  search.query.text());
              out.back().active = vm.find_widget.search_focused;
              if (fw.replace_mode) {
                AddControlUiElement(out, kField, "find/replace", fw.replace_field,
                                    search.replace_text.text());
                out.back().active = vm.find_widget.replace_focused;
                AddControlUiElement(out, kButton, "find/replace", fw.replace_button);
                AddControlUiElement(out, kButton, "find/replace-all", fw.replace_all_button);
              }
              static constexpr std::array<std::string_view, kBufferFindToggleCount> kToggles = {
                  "match-case", "whole-word", "regex"};
              for (std::size_t i = 0; i < fw.toggle_count && i < kToggles.size(); ++i) {
                AddControlUiElement(out, kButton, std::string("find/") + std::string(kToggles[i]),
                                    fw.toggle_buttons[i]);
                out.back().active = vm.find_widget.toggles[i].active;
              }
              AddControlUiElement(out, kButton, "find/previous", fw.prev_button);
              AddControlUiElement(out, kButton, "find/next", fw.next_button);
              AddControlUiElement(out, kButton, "find/close", fw.close_button);
              return;
            }
            // The list overlays: one row per visible entry, named by its primary
            // text (as painted, so a long label is the truncated one).
            for (std::size_t i = 0; i < vm.rows.size(); ++i) {
              const std::size_t index = static_cast<std::size_t>(vm.list_layout.scroll_row) + i;
              const std::string label(vm.rows[i].primary);
              AddControlUiElement(out, kOverlayRow,
                                  label.empty() ? "#" + std::to_string(index) : label,
                                  ScrollableListRowRect(vm.list_layout, static_cast<int>(i)), label);
              out.back().selected = static_cast<int>(index) == vm.selected_row;
            }
          },
      .dialogs =
          [this](const WorkspaceLayout& layout, ControlUiElements& out) {
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
                AddControlUiElement(out, kField, "prompt", ComputePromptSurfaceInputRect(dialog),
                                    prompt.input.text());
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
            if (!settings_overlay_service_.Visible() ||
                settings_overlay_service_.Mode() != SettingsOverlayMode::Settings) {
              return;
            }
            const SettingsOverlayViewModel vm = RenderViewModelBuilder(context_).BuildSettingsOverlay(
                layout, settings_overlay_service_, text_renderer_);
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
            // The rows the pane shows, not the category's whole list: a driver
            // reading "the first row" after a wheel must see it move.
            for (const SettingsRowViewModel& row : vm.rows) {
              if (!SettingsRowShownIn(vm.right_pane_rect, row.row_rect)) {
                continue;
              }
              const std::string id(row.id);
              // The row's label is its current value as shown: the checkbox state,
              // or the stepper/segmented/text value.
              std::string value = row.control.kind == SettingsControlKind::Checkbox
                                      ? std::string(row.control.checkbox_on ? "on" : "off")
                                      : std::string(row.control.display_value);
              AddControlUiElement(out, kSetting, id,
                                  SettingsRowVisibleRect(vm.right_pane_rect, row.row_rect),
                                  std::move(value));
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
            // The font picker's rows while a font row is being edited: the same
            // overlay-row kind as every other list a person picks from, `selected`
            // on the highlighted one.
            if (vm.value_picker.visible) {
              for (const SettingsPickerItemViewModel& item : vm.value_picker.items) {
                if (!has_area(item.rect)) {
                  continue;
                }
                AddControlUiElement(out, kOverlayRow, std::string(item.text), item.rect, std::string(item.text));
                out.back().selected = item.highlighted;
              }
            }
          },
      .fields =
          [this](const WorkspaceLayout& layout, ControlUiElements& out) {
            if (!context_.current_project_state.sidebar.visible ||
                !UiRectHasArea(layout.sidebar)) {
              return;
            }
            const SidebarMode mode = ActiveSidebarMode();
            if (mode == SidebarMode::Search) {
              const auto& search = context_.current_project_state.overlay.workflow.project_search;
              static constexpr std::array<std::string_view, 4> kNames = {"query", "replace",
                                                                         "include", "exclude"};
              const std::array<const editor::SingleLineEditor*, 4> committed = {
                  &search.query, &search.replace_text, &search.include_globs, &search.exclude_globs};
              const auto slots = project_search_panel::SidebarSearchFieldRects(
                  layout.sidebar, search.scope_expanded);
              for (std::size_t i = 0; i < slots.size(); ++i) {
                if (slots[i].rect.w <= 0.0f) {
                  continue;
                }
                // The field being edited shows its live edit buffer.
                const bool editing = search.editing && search.edit_field == slots[i].field;
                AddControlUiElement(out, kField, "search/" + std::string(kNames[i]), slots[i].rect,
                                    editing ? search.edit_buffer.text() : committed[i]->text());
                out.back().active = editing;
              }
              AddControlUiElement(out, kButton, "search/mode",
                                  project_search_panel::ModeButtonRect(layout.sidebar));
              AddControlUiElement(out, kButton, "search/case",
                                  project_search_panel::CaseButtonRect(layout.sidebar));
              AddControlUiElement(out, kButton, "search/hidden",
                                  project_search_panel::HiddenButtonRect(layout.sidebar));
              AddControlUiElement(out, kButton, "search/scope",
                                  project_search_panel::ScopeButtonRect(layout.sidebar));
              AddControlUiElement(out, kButton, "search/replace-all",
                                  project_search_panel::ReplaceAllButtonRect(layout.sidebar));
              return;
            }
            if (mode != SidebarMode::Git) {
              return;
            }
            const SDL_FRect sidebar = layout.sidebar;
            // The branch and Sync buttons' labels are the ones the render draws
            // (the branch name; Sync with its behind/ahead counts), from the same
            // revision-exact memo.
            const GitSidebarViewModel& git_vm =
                CachedGitSidebarPresentation(context_.current_project_state.sidebar.git,
                                             context_.current_project_state.root,
                                             context_.current_project_state.branch_review)
                    .view_model;
            AddControlUiElement(out, kButton, "git/stage-all",
                                git_sidebar_header::StageAllButtonRect(sidebar), "Stage All");
            out.back().enabled = CanStageAllGitSidebarEntries();
            AddControlUiElement(out, kButton, "git/discard-all",
                                git_sidebar_header::DiscardAllButtonRect(sidebar), "Discard All");
            out.back().enabled = CanDiscardAllGitSidebarEntries();
            AddControlUiElement(out, kButton, "git/refresh",
                                git_sidebar_header::RefreshButtonRect(sidebar), "Refresh");
            AddControlUiElement(out, kButton, "git/branch",
                                git_sidebar_header::BranchButtonRect(sidebar), git_vm.branch_button_label);
            AddControlUiElement(out, kButton, "git/sync", git_sidebar_header::SyncButtonRect(sidebar),
                                git_vm.sync_button_label);
            // The Outgoing section's base dropdown (Auto, the previous commit, a ref).
            if (const auto outgoing_base = GitSidebarOutgoingBaseButtonRect(sidebar); outgoing_base.has_value()) {
              AddControlUiElement(out, kButton, "git/outgoing-base", *outgoing_base);
            }
            const auto& workflow = context_.current_project_state.sidebar.git.commit_workflow;
            if (!workflow.open) {
              AddControlUiElement(out, kButton, "git/commit",
                                  git_sidebar_header::CommitButtonRect(sidebar), "Commit");
              out.back().enabled = git_vm.commit_ready;
              return;
            }
            // The commit panel's rects are the ones its render cached (they sit
            // under a content-sized summary, so they are not a pure layout).
            AddControlUiElement(out, kField, "git/subject", workflow.subject_field_rect,
                                workflow.subject.text());
            out.back().active = workflow.focus_field == CommitWorkflowFocusField::Subject;
            AddControlUiElement(out, kField, "git/body", workflow.body_field_rect, workflow.BodyText());
            out.back().active = workflow.focus_field == CommitWorkflowFocusField::Body;
            AddControlUiElement(out, kButton, "git/confirm-commit", workflow.commit_button_rect);
          },
      .debug =
          [this](const WorkspaceLayout& layout, ControlUiElements& out) {
            const ProjectWorkspaceState& ps = context_.current_project_state;
            // The floating toolbar: the layout and enablement its click handler
            // (HandleDebugToolbarButtonDown) uses.
            if (DebugToolbarVisible()) {
              const DebugToolbarLayout tb = ComputeDebugToolbarLayout(
                  layout.editor_surface, DebugToolbarAvoidBelowY(layout), DebugSupportsReverse());
              const bool stopped = IsDebugSessionStopped();
              for (std::size_t i = 0; i < tb.button_count; ++i) {
                const DebugToolbarButton button = tb.kinds[i];
                AddControlUiElement(out, kButton,
                                    "debug-toolbar/" + std::string(DebugToolbarButtonId(button)),
                                    tb.buttons[i],
                                    std::string(DebugToolbarButtonTooltip(button, stopped)));
                const bool step = button != DebugToolbarButton::ContinuePause &&
                                  button != DebugToolbarButton::Restart &&
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
            // The visible rows, each with the band DebugPaneRowAtPoint maps a click
            // in to that row (`debug-row:<mode>/<index>`, index into the mode's rows).
            const DebugPaneSurfaceSpec* spec = FindDebugPaneSurface(ps.debug_pane.mode);
            const std::string mode_id = spec != nullptr ? std::string(spec->id) : "debug";
            const std::size_t count = DebugPaneActiveRowCount();
            const LogSurfaceLayout list = ComputeDebugPaneListLayout(layout, count);
            for (int r = 0; r < list.scroll.visible_rows; ++r) {
              const std::size_t index =
                  static_cast<std::size_t>(list.scroll.vertical_scroll + r);
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
              const float y = list.text_y + static_cast<float>(r) * list.line_height;
              AddControlUiElement(out, kDebugRow, mode_id + "/" + std::to_string(index),
                                  MakeRect(list.content_rect.x, y, list.content_rect.w,
                                           list.line_height),
                                  std::move(label));
              out.back().selected = selected;
            }
          },
      .details =
          [this](const WorkspaceLayout& layout, util::JsonObject& root) {
            const ProjectWorkspaceState& state = context_.current_project_state;
            if (state.overlay.visible) {
              util::JsonObject& overlay = std::get<util::JsonObject>(root["overlay"].v);
              overlay["count"] = util::JsonValue(static_cast<std::int64_t>(OverlayItemCount()));
              overlay["selected"] = util::JsonValue(static_cast<std::int64_t>(OverlaySelectedIndex()));
              if (const editor::SingleLineEditor* query = OverlayQueryEditor(state); query != nullptr) {
                overlay["query"] = util::JsonValue(query->text());
              }
            }
            if (context_.prompts.surface_visible || context_.prompts.dirty_visible) {
              util::JsonObject prompt;
              const bool dirty = context_.prompts.dirty_visible;
              prompt["kind"] = util::JsonValue(std::string(
                  dirty ? "unsaved"
                        : context_.prompts.surface.kind == PromptSurfaceState::Kind::TextInput ? "input"
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
                  break;
                case EditorHoverTarget::Kind::Diagnostic:
                  kind = "diagnostic";
                  text = hover->diagnostic.has_value() ? hover->diagnostic->message : std::string{};
                  break;
                case EditorHoverTarget::Kind::Plugin:
                  kind = "symbol";
                  if (hover->plugin_hover.has_value()) {
                    text = hover->plugin_hover->title.empty()
                               ? hover->plugin_hover->content
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
              util::JsonArray rect;
              for (const float v : {hover->rect.x, hover->rect.y, hover->rect.w, hover->rect.h}) {
                rect.push_back(util::JsonValue(static_cast<std::int64_t>(v)));
              }
              entry["rect"] = util::JsonValue(std::move(rect));
              popups["hover"] = util::JsonValue(std::move(entry));
            }
            popups["signatureHelp"] =
                active_signature_help_.has_value()
                    ? util::JsonValue(active_signature_help_->signature)
                    : util::JsonValue(nullptr);
            root["popups"] = util::JsonValue(std::move(popups));

            // The last action a key, menu row, palette entry or command ran
            // (ActionCoordinator's receipt), by command name.
            {
              const WorkspaceActionContext::ActionReceipt& receipt =
                  MakeActionContext().LastActionReceipt();
              util::JsonObject actions;
              actions["dispatched"] = util::JsonValue(static_cast<std::int64_t>(receipt.dispatched));
              actions["last"] = util::JsonValue(nullptr);
              if (receipt.last.has_value()) {
                for (const ActionSpec& spec : WorkspaceCommandSpecs()) {
                  if (spec.id == *receipt.last) {
                    actions["last"] = util::JsonValue(std::string(
                        spec.command_name.empty() ? spec.label : spec.command_name));
                    break;
                  }
                }
              }
              const char* source = "command";
              switch (receipt.source) {
                case ActionSource::Command: source = "command"; break;
                case ActionSource::Shortcut: source = "shortcut"; break;
                case ActionSource::Menu: source = "menu"; break;
                case ActionSource::ContextMenu: source = "context-menu"; break;
              }
              actions["source"] = util::JsonValue(std::string(source));
              actions["rejected"] = util::JsonValue(receipt.rejected);
              root["actions"] = util::JsonValue(std::move(actions));
            }

            // Per editor pane: the text geometry a click is resolved against
            // (EditorMouseCoordinator's fast pointer metrics), and which buffer
            // line each visible row shows, so a driver can click a line and column
            // or a fold marker without knowing the font. Inline insets (plugin
            // surfaces, code lens, ghost text) are not modelled.
            util::JsonArray editors;
            const bool line_numbers = SettingFlagEnabled(GetSettingValue("editor.line_numbers"), true);
            for (const EditorPaneLayout& pane : ComputeEditorPaneLayouts(layout)) {
              const editor::TextViewport* viewport = ViewportForPane(pane);
              if (viewport == nullptr || viewport->is_placeholder()) {
                continue;
              }
              // The band the last frame painted, as a hover resolves it.
              const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
                  editor_text_renderer_, *viewport, pane.rect, viewport->sticky_band_rows(), line_numbers);
              const auto num = [](float v) { return util::JsonValue(static_cast<double>(v)); };
              util::JsonObject entry;
              entry["group"] = util::JsonValue(static_cast<std::int64_t>(pane.group_index));
              entry["focused"] = util::JsonValue(pane.active);
              util::JsonArray rect;
              for (const float v : {pane.rect.x, pane.rect.y, pane.rect.w, pane.rect.h}) {
                rect.push_back(util::JsonValue(static_cast<std::int64_t>(v)));
              }
              entry["rect"] = util::JsonValue(std::move(rect));
              entry["textX"] = num(metrics.text_x);
              entry["firstLineY"] = num(metrics.first_line_y);
              entry["lineHeight"] = num(metrics.line_height);
              entry["charWidth"] = num(editor_text_renderer_.CharWidth());
              entry["gutterX"] = num(pane.rect.x);
              entry["gutterWidth"] = num(metrics.gutter_width);
              entry["foldHitX"] = num(editor::kGutterFoldHitLeft(pane.rect.x, metrics.gutter_width));
              entry["stickyRows"] = util::JsonValue(static_cast<std::int64_t>(metrics.sticky_scroll_rows));
              entry["scrollColumn"] =
                  util::JsonValue(static_cast<std::int64_t>(viewport->horizontal_scroll()));
              util::JsonArray rows;
              const std::size_t visual_count = viewport->visual_line_count();
              for (std::size_t i = 0; i < metrics.visible_rows; ++i) {
                const std::size_t visual = viewport->scroll_line() + i;
                if (visual >= visual_count) {
                  break;
                }
                util::JsonObject row;
                row["line"] = util::JsonValue(
                    static_cast<std::int64_t>(viewport->VisualRowLineIndex(visual) + 1));
                row["y"] = num(metrics.first_line_y + static_cast<float>(i) * metrics.line_height);
                rows.push_back(util::JsonValue(std::move(row)));
              }
              entry["rows"] = util::JsonValue(std::move(rows));
              editors.push_back(util::JsonValue(std::move(entry)));
            }
            root["editors"] = util::JsonValue(std::move(editors));

            // The active terminal pane's text geometry, as its mouse path resolves
            // a point (CellInPaneLayout): row r of the transcript (0-based, the
            // `terminal-output` line) sits at firstLineY + (r - firstRow) *
            // lineHeight, column c at textX + c * charWidth.
            if (const TerminalPaneState* pane = ActiveTerminalPane(); pane != nullptr && BottomPanelVisible()) {
              const auto panel_layout = ComputeBottomPanelLogLayout(layout, pane->session.LineCount());
              const auto num = [](float v) { return util::JsonValue(static_cast<double>(v)); };
              util::JsonObject terminal;
              util::JsonArray rect;
              for (const float v : {panel_layout.content_rect.x, panel_layout.content_rect.y,
                                    panel_layout.content_rect.w, panel_layout.content_rect.h}) {
                rect.push_back(util::JsonValue(static_cast<std::int64_t>(v)));
              }
              terminal["rect"] = util::JsonValue(std::move(rect));
              terminal["textX"] = num(panel_layout.text_x);
              terminal["firstLineY"] = num(panel_layout.text_y);
              terminal["lineHeight"] = num(panel_layout.line_height);
              terminal["charWidth"] = num(std::max(1.0f, terminal_text_renderer_.CharWidth()));
              terminal["firstRow"] =
                  util::JsonValue(static_cast<std::int64_t>(std::max(0, panel_layout.scroll.vertical_scroll)));
              terminal["visibleRows"] =
                  util::JsonValue(static_cast<std::int64_t>(std::max(0, panel_layout.scroll.visible_rows)));
              root["terminal"] = util::JsonValue(std::move(terminal));
            }
          },
  };
}

}  // namespace microide::workspace
