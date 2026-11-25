#include "workspace/render/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <string_view>

#include "workspace/git/GitSidebarCommandCenter.h"
#include "workspace/git/GitSidebarHeaderLayout.h"
#include "workspace/ProjectSearchPanelLayout.h"
#include "workspace/render/CompareReviewControls.h"
#include "workspace/render/RenderViewModelBuilder.h"
#include "workspace/services/StatusBarService.h"
#include "workspace/registries/WorkspaceSidebarRegistry.h"

namespace microide::workspace {

using namespace detail;

namespace {

struct TooltipHit {
  std::string text;
  SDL_FRect anchor{};

  explicit operator bool() const { return !text.empty(); }
};

constexpr std::string_view kFindPrevTooltip = "Previous Match (Shift+Enter)";
constexpr std::string_view kFindNextTooltip = "Next Match (Enter)";
constexpr std::string_view kFindCloseTooltip = "Close (Escape)";
constexpr std::string_view kFindReplaceTooltip = "Replace";
constexpr std::string_view kFindReplaceAllTooltip = "Replace All";

TooltipHit Hit(std::string_view text, const SDL_FRect& anchor) {
  return TooltipHit{.text = std::string(text), .anchor = anchor};
}

TooltipHit GitSidebarHeaderTooltip(const SDL_FRect& sidebar, const GitSidebarState& git, float x,
                                   float y) {
  if (const SDL_FRect refresh = git_sidebar_header::RefreshButtonRect(sidebar);
      Contains(refresh, x, y)) {
    return Hit(git.refreshing ? "Refreshing repository snapshot" : "Refresh", refresh);
  }
  if (const SDL_FRect sync = git_sidebar_header::SyncButtonRect(sidebar); Contains(sync, x, y)) {
    return TooltipHit{.text = BuildGitSyncButtonTooltip(git), .anchor = sync};
  }
  if (const SDL_FRect branch = git_sidebar_header::BranchButtonRect(sidebar);
      Contains(branch, x, y)) {
    return Hit("Checkout branch", branch);
  }
  if (const SDL_FRect stage_all = git_sidebar_header::StageAllButtonRect(sidebar);
      Contains(stage_all, x, y)) {
    return Hit("Stage All Changes", stage_all);
  }
  if (const SDL_FRect discard_all = git_sidebar_header::DiscardAllButtonRect(sidebar);
      Contains(discard_all, x, y)) {
    return Hit("Discard All Changes", discard_all);
  }
  return {};
}

TooltipHit ProjectSearchPanelTooltip(const SDL_FRect& sidebar,
                                     const project::ProjectSearchOptions& options,
                                     bool scope_expanded, float x, float y) {
  if (const SDL_FRect rect = project_search_panel::ModeButtonRect(sidebar); Contains(rect, x, y)) {
    return Hit(options.pattern_mode == project::ProjectSearchPatternMode::Regex
                   ? "Pattern: regex (click or Alt+R for literal)"
                   : "Pattern: literal (click or Alt+R for regex)",
               rect);
  }
  if (const SDL_FRect rect = project_search_panel::CaseButtonRect(sidebar); Contains(rect, x, y)) {
    return Hit(options.case_mode == project::ProjectSearchCaseMode::Sensitive
                   ? "Case: sensitive (click or Alt+C to cycle)"
               : options.case_mode == project::ProjectSearchCaseMode::Insensitive
                   ? "Case: insensitive (click or Alt+C to cycle)"
                   : "Case: smart (click or Alt+C to cycle)",
               rect);
  }
  if (const SDL_FRect rect = project_search_panel::HiddenButtonRect(sidebar); Contains(rect, x, y)) {
    return Hit(options.show_hidden ? "Searching hidden files (click or Alt+H to skip)"
                                   : "Skipping hidden files (click or Alt+H to include)",
               rect);
  }
  if (const SDL_FRect rect = project_search_panel::ScopeButtonRect(sidebar); Contains(rect, x, y)) {
    return Hit(scope_expanded ? "Hide files to include/exclude" : "Show files to include/exclude",
               rect);
  }
  if (const SDL_FRect rect = project_search_panel::ReplaceAllButtonRect(sidebar);
      Contains(rect, x, y)) {
    return Hit("Replace All (Ctrl+Alt+Enter in the replace field)", rect);
  }
  return {};
}

TooltipHit FindWidgetTooltip(const FindWidgetLayout& fw,
                             std::span<const std::string_view> toggle_tooltips, float x, float y) {
  if (!Contains(fw.widget, x, y)) {
    return {};
  }
  for (std::size_t i = 0; i < fw.toggle_count && i < toggle_tooltips.size(); ++i) {
    if (Contains(fw.toggle_buttons[i], x, y)) {
      return Hit(toggle_tooltips[i], fw.toggle_buttons[i]);
    }
  }
  if (Contains(fw.prev_button, x, y)) {
    return Hit(kFindPrevTooltip, fw.prev_button);
  }
  if (Contains(fw.next_button, x, y)) {
    return Hit(kFindNextTooltip, fw.next_button);
  }
  if (Contains(fw.close_button, x, y)) {
    return Hit(kFindCloseTooltip, fw.close_button);
  }
  if (fw.replace_mode && Contains(fw.replace_button, x, y)) {
    return Hit(kFindReplaceTooltip, fw.replace_button);
  }
  if (fw.replace_mode && Contains(fw.replace_all_button, x, y)) {
    return Hit(kFindReplaceAllTooltip, fw.replace_all_button);
  }
  return {};
}

}

std::optional<HoverTooltip> WorkspaceShell::HoveredTooltip(const WorkspaceLayout& layout) const {
  if (!last_mouse_position_valid_ || MenuSurfaceCapturingMouse() ||
      settings_overlay_service_.Visible() || context_.prompts.surface_visible ||
      context_.prompts.dirty_visible) {
    return std::nullopt;
  }
  if (context_.interaction_state.tab_drag.dragging) {
    return std::nullopt;
  }
  const float x = last_mouse_x_;
  const float y = last_mouse_y_;

  const auto strip_hit = [&](const SDL_FRect& strip, auto&& compute_tabs) -> TooltipHit {
    if (!Contains(strip, x, y)) {
      return {};
    }
    for (const VisibleStripTab& tab : compute_tabs()) {
      if (Contains(tab.rect, x, y)) {
        return Hit(tab.tooltip_label, tab.rect);
      }
    }
    return {};
  };

  TooltipHit found =
      strip_hit(layout.project_tab_strip, [&]() -> const std::vector<VisibleStripTab>& {
        return tab_strip_chrome_.ComputeVisibleProjectTabs(layout.project_tab_strip);
      });
  if (!found && !context_.current_project_state.root.empty()) {
    const EditorGroupRectsLayout rects = ComputeEditorGroupRectsForState(layout);
    for (std::size_t group = 0; group < rects.groups.size() && !found; ++group) {
      const SDL_FRect& strip = rects.groups[group].tab_strip;
      found = strip_hit(strip, [&]() -> const std::vector<VisibleStripTab>& {
        return tab_strip_chrome_.ComputeVisibleTabsForGroup(group, strip);
      });
    }
  }

  if (!found && Contains(layout.breadcrumb, x, y)) {
    for (const VisibleStatusItem& item : ComputeVisibleStatusItems(layout.breadcrumb)) {
      if (item.hovered && !item.item.tooltip.empty()) {
        found = Hit(item.item.tooltip, item.rect);
        break;
      }
    }
  }

  if (!found && context_.current_project_state.sidebar.visible &&
      Contains(layout.sidebar, x, y)) {
    const SidebarModeRowLayout mode_row = SidebarModeRow(layout.sidebar);
    if (mode_row.icon_only) {
      for (int i = 0; i < mode_row.tab_count; ++i) {
        const SidebarModeTab& tab = mode_row.tabs[static_cast<std::size_t>(i)];
        if (!Contains(tab.rect, x, y)) {
          continue;
        }
        const SidebarViewSpec* spec = FindBuiltinSidebarView(tab.mode);
        if (spec != nullptr) {
          found = Hit(spec->label, tab.rect);
        }
        break;
      }
    }
    if (!found && mode_row.has_overflow && Contains(mode_row.overflow_rect, x, y)) {
      found = Hit("More Views", mode_row.overflow_rect);
    }

    const SidebarMode mode = ActiveSidebarMode();
    if (!found && mode == SidebarMode::Git) {
      found = GitSidebarHeaderTooltip(layout.sidebar, context_.current_project_state.sidebar.git, x, y);
    }
    if (!found && mode == SidebarMode::Search) {
      found = ProjectSearchPanelTooltip(
          layout.sidebar, context_.current_project_state.overlay.workflow.project_search.options,
          ProjectSearchScopeExpanded(), x, y);
    }
  }

  if (!found) {
    const OverlayState& overlay = context_.current_project_state.overlay;
    if (overlay.visible && (overlay.mode == OverlayMode::BufferSearch ||
                            overlay.mode == OverlayMode::BufferReplace)) {
      static constexpr std::array<std::string_view, kBufferFindToggleCount> kEditorToggles = {
          "Match Case (Alt+C)",
          "Match Whole Word (Alt+W)",
          "Use Regular Expression (Alt+R)",
          "Find in Selection (Alt+L)",
      };
      found = FindWidgetTooltip(
          ComputeBufferFindWidgetLayout(ActiveTabSurface(layout),
                                        overlay.mode == OverlayMode::BufferReplace),
          kEditorToggles, x, y);
    }
  }
  if (!found && BottomPanelVisible()) {
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                 kWorkspaceBottomPanelHeaderHeight);
    if (Contains(panel_header, x, y)) {
      const LayoutMode mode = layout_mode_service_.CurrentMode();
      if (const SDL_FRect rect = tab_strip_service_.BottomPanelTerminalNewTabRect(mode, panel_header);
          Contains(rect, x, y)) {
        found = Hit("New Terminal (Ctrl+Shift+`)", rect);
      } else if (const SDL_FRect maximize =
                     tab_strip_service_.BottomPanelMaximizeButtonRect(mode, panel_header);
                 Contains(maximize, x, y)) {
        found = Hit(layout.panel_maximized ? "Restore Panel Size (Ctrl+Shift+J)"
                                           : "Maximize Panel (Ctrl+Shift+J)",
                    maximize);
      }
    }
  }
  if (!found && BottomPanelVisible() && terminal_find_service_.visible()) {
    static constexpr std::array<std::string_view, kTerminalFindToggleCount> kTerminalToggles = {
        "Match Case (Alt+C)", "Match Whole Word (Alt+W)"};
    found = FindWidgetTooltip(
        ComputeTerminalFindWidgetLayout(ActiveTerminalBodyRect(layout), layout), kTerminalToggles,
        x, y);
  }

