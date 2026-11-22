#include "workspace/render/WorkspaceShellRenderPrimitives.h"
#include "render/ScopedRenderClip.h"

#include "workspace/ProjectSearchPanelLayout.h"
#include "workspace/git/GitSidebarHeaderLayout.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "editor/GutterIconRegistry.h"
#include "workspace/git/CommitWorkflowLayout.h"
#include "workspace/git/CommitWorkflowService.h"
#include "workspace/git/CommitWorkflowState.h"
#include "workspace/git/GitSidebarCommandCenter.h"
#include "workspace/render/GitSidebarPaint.h"
#include "workspace/render/TreeSidebarPaint.h"
#include "workspace/SettingFlags.h"
#include "workspace/git/WorkspaceGitSidebarPresentation.h"
#include "workspace/registries/WorkspaceSidebarRegistry.h"

namespace microide::workspace {

using namespace detail;

namespace {

constexpr float kSidebarInset = 10.0f;
// Frame prep derives the commit-draft body field width from the sidebar width and
// its own copy of this inset (it has no panel layout to read). If the two ever
// drift, the viewport is sized for a different width than the field is drawn at
// and the body's wrap/scroll silently disagrees with what is painted.
static_assert(kSidebarInset == kCommitWorkflowFieldInset,
              "PrepareCommitBodyViewportForFrame sizes the commit body field with "
              "kCommitWorkflowFieldInset; keep it equal to the sidebar inset");
constexpr float kTreeIndentWidth = kWorkspaceSidebarTreeIndentWidth;
constexpr float kTreeChevronSlotWidth = kWorkspaceSidebarTreeChevronSlotWidth;

// Reuse a thread-local scratch so the per-result label assembly is allocation-bounded by max label
// width, not by `results × frames`. The render path only inspects the returned view immediately
// after this call, so the scratch's lifetime is safe.
// Number of decimal digits in `value` (>=1). Used to size the "line:col  "
// prefix when positioning the preview match highlight, without materializing the
// label string (render TUs must avoid std::to_string).
std::size_t DecimalDigitCount(std::size_t value) {
  std::size_t digits = 1;
  while (value >= 10) {
    value /= 10;
    ++digits;
  }
  return digits;
}

std::string_view BuildProjectSearchResultLabel(std::size_t line,
                                                std::size_t column,
                                                std::string_view snippet) {
  thread_local std::string label;
  label.clear();
  label.reserve(snippet.size() + 32);
  AppendUnsigned(label, line + 1);
  label += ":";
  AppendUnsigned(label, column + 1);
  label += "  ";
  label += snippet;
  return label;
}

// An empty list's note: under the list's first row, down to the list's bottom.
SDL_FRect SidebarPlaceholderBox(const SDL_FRect& sidebar, const ScrollableListLayout& list) {
  const float top = list.row_y + 4.0f;
  return MakeRect(sidebar.x + kSidebarInset, top, sidebar.w - kSidebarInset * 2.0f,
                  std::max(0.0f, list.list_rect.y + list.list_rect.h - top));
}

}  // namespace

void WorkspaceShell::RenderSidebarSurface(SDL_Renderer* renderer, const WorkspaceLayout& layout) {
  const SidebarSurfaceViewModel& sidebar_vm = *prepare_cached_sidebar_vm_;
  const TextInputSurfaceViewModel& text_input_vm = *prepare_cached_text_input_vm_;
  ProjectWorkspaceState& project_state = *sidebar_vm.project_state;
  if (!sidebar_vm.visible) {
    return;
  }
  // Nothing the sidebar paints leaves it. Narrowed by a drag, its header buttons
  // and its rows' truncated labels ran past its right edge into the editor -- so
  // a full frame drew them there and a partial frame of the editor painted over
  // them, and the two disagreed (TD-2026-10-20-372, CI's ASan monkey, seed 4).
  const SDL_FRect& sidebar_area = layout.sidebar;
  const render::ScopedRenderClip sidebar_clip(
      renderer, SDL_Rect{static_cast<int>(std::floor(sidebar_area.x)), static_cast<int>(std::floor(sidebar_area.y)),
                         static_cast<int>(std::ceil(sidebar_area.w)), static_cast<int>(std::ceil(sidebar_area.h))});

  const auto draw_vertical_scrollbar = [&](const SDL_FRect& area,
                                           float total_units,
                                           float visible_units,
                                           float scroll_units,
                                           bool active = false,
                                           bool reserve_horizontal = false) {
    if (const auto geometry = MakeVerticalScrollbarGeometry(area, total_units, visible_units,
                                                            scroll_units, reserve_horizontal);
        geometry.has_value()) {
      DrawScrollbar(renderer, theme_, geometry->track, geometry->thumb, active);
    }
  };
  // Mode-switch tab row (Project / Search / Source Control + optional overflow), replacing the
  // old dropdown. Single-click switches view; the row collapses to icon-only when narrow.
  const std::string_view active_view_id = project_state.sidebar.view_id;
  const SidebarModeRowLayout mode_row = SidebarModeRow(layout.sidebar);
  const bool sidebar_mode_open =
      context_.menu_state.menu_bar_open && context_.menu_state.active_menu_id == MenuId::SidebarMode &&
      context_.menu_state.active_menu_anchor_rect.has_value();
  const auto draw_mode_glyph = [&](SidebarMode mode, const SDL_FRect& icon_rect, SDL_Color color) {
    switch (mode) {
      case SidebarMode::Tree: DrawFolderGlyph(renderer, icon_rect, color); break;
      case SidebarMode::Search: DrawSearchGlyph(renderer, icon_rect, color); break;
      case SidebarMode::Git: DrawBranchGlyph(renderer, icon_rect, color); break;
      default: break;
    }
  };
  for (int i = 0; i < mode_row.tab_count; ++i) {
    const SidebarModeTab& tab = mode_row.tabs[static_cast<std::size_t>(i)];
    const SidebarViewSpec* spec = FindBuiltinSidebarView(tab.mode);
    const std::string_view tab_label =
        mode_row.icon_only || spec == nullptr ? std::string_view{} : spec->label;
    DrawModeTab(text_renderer_, renderer, theme_, tab.rect,
                PointerOver(tab.rect),
                tab.id == active_view_id, tab_label,
                [&](const SDL_FRect& icon_rect, SDL_Color color) {
                  draw_mode_glyph(tab.mode, icon_rect, color);
                });
  }
  if (mode_row.has_overflow) {
    const bool hovered =
        PointerOver(mode_row.overflow_rect);
    // Lit whenever the current view lives behind the overflow rather than in a tab
    // (Problems / Tests / Outline / any plugin view), so the rail always shows where
    // you are. Testing "is it a builtin view" instead of "is it one of the drawn
    // tabs" left the row completely unlit in the three builtin overflow views.
    const bool active_view_has_tab =
        std::any_of(mode_row.tabs.begin(), mode_row.tabs.begin() + mode_row.tab_count,
                    [&](const SidebarModeTab& tab) { return tab.id == active_view_id; });
    const bool active = sidebar_mode_open || !active_view_has_tab;
    const ButtonColors colors = ResolveButtonColors(
        theme_, ButtonTone::Neutral,
        ButtonVisualState{.enabled = true, .hovered = hovered, .active = active});
    DrawFilledRect(renderer, mode_row.overflow_rect, colors.fill);
    DrawRect(renderer, mode_row.overflow_rect, colors.border);
    DrawEllipsisGlyph(renderer, mode_row.overflow_rect, colors.text);
  }

  const SidebarMode sidebar_mode = sidebar_vm.mode;
  if (sidebar_mode == SidebarMode::Search) {
    const auto& ps = project_state.overlay.workflow.project_search;
    const TextInputSurface current_surface = text_input_vm.current_surface;
    const bool sidebar_needs_visual = IsSidebarSearchFieldSurface(current_surface);
    const auto visual =
        sidebar_needs_visual ? BuildActiveTextInputVisual(layout, std::nullopt) : std::nullopt;
    const auto sidebar_display_text = [&](TextInputSurface surface,
                                          std::string_view fallback) -> std::string_view {
      if (visual.has_value() && visual->surface == surface && !visual->displayed_text.empty()) {
        return visual->displayed_text;
      }
      return fallback;
    };

    // One draw per search field: query, replace, and (when the scope section is
    // expanded) the include/exclude glob boxes. A collapsed scope field has a
    // zero-sized rect, which the loop skips.
    // Placeholder/committed text per field, indexed the same as the shared field
    // list below so the two never fall out of order.
    const std::string_view fallbacks[] = {
        sidebar_vm.query_fallback_text,
        sidebar_vm.replace_fallback_text,
        sidebar_vm.include_fallback_text,
        sidebar_vm.exclude_fallback_text,
    };
    const auto search_fields =
        project_search_panel::SidebarSearchFieldRects(layout.sidebar, ps.scope_expanded);
    for (std::size_t index = 0; index < search_fields.size(); ++index) {
      const auto& field = search_fields[index];
      if (field.rect.w <= 0.0f) {
        continue;
      }
      DrawTextFieldFrame(renderer, theme_, field.rect, current_surface == field.surface);
      DrawSingleLineTextTail(renderer, field.rect.x + 6.0f, field.rect.y + 3.0f,
                             std::max(1.0f, field.rect.w - 12.0f),
                             ps.editing && ps.edit_field == field.field ? theme_.text_primary
                                                                       : theme_.text_secondary,
                             theme_.surface_background,
                             sidebar_display_text(field.surface, fallbacks[index]));
    }
    const auto draw_search_button = [&](const SDL_FRect& rect,
                                        std::string_view label,
                                        bool active) {
      DrawButtonCentered(
          text_renderer_, renderer, theme_, rect, label, ButtonTone::Neutral,
          ButtonVisualState{
              .enabled = true,
              .hovered = PointerOver(rect),
              .active = active,
          });
    };

    draw_search_button(project_search_panel::ModeButtonRect(layout.sidebar), ProjectSearchModeButtonLabel(),
                       project_state.overlay.workflow.project_search.options.pattern_mode ==
                           project::ProjectSearchPatternMode::Regex);
    draw_search_button(project_search_panel::CaseButtonRect(layout.sidebar), ProjectSearchCaseButtonLabel(),
                       project_state.overlay.workflow.project_search.options.case_mode !=
                           project::ProjectSearchCaseMode::Smart);
    draw_search_button(project_search_panel::HiddenButtonRect(layout.sidebar),
                       ProjectSearchHiddenButtonLabel(),
                       project_state.overlay.workflow.project_search.options.show_hidden);
    DrawButtonCentered(text_renderer_, renderer, theme_,
                       project_search_panel::ReplaceAllButtonRect(layout.sidebar), "All",
                       ButtonTone::Neutral,
                       ButtonVisualState{
                           .enabled = !ps.query.text().empty(),
                           .hovered = PointerOver(project_search_panel::ReplaceAllButtonRect(layout.sidebar)),
                           .active = false,
                       });
    draw_search_button(project_search_panel::ScopeButtonRect(layout.sidebar),
                       // "..." mirrors VS Code's show/hide-scope affordance; a
                       // constant label needs no shell accessor.
                       "\xE2\x80\xA6", ps.scope_expanded);

    // The status/hint line is composed once (and cached) in
    // RenderViewModelBuilder::BuildSidebarSurface; draw the prebuilt view.
    DrawTextOn(text_renderer_, renderer, layout.sidebar.x + kSidebarInset,
               layout.sidebar.y + project_search_panel::StatusTop(ps.scope_expanded), theme_.text_muted,
               theme_.surface_background,
               TruncateLabelView(sidebar_vm.project_search_status_text,
                                 layout.sidebar.w - kSidebarInset * 2.0f));

    const auto line_map = BuildProjectSearchLineMap();
    const auto list_layout = ComputeProjectSearchSidebarListLayout(layout.sidebar, line_map.size());
    // Honor the stored (clamped) scroll position so mouse-wheel/scrollbar scrolling
    // lets the user scroll freely past the selected row. The selected row is pulled
    // into view only on navigation (see MoveProjectSearchSelection), matching how
    // the git/problems/tests sidebars behave.
    const int scroll_row = list_layout.scroll_row;
    project_state.sidebar.scroll_row = scroll_row;

    for (int row = 0; row < list_layout.visible_rows; ++row) {
      const int line_index = scroll_row + row;
      if (line_index >= static_cast<int>(line_map.size())) {
        break;
      }

      SDL_FRect row_rect = ScrollableListRowRect(list_layout, row);
      const int result_index = line_map[static_cast<std::size_t>(line_index)];
      if (result_index < 0) {
        const std::size_t next_result_index = static_cast<std::size_t>(
            std::min(line_index + 1, static_cast<int>(line_map.size()) - 1));
        const auto& file_result =
            project_state.overlay.workflow.project_search
                .results[static_cast<std::size_t>(line_map[next_result_index])];
        DrawVCenteredTextOn(text_renderer_, renderer, row_rect, 4.0f, theme_.text_primary,
                            theme_.surface_background,
                            TruncateLabelView(std::string_view(file_result.relative_path_string),
                                          row_rect.w - 8.0f));
        continue;
      }

      const auto& result =
          project_state.overlay.workflow.project_search.results[static_cast<std::size_t>(result_index)];
      const bool selected =
          static_cast<std::size_t>(result_index) ==
          project_state.overlay.workflow.project_search.selected_index;
      const bool emphasized = selected || PointerOver(row_rect);
      DrawSelectableRowBackground(renderer, theme_, row_rect, theme_.surface_background, emphasized,
                                  selected);

      // Highlight the matched span inside the preview with the editor's search
      // match colour. Positions assume the monospace sidebar font; non-ASCII
      // previews degrade to a slight horizontal offset.
      if (result.match_preview_length > 0) {
        const float char_width = std::max(1.0f, text_renderer_.CharWidth());
        const std::size_t prefix_chars = DecimalDigitCount(result.line + 1) + 1 +
                                         DecimalDigitCount(result.column + 1) + 2;
        const float text_x = row_rect.x + 6.0f;
        const float text_right = row_rect.x + row_rect.w - 6.0f;
        const float highlight_x =
            text_x +
            static_cast<float>(prefix_chars + result.match_preview_start) * char_width;
        const float highlight_w = static_cast<float>(result.match_preview_length) * char_width;
        if (highlight_x < text_right && highlight_w > 0.0f) {
          FillRect(renderer,
                   MakeRect(highlight_x, row_rect.y + 2.0f,
                            std::min(highlight_w, text_right - highlight_x), row_rect.h - 4.0f),
                   theme_.search_match);
        }
      }

      const std::string_view label =
          BuildProjectSearchResultLabel(result.line, result.column, result.preview);
      DrawVCenteredTextOn(text_renderer_, renderer, row_rect, 6.0f,
                          emphasized ? theme_.text_primary : theme_.text_secondary,
                          emphasized ? theme_.row_highlight : theme_.surface_background,
                          TruncateLabelView(label, row_rect.w - 12.0f));
    }

    // Prebuilt by RenderViewModelBuilder (CachedProjectSearchEmptyText), like the
    // plugin sidebar's placeholder and this panel's own status line. Composing it
    // here cost two heap allocations on every repaint of a searching panel.
    if (line_map.empty() && !sidebar_vm.project_search_empty_text.empty()) {
      DrawWrappedPlaceholderIn(
          text_renderer_, renderer, SidebarPlaceholderBox(layout.sidebar, list_layout),
          sidebar_vm.project_search_empty_is_error ? theme_.diff_deleted : theme_.text_muted,
          theme_.surface_background, sidebar_vm.project_search_empty_text);
    }

    draw_vertical_scrollbar(list_layout.list_rect, static_cast<float>(line_map.size()),
                            list_layout.visible_units, static_cast<float>(scroll_row),
                            context_.interaction_state.drag_target == DragTarget::SidebarScrollbar);
  } else if (sidebar_mode == SidebarMode::Git) {
    // The whole panel -- the action and branch rows, the Commit button or line, the
    // banners, the open commit draft, and the list's rows and scrollbar -- as frame
    // prep laid it out (PrepareGitPanelFrame), each element in its own PaintElement.
    PaintGitPanel(renderer, frame_state_, theme_, text_renderer_);
  } else if (UsesPluginItemSurface(sidebar_mode) || sidebar_mode == SidebarMode::Problems || sidebar_mode == SidebarMode::Tests) {
    // One two-column list surface for every secondary sidebar view. Outline already
    // reused the plugin item surface; Problems and Tests did not, and because this
    // dispatch ends in an unguarded `else` that paints the file tree, both of them
    // painted the tree's rows under their own header while their hit-testing,
    // keyboard and context menus acted on the entry list nobody could see. Clicking
    // what looked like a file in Problems jumped to a diagnostic.
    const auto& problems = project_state.sidebar.problems;
    const auto& tests = project_state.sidebar.tests;
    const auto& plugin = project_state.sidebar.plugin;

    // Row source per view: same geometry, same painting, three different lists.
    struct SecondaryRow {
      std::string_view primary;
      std::string_view secondary;
      int depth = 0;
      bool collapsible = false;
      bool collapsed = false;
    };
    const std::size_t row_count = sidebar_mode == SidebarMode::Problems ? problems.entries.size()
                                  : sidebar_mode == SidebarMode::Tests  ? tests.entries.size()
                                                                        : plugin.items.size();
    const std::size_t selected_index = sidebar_mode == SidebarMode::Problems
                                           ? problems.selected_index
                                       : sidebar_mode == SidebarMode::Tests ? tests.selected_index
                                                                            : plugin.selected_index;
    const auto row_at = [&](std::size_t index) -> SecondaryRow {
      if (sidebar_mode == SidebarMode::Problems) {
        const auto& entry = problems.entries[index];
        return SecondaryRow{entry.primary_label, entry.detail_label};
      }
      if (sidebar_mode == SidebarMode::Tests) {
        const auto& entry = tests.entries[index];
        return SecondaryRow{entry.label, entry.detail_label};
      }
      const auto& item = plugin.items[index];
      return SecondaryRow{item.label, item.detail, item.depth, item.collapsible, item.collapsed};
    };

    // Every one of these views is laid out from the same origin; each keeps its own
    // accessor so render and hit-test literally call the same function per view.
    const auto list_layout = sidebar_mode == SidebarMode::Problems
                                 ? ComputeProblemsSidebarListLayout(layout.sidebar, row_count)
                             : sidebar_mode == SidebarMode::Tests
                                 ? ComputeTestsSidebarListLayout(layout.sidebar, row_count)
                                 : ComputePluginSidebarListLayout(layout.sidebar, row_count);
    const int scroll_row = list_layout.scroll_row;

    for (int row = 0; row < list_layout.visible_rows; ++row) {
      const int item_index = scroll_row + row;
      if (item_index >= static_cast<int>(row_count)) {
        break;
      }

      const SecondaryRow item = row_at(static_cast<std::size_t>(item_index));
      SDL_FRect row_rect = ScrollableListRowRect(list_layout, row);
      const bool selected = static_cast<std::size_t>(item_index) == selected_index;
      const bool emphasized = selected || PointerOver(row_rect);
      const SDL_Color row_background =
          emphasized ? theme_.row_highlight : theme_.surface_background;
      DrawSelectableRowBackground(renderer, theme_, row_rect, theme_.surface_background, emphasized,
                                  selected);

      const SDL_Color primary_color = emphasized ? theme_.text_primary : theme_.text_secondary;
      const SDL_Color secondary_color = emphasized ? theme_.text_secondary : theme_.text_muted;

      // Tree rows: host-drawn indentation by depth plus a disclosure twisty on
      // collapsible rows. Flat sidebars (depth 0, not collapsible) keep the
      // original left inset so their layout is unchanged.
      const bool tree_row = item.depth > 0 || item.collapsible;
      float label_x = row_rect.x + 6.0f;
      if (tree_row) {
        const float depth_offset = static_cast<float>(item.depth) * kTreeIndentWidth;
        const float tree_x = row_rect.x + 6.0f + depth_offset;
        label_x = tree_x + kTreeChevronSlotWidth + 4.0f;
        if (item.collapsible) {
          DrawChevron(renderer, tree_x, row_rect.y + row_rect.h * 0.5f, !item.collapsed,
                      theme_.text_muted);
        }
      }
      DrawPrimarySecondaryRowText(text_renderer_, renderer, row_rect, label_x,
                                  row_rect.x + row_rect.w - 6.0f, primary_color, secondary_color,
                                  row_background, item.primary, item.secondary, 0.62f);
    }

    // Empty state. Plugin/Outline placeholder text is prebuilt on refresh
    // (SidebarCoordinator::RecomputePluginSidebarPlaceholder); Problems and Tests use
    // static literals that say what would put a row here, matching the debug pane's
    // hints ("No breakpoints — click the editor gutter to add one."). Either way the
    // render path never materializes a string.
    std::string_view placeholder;
    bool placeholder_is_error = false;
    if (sidebar_mode == SidebarMode::Problems) {
      if (problems.entries.empty()) {
        placeholder = "No problems detected in this project.";
      }
    } else if (sidebar_mode == SidebarMode::Tests) {
      if (!tests.error.empty()) {
        placeholder = tests.error;
        placeholder_is_error = true;
      } else if (tests.entries.empty()) {
        placeholder = tests.running ? "Discovering tests\xE2\x80\xA6"
                                    : "No tests discovered — run Discover Tests.";
      }
    } else {
      placeholder = plugin.placeholder;
      placeholder_is_error = plugin.placeholder_is_error;
    }
    if (!placeholder.empty()) {
      DrawWrappedPlaceholderIn(text_renderer_, renderer, SidebarPlaceholderBox(layout.sidebar, list_layout),
                               placeholder_is_error ? theme_.diff_deleted : theme_.text_muted,
                             theme_.surface_background, placeholder);
    }

    draw_vertical_scrollbar(list_layout.list_rect, static_cast<float>(row_count),
                            list_layout.visible_units, static_cast<float>(scroll_row),
                            context_.interaction_state.drag_target == DragTarget::SidebarScrollbar);
  } else {
    // The file tree -- its Collapse and Refresh buttons, its rows and its scrollbar
    // -- as frame prep laid it out (PrepareTreeSidebarFrame), each element in its
    // own PaintElement.
    PaintTreeSidebar(renderer, frame_state_, theme_, text_renderer_);
  }

  if (project_state.surface.focus == FocusTarget::Sidebar) {
    DrawSurfaceFocusRing(renderer, layout.sidebar);
  }
}

}  // namespace microide::workspace
