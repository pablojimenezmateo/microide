#include "workspace/shell/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

#include "project/GitRepositoryMetadataTracker.h"
#include "project/GitMetadataSource.h"
#include "util/PathMatch.h"
#include "workspace/git/GitRepositoryService.h"
#include "workspace/WorkspaceProjectPresentation.h"
#include "workspace/WorkspaceUiText.h"
#include "workspace/WorkspaceProjectSearchPresentation.h"
#include "workspace/control/ControlUiQuery.h"
#include "workspace/render/RenderViewModelBuilder.h"

namespace microide::workspace {

const std::string& WorkspaceShell::BreadcrumbLabel() const {
  return BreadcrumbLabel(context_.current_project_state.clamped_focused_group_index());
}

// By reference, not by value: the memo below already avoids REBUILDING the label,
// but returning it by value copied it on every hit — and the callers are the
// window-chrome paint, so that was an allocation per painted frame for a string
// that had not changed (and one more on a miss, since the builder's local was
// copied into the cache and then returned).
//
// Per GROUP, because the band is per pane: each top-row pane of a split carries
// its own breadcrumb over its own column, and one shared memo would rebuild the
// string every time the paint moved from one pane to the next. One slot per pane;
// idle slots hold no string.
const std::string& WorkspaceShell::BreadcrumbLabel(std::size_t group_index) const {
  static const std::filesystem::path kEmptyPath;
  const std::vector<EditorGroup>& groups = context_.current_project_state.editor_groups;
  const std::size_t slot = group_index < groups.size() && group_index < kMaxEditorGroups
                               ? group_index
                               : context_.current_project_state.clamped_focused_group_index();
  BreadcrumbLabelCache& cache = breadcrumb_label_cache_[std::min(slot, kMaxEditorGroups - 1)];
  // Resolve the label inputs (mode, path, secondary labels) by reference so a cache
  // hit compares without allocating; only a genuine input change rebuilds the string.
  int mode = 0;
  bool placeholder = false;
  const std::filesystem::path* path = &kEmptyPath;
  std::string_view left;
  std::string_view right;
  const EditorGroup& group = groups[std::min(slot, groups.size() - 1)];
  const TabEntry* active = group.active_tab_index < group.open_tabs.size()
                               ? &group.open_tabs[group.active_tab_index]
                               : nullptr;
  if (active != nullptr && active->kind == TabEntry::Kind::Compare) {
    if (!active->compare.has_value()) {
      // No inputs to memo against, so mark the memo stale and lend the fallback.
      cache.valid = false;
      cache.label = "compare";
      return cache.label;
    }
    mode = 1;
    path = &active->compare->path;
    left = active->compare->left_label;
    right = active->compare->right_label;
  } else if (active != nullptr && active->kind == TabEntry::Kind::Merge) {
    if (!active->merge.has_value()) {
      cache.valid = false;
      cache.label = "merge";
      return cache.label;
    }
    mode = 2;
    path = &active->merge->output_path;
    left = active->merge->incoming_label;
    right = active->merge->current_label;
  } else {
    const editor::TextViewport* viewport = GroupActiveViewport(group);
    mode = 0;
    if (viewport != nullptr) {
      path = &viewport->path();
      placeholder = viewport->is_placeholder();
    }
  }

  const std::filesystem::path& root = context_.current_project_state.root;
  if (cache.valid && cache.mode == mode && cache.placeholder == placeholder &&
      cache.root == root && cache.path == *path && cache.left_label == left &&
      cache.right_label == right) {
    return cache.label;
  }

  switch (mode) {
    case 1:
      cache.label = BuildCompareBreadcrumbLabel(root, *path, left, right);
      break;
    case 2:
      cache.label = BuildMergeBreadcrumbLabel(root, *path, left, right);
      break;
    default:
      // A remote project's copy of a host file outside the project lives in a
      // cache; the path bar names the host's file, as go to definition did.
      cache.label = context_.current_project_state.launcher().DisplayPathOf(*path);
      if (cache.label.empty()) {
        cache.label = BuildEditorBreadcrumbLabel(root, *path, placeholder);
      }
      break;
  }
  cache.valid = true;
  cache.mode = mode;
  cache.placeholder = placeholder;
  cache.root = root;
  cache.path = *path;
  cache.left_label = left;
  cache.right_label = right;
  return cache.label;
}

std::string WorkspaceShell::ProjectLabel() const {
  return context_.current_project_state.root.empty() ? "microide" : ProjectLabelForRoot(context_.current_project_state.root);
}

std::string WorkspaceShell::ProjectTabDisplayTitle(std::size_t index) const {
  if (index >= context_.project_catalog.entries.size()) {
    return {};
  }
  const std::filesystem::path& root = ProjectCatalogRoot(index);
  const std::string label = ProjectLabelForRoot(root);
  // The PREDICATE, not the list. This runs once per project tab per painted
  // frame, and DirtyEditorTabIndicesForProject reserves a vector sized to the
  // tab count before it finds out whether anything is dirty -- so the common
  // "nothing dirty" answer cost a heap allocation per tab per frame.
  return HasDirtyEditorTabForProject(index) ? "*" + label : label;
}

std::string WorkspaceShell::ProjectTabTooltipLabel(std::size_t index) const {
  if (index >= context_.project_catalog.entries.size()) {
    return {};
  }
  const std::filesystem::path& root = ProjectCatalogRoot(index);
  if (root.empty()) {
    return ProjectLabelForRoot(root);
  }
  // Catalog roots are normalized on the way in, so `lexically_normal()` here was
  // ~12 allocations producing a path identical to the one it was given -- once
  // per project tab per painted frame (TD-2026-08-10-174).
  if (!util::PathTextNeedsNormalizing(root.native())) {
    return root.string();
  }
  return root.lexically_normal().string();
}

SDL_FRect WorkspaceShell::ComputeCaretAnchoredOverlayRect(const SDL_FRect& editor_area,
                                                          const SDL_FRect& caret_anchor) const {
  // Compact list popup placed next to the caret rather than a centered modal. Sized to the
  // visible row count and clamped inside the editor area, flipping above the caret when there
  // is not enough room below.
  constexpr float kRowStep = 22.0f;       // must match ComputeOverlayListLayout's row_step
  constexpr float kListBottomPadding = 16.0f;
  constexpr float kMaxVisibleRows = 9.0f;
  constexpr float kMargin = 4.0f;

  const int rows = std::clamp(static_cast<int>(OverlayItemCount()), 1,
                              static_cast<int>(kMaxVisibleRows));
  const float height =
      OverlayListStartOffset() + kListBottomPadding + static_cast<float>(rows) * kRowStep + 6.0f;
  const float width =
      std::min(editor_area.w - 2.0f * kMargin, std::max(260.0f, editor_area.w * 0.42f));

  float x = caret_anchor.x;
  if (x + width > editor_area.x + editor_area.w - kMargin) {
    x = editor_area.x + editor_area.w - width - kMargin;
  }
  x = std::max(x, editor_area.x + kMargin);

  float y = caret_anchor.y + caret_anchor.h + 2.0f;
  if (y + height > editor_area.y + editor_area.h - kMargin) {
    const float above_y = caret_anchor.y - height - 2.0f;
    y = above_y >= editor_area.y + kMargin
            ? above_y
            : std::max(editor_area.y + kMargin, editor_area.y + editor_area.h - height - kMargin);
  }

  return SDL_FRect{std::floor(x), std::floor(y), std::floor(width), std::floor(height)};
}

SDL_FRect WorkspaceShell::ComputeCenteredMenuOverlayRect(const SDL_FRect& editor_area) const {
  // Compact centered menu sized to the visible row count (matches the row metrics
  // ComputeCaretAnchoredOverlayRect uses), but horizontally centered and pinned to
  // a fixed vertical fraction — the canonical modal placement, without the
  // finder-sized footprint that would dwarf a two-or-three-item action list.
  constexpr float kRowStep = 22.0f;  // must match ComputeOverlayListLayout's row_step
  constexpr float kListBottomPadding = 16.0f;
  constexpr float kMaxVisibleRows = 9.0f;
  constexpr float kVerticalFrac = 0.30f;

  const int rows = std::clamp(static_cast<int>(OverlayItemCount()), 1,
                              static_cast<int>(kMaxVisibleRows));
  const float height =
      OverlayListStartOffset() + kListBottomPadding + static_cast<float>(rows) * kRowStep + 6.0f;
  const float width =
      std::min(editor_area.w - 32.0f, std::max(320.0f, editor_area.w * 0.42f));

  const float x = editor_area.x + (editor_area.w - width) * 0.5f;
  const float y = editor_area.y + std::max(0.0f, (editor_area.h - height) * kVerticalFrac);
  return SDL_FRect{std::floor(x), std::floor(y), std::floor(width), std::floor(height)};
}

SDL_FRect WorkspaceShell::FindWidgetAnchorRect(const SDL_FRect& fallback) const {
  // The find/replace widget floats over the editor text region (below the
  // breadcrumb). Fall back to the supplied rect only if the surface is unavailable.
  if (const auto layout = CurrentWorkspaceLayout();
      layout.has_value() && layout->editor_surface.w > 0.0f && layout->editor_surface.h > 0.0f) {
    return layout->editor_surface;
  }
  return fallback;
}

SDL_FRect WorkspaceShell::ComputeOverlayRect(const SDL_FRect& editor_area) const {
  const OverlayState& overlay = context_.current_project_state.overlay;
  if (overlay.mode == OverlayMode::Completion && overlay.caret_anchor.has_value()) {
    return ComputeCaretAnchoredOverlayRect(editor_area, *overlay.caret_anchor);
  }
  // Code actions are a centered, content-sized menu — the canonical modal look.
  if (overlay.mode == OverlayMode::CodeActions) {
    return ComputeCenteredMenuOverlayRect(editor_area);
  }
  // Local file search is a compact non-modal widget pinned to the top-right of the
  // editor *text* area (editor_surface, below the breadcrumb) — VSCode-style, not a
  // centered modal. Anchoring to editor_surface (not editor_area) keeps it clear of
  // the breadcrumb so editor redraws fully cover it. The same helper drives the
  // renderer, field hit-test, redraw region, and reveal so they stay aligned.
  if (overlay.mode == OverlayMode::BufferSearch || overlay.mode == OverlayMode::BufferReplace) {
    return ComputeFindWidgetRect(FindWidgetAnchorRect(editor_area),
                                 overlay.mode == OverlayMode::BufferReplace);
  }
  // Every quick-open modal (file finder, project search, command palette, commit
  // and launch pickers) shares one card so the surface does not resize and shift
  // as the user flips between them.
  return ComputeQuickOpenOverlaySurfaceRect(editor_area);
}

void WorkspaceShell::RefreshStatusBar() {
  std::string_view startup_mode_text;
  std::string_view startup_mode_tooltip;
  if (startup_options_.safe_mode) {
    startup_mode_text = "Safe mode";
    startup_mode_tooltip =
        "Plugins disabled; workspace/session restore skipped at startup";
  } else if (startup_options_.disable_plugins) {
    startup_mode_text = "Plugins off";
    startup_mode_tooltip = "User-scope plugins and plugin syntax are disabled";
  }

  status_bar_model_service_.Refresh(
      status_bar_service_,
      StatusBarModelService::Operations{
          // Both through the project's metadata source: the LOCAL one is never
          // Unknown (`.git` is there or it is not), a remote one is Unknown until
          // the host's first git/metadata lands (TD-2026-10-06-319).
          .git_availability =
              [this](const std::filesystem::path& project_root) {
                return project::GitMetadataFor(context_.current_project_state.launcher())
                    .Availability(project_root);
              },
          .read_head_branch =
              [this](const std::filesystem::path& project_root) {
                return project::ReadHeadBranchName(
                    project_root,
                    project::GitMetadataFor(context_.current_project_state.launcher()));
              },
          .active_lsp_status_strings =
              [this](bool ensure_started, std::string& text, std::string& tooltip,
                     StatusBarSegmentTone& tone) {
                ActiveLspStatusStrings(ensure_started, text, tooltip, tone);
              },
          .startup_mode_text = startup_mode_text,
          .startup_mode_tooltip = startup_mode_tooltip,
      },
      context_.current_project_state, StatusBarSourceViewport());
}

const editor::TextViewport* WorkspaceShell::StatusBarSourceViewport() const {
  // The bar describes whatever the caret is actually in. On a merge tab that is
  // the result buffer and on a compare tab the editable right side — reading the
  // group's active *editor* viewport instead left the previously open file's
  // language and indent on the bar while a merge of a .cpp was on screen (the
  // media stills showed "markdown  Tabs: 4" over a C++ three-way merge). The
  // compare tab's read-only left side has no navigable viewport, so fall back
  // rather than blanking the bar there.
  if (const editor::TextViewport* navigable = ActiveNavigableViewport(); navigable != nullptr) {
    return navigable->is_placeholder() ? nullptr : navigable;
  }
  // With no tab open the group resolves to its welcome surface's built-in
  // placeholder buffer; describing that put "unknown  Tabs: 4  UTF-8 · LF" on
  // the bar over an empty editor area. VS Code shows none of those segments
  // with no editor open. `is_placeholder()` is cleared by every real-content
  // path, `SetUntitledBuffer` included, so Ctrl+N keeps its segments.
  const editor::TextViewport* active = ActiveEditorViewport();
  return active != nullptr && !active->is_placeholder() ? active : nullptr;
}

}  // namespace microide::workspace