  if (!found && DebugToolbarVisible()) {
    const DebugToolbarLayout tb = ComputeDebugToolbarLayout(
        ActiveTabSurface(layout), DebugToolbarAvoidBelowY(layout), debug_service_.SupportsStepBack());
    if (Contains(tb.widget, x, y)) {
      const bool session_stopped = IsDebugSessionStopped();
      for (std::size_t i = 0; i < tb.button_count && !found; ++i) {
        if (!Contains(tb.buttons[i], x, y)) {
          continue;
        }
        found = Hit(DebugToolbarButtonTooltip(tb.kinds[i], session_stopped),
                    tb.buttons[i]);
      }
    }
  }

  if (!found && Contains(layout.status_bar, x, y)) {
    const StatusBarViewModel status_vm =
        RenderViewModelBuilder(context_).BuildStatusBar(layout, status_bar_service_);
    ForEachStatusBarSegmentRect(
        status_vm, text_renderer_,
        [&](const StatusBarSegmentViewModel& segment, const SDL_FRect& rect) {
          if (found || segment.tooltip.empty() || !Contains(rect, x, y)) {
            return;
          }
          found = Hit(segment.tooltip, rect);
        });
  }

  if (!found && ActiveTabIsCompare()) {
    const SDL_FRect view_surface = ActiveTabSurface(layout);
    if (const CompareTabState* compare_tab = ActiveCompareTab();
        compare_tab != nullptr && Contains(view_surface, x, y)) {
      if (const std::optional<CompareClippedNote> note = CompareClippedNoteAt(
              editor_text_renderer_,
              MakeCompareControlGeometry(view_surface, ComputeCompareSurfaceLayout(view_surface, *compare_tab)),
              *compare_tab, x, y)) {
        found = Hit(note->text, note->block);
      }
    }
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

}
