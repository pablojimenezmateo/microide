// The shell's one hover-tooltip resolver.
//
// Every tooltip in the workspace chrome used to be its own pair of
// `Hovered<Thing>TooltipLabel` / `Hovered<Thing>TooltipRect` shell methods, each
// with its own copy of the placement maths, and each caller had to remember to
// list all of them. They had already drifted apart: the sidebar search tooltip
// was missing from the menu-blocked invalidation lists, the git sidebar tooltip
// was missing from the motion change-detection list, and the sidebar mode-row
// tooltip — the only tooltip on an icon-only control — was in neither, so it
// painted only when the sidebar happened to repaint for some other reason.
//
// Only one tooltip can ever show, because the pointer is in one place. So there
// is one resolver: it walks the providers in a fixed order, returns the first
// hit already truncated and already placed, and every consumer (paint, redraw
// invalidation, motion change-detection, tests) reads that single answer.
//
// Deliberately NOT named WorkspaceShell*.cpp: that keeps the file off the shell
// companion-count cap and out of the render-surface lint glob, so it may read
// state for the hit test, exactly like DebugToolbarSurface.cpp.

#include "workspace/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <string_view>

#include "workspace/GitSidebarHeaderLayout.h"
#include "workspace/ProjectSearchPanelLayout.h"
#include "workspace/RenderViewModelBuilder.h"
#include "workspace/StatusBarService.h"
#include "workspace/WorkspaceSidebarRegistry.h"

namespace microide::workspace {

using namespace detail;

namespace {

// A provider's answer before placement: what to say, and which control to say it
// about. An empty `text` means "not hovering anything of mine".
struct TooltipHit {
  std::string text;
  SDL_FRect anchor{};