namespace microide::workspace {

namespace {

bool UiRectHasArea(const SDL_FRect& rect) { return rect.w > 0.0f && rect.h > 0.0f; }

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
      .menu_label =
          [this](MenuId id) {
            const MenuSpec* spec = FindMenuSpec(id);
            return spec == nullptr ? std::string{} : std::string(spec->label);
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
            const auto menu_name = [this](MenuId id) {
              const MenuSpec* spec = FindMenuSpec(id);
              return spec == nullptr || spec->label.empty() ? std::string("menu")
                                                            : std::string(spec->label);
            };
            for (const auto& item : ComputeVisibleMenuBarItems(layout.menu_bar)) {
              const std::string name = menu_name(item.id);
              AddControlUiElement(out, kMenu, name, item.rect, name);
              out.back().active = menus.menu_bar_open && menus.active_menu_id == item.id;
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
              selected = state.sidebar.git.selected_index;
              label = [&lines](std::size_t index) { return lines[index].label; };
            } else if (mode == SidebarMode::Problems) {
              const auto& entries = state.sidebar.problems.entries;
              count = entries.size();
              list = ComputeProblemsSidebarListLayout(layout.sidebar, count);
              selected = state.sidebar.problems.selected_index;
              label = [&entries](std::size_t index) { return entries[index].primary_label; };
            } else if (mode == SidebarMode::Tests) {
              const auto& entries = state.sidebar.tests.entries;
              count = entries.size();
              list = ComputeTestsSidebarListLayout(layout.sidebar, count);
              selected = state.sidebar.tests.selected_index;
              label = [&entries](std::size_t index) { return entries[index].label; };
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
                                  ScrollableListRowRect(list, visible), text);
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
            }
            if (context_.current_project_state.panel.content == PanelContentKind::Terminal) {
              AddControlUiElement(out, kButton, "panel/new-terminal",
                                  tab_strip_service_.BottomPanelTerminalNewTabRect(mode, header));
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
          },
      .buttons =
          [this](const WorkspaceLayout& layout, ControlUiElements& out) {
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
            const NotificationsViewModel toasts = RenderViewModelBuilder(context_).BuildNotifications(
                notification_service_, layout.status_bar, text_renderer_);
            for (const NotificationEntryViewModel& entry : toasts.entries) {
              for (const NotificationButtonViewModel& button : entry.buttons) {
                const std::string label(button.label);
                AddControlUiElement(out, kButton, "notification/" + label, button.rect, label);
              }
            }
          },
  };
}

}  // namespace microide::workspace