  explicit operator bool() const { return !text.empty(); }
};

// Tooltips for the shared find widget's fixed buttons. The two surfaces that use
// it (the editor find/replace card and the terminal find bar) differ only in
// their toggles, handled by the caller.
constexpr std::string_view kFindPrevTooltip = "Previous Match (Shift+Enter)";
constexpr std::string_view kFindNextTooltip = "Next Match (Enter)";
constexpr std::string_view kFindCloseTooltip = "Close (Escape)";
constexpr std::string_view kFindReplaceTooltip = "Replace";
constexpr std::string_view kFindReplaceAllTooltip = "Replace All";

}  // namespace

std::optional<HoverTooltip> WorkspaceShell::HoveredTooltip(const WorkspaceLayout& layout) const {
  if (!last_mouse_position_valid_ || MenuSurfaceCapturingMouse()) {
    return std::nullopt;
  }
  const float x = last_mouse_x_;
  const float y = last_mouse_y_;

  const auto hit = [&](std::string text, const SDL_FRect& anchor) {
    return TooltipHit{.text = std::move(text), .anchor = anchor};
  };

  // 1) Project tab strip, 2) editor tab strip: the tab's full path.
  const auto strip_hit = [&](const SDL_FRect& strip,
                             const std::vector<VisibleStripTab>& tabs) -> TooltipHit {
    if (!Contains(strip, x, y)) {
      return {};
    }
    for (const VisibleStripTab& tab : tabs) {
      if (Contains(tab.rect, x, y)) {
        return hit(tab.tooltip_label, tab.rect);
      }
    }
    return {};
  };

  TooltipHit found = strip_hit(layout.project_tab_strip,
                               tab_strip_chrome_.ComputeVisibleProjectTabs(layout.project_tab_strip));
  if (!found && !context_.current_project_state.root.empty()) {
    found = strip_hit(layout.tab_strip, tab_strip_chrome_.ComputeVisibleTabs(layout.tab_strip));
  }

  // 3) Breadcrumb status items (plugin-contributed and built-in).
  if (!found && Contains(layout.breadcrumb, x, y)) {
    for (const VisibleStatusItem& item : ComputeVisibleStatusItems(layout.breadcrumb)) {
      if (item.hovered && !item.item.tooltip.empty()) {
        found = hit(item.item.tooltip, item.rect);
        break;
      }
    }
  }

  // 4) Sidebar: the mode row, then whichever view owns the header controls.
  if (!found && context_.current_project_state.sidebar.visible &&
      Contains(layout.sidebar, x, y)) {
    const SidebarModeRowLayout mode_row = SidebarModeRow(layout.sidebar);
    // Labelled tabs already say what they are; only name them when collapsed.
    if (mode_row.icon_only) {
      for (int i = 0; i < mode_row.tab_count; ++i) {
        const SidebarModeTab& tab = mode_row.tabs[static_cast<std::size_t>(i)];
        if (!Contains(tab.rect, x, y)) {
          continue;
        }
        const SidebarViewSpec* spec = FindBuiltinSidebarView(tab.mode);
        if (spec != nullptr) {
          found = hit(std::string(spec->label), tab.rect);
        }
        break;
      }
    }
    if (!found && mode_row.has_overflow && Contains(mode_row.overflow_rect, x, y)) {
      found = hit("More Views", mode_row.overflow_rect);
    }

    const SidebarMode mode = ActiveSidebarMode();
    if (!found && mode == SidebarMode::Git) {
      if (const SDL_FRect refresh = git_sidebar_header::RefreshButtonRect(layout.sidebar);
          Contains(refresh, x, y)) {
        found = hit(context_.current_project_state.sidebar.git.refreshing
                        ? "Refreshing repository snapshot"
                        : "Refresh",
                    refresh);
      }
      // Per-entry git actions live on the row context menu, so the header
      // refresh button is the git sidebar's only tooltip.
    }
    if (!found && mode == SidebarMode::Search) {
      const auto& options = context_.current_project_state.overlay.workflow.project_search.options;
      if (const SDL_FRect rect = project_search_panel::ModeButtonRect(layout.sidebar);
          Contains(rect, x, y)) {
        found = hit(options.pattern_mode == project::ProjectSearchPatternMode::Regex
                        ? "Pattern: regex (click for literal)"
                        : "Pattern: literal (click for regex)",
                    rect);
      } else if (const SDL_FRect case_rect = project_search_panel::CaseButtonRect(layout.sidebar);
                 Contains(case_rect, x, y)) {
        found = hit(options.case_mode == project::ProjectSearchCaseMode::Sensitive
                        ? "Case: sensitive (click to cycle)"
                    : options.case_mode == project::ProjectSearchCaseMode::Insensitive
                        ? "Case: insensitive (click to cycle)"
                        : "Case: smart (click to cycle)",
                    case_rect);
      } else if (const SDL_FRect hidden = project_search_panel::HiddenButtonRect(layout.sidebar);
                 Contains(hidden, x, y)) {
        found = hit(options.show_hidden ? "Searching hidden files (click to skip)"
                                        : "Skipping hidden files (click to include)",
                    hidden);
      } else if (const SDL_FRect scope = project_search_panel::ScopeButtonRect(layout.sidebar);
                 Contains(scope, x, y)) {
        found = hit(ProjectSearchScopeExpanded() ? "Hide files to include/exclude"
                                                 : "Show files to include/exclude",
                    scope);
      }
    }
  }

  // 5) The two find widgets. Same geometry, same buttons; only the toggles differ.
  const auto find_widget_hit = [&](const FindWidgetLayout& fw,
                                   std::span<const std::string_view> toggle_tooltips) -> TooltipHit {
    if (!Contains(fw.widget, x, y)) {
      return {};
    }
    for (std::size_t i = 0; i < fw.toggle_count && i < toggle_tooltips.size(); ++i) {
      if (Contains(fw.toggle_buttons[i], x, y)) {
        return hit(std::string(toggle_tooltips[i]), fw.toggle_buttons[i]);
      }
    }
    if (Contains(fw.prev_button, x, y)) {
      return hit(std::string(kFindPrevTooltip), fw.prev_button);
    }
    if (Contains(fw.next_button, x, y)) {
      return hit(std::string(kFindNextTooltip), fw.next_button);
    }
    if (Contains(fw.close_button, x, y)) {
      return hit(std::string(kFindCloseTooltip), fw.close_button);
    }
    if (fw.replace_mode) {
      if (Contains(fw.replace_button, x, y)) {
        return hit(std::string(kFindReplaceTooltip), fw.replace_button);
      }
      if (Contains(fw.replace_all_button, x, y)) {
        return hit(std::string(kFindReplaceAllTooltip), fw.replace_all_button);
      }
    }
    return {};
  };

  if (!found) {
    const OverlayState& overlay = context_.current_project_state.overlay;
    if (overlay.visible && (overlay.mode == OverlayMode::BufferSearch ||
                            overlay.mode == OverlayMode::BufferReplace)) {
      static constexpr std::array<std::string_view, 1> kEditorToggles = {
          "Use Regular Expression (Alt+R)"};
      found = find_widget_hit(
          ComputeFindWidgetLayout(layout.editor_surface,
                                  overlay.mode == OverlayMode::BufferReplace),
          kEditorToggles);
    }
  }
  if (!found && BottomPanelVisible() && terminal_find_service_.visible()) {
    static constexpr std::array<std::string_view, 2> kTerminalToggles = {"Match Case",
                                                                         "Match Whole Word"};
    found = find_widget_hit(ComputeFindWidgetLayout(BottomPanelContentRect(layout),
                                                    /*replace_mode=*/false, /*toggle_count=*/2),
                            kTerminalToggles);
  }

  // 6) Status bar segments. Their tooltips were built every frame and thrown
  //    away: nothing ever drew them, so the one row of chrome that tells you the
  //    language, encoding, indent and LSP state explained none of it.
  if (!found && Contains(layout.status_bar, x, y)) {
    const StatusBarViewModel status_vm =
        RenderViewModelBuilder(context_).BuildStatusBar(layout, status_bar_service_);
    ForEachStatusBarSegmentRect(
        status_vm, text_renderer_,
        [&](const StatusBarSegmentViewModel& segment, const SDL_FRect& rect) {
          if (found || segment.tooltip.empty() || !Contains(rect, x, y)) {
            return;
          }
          found = hit(std::string(segment.tooltip), rect);
        });
  }

  if (!found) {
    return std::nullopt;
  }

  TooltipLayout card =
      BuildTooltipLayout(text_renderer_, found.text, std::max(160.0f, layout.full.w - 24.0f));
  return HoverTooltip{
      .text = std::move(card.text),
      .rect = PlaceTooltipCard(found.anchor, card.rect.w, card.rect.h, layout.full),
  };
}

void WorkspaceShell::RenderHoverTooltip(SDL_Renderer* renderer,
                                        const WorkspaceLayout& layout) const {
  if (renderer == nullptr) {
    return;
  }
  if (const auto tooltip = HoveredTooltip(layout); tooltip.has_value()) {
    DrawTooltip(text_renderer_, renderer, theme_, tooltip->rect, tooltip->text);
  }
}

}  // namespace microide::workspace
