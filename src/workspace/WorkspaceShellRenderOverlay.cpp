#include "workspace/WorkspaceShellRenderPrimitives.h"

#include "workspace/RenderViewModelBuilder.h"

#include <string>
#include <utility>
#include <vector>

namespace microide::workspace {

using namespace detail;

void WorkspaceShell::RenderOverlaySurface(SDL_Renderer* renderer,
                                          const WorkspaceLayout& layout,
                                          const OverlaySurfaceViewModel& overlay_vm) {
  const OverlayState& overlay_state = *overlay_vm.state;
  ProjectWorkspaceState& project_state = *overlay_vm.project_state;
  if (!overlay_vm.visible) {
    return;
  }

  // Local file search/replace renders as a compact, non-modal floating widget
  // (no backdrop, no match list) — handled entirely by RenderFindWidget.
  if (overlay_vm.mode == OverlayMode::BufferSearch ||
      overlay_vm.mode == OverlayMode::BufferReplace) {
    RenderFindWidget(renderer, layout, overlay_vm);
    return;
  }

  const TextInputSurface current_surface = overlay_vm.current_surface;
  const bool overlay_needs_visual =
      current_surface == TextInputSurface::BufferSearch ||
      current_surface == TextInputSurface::BufferReplaceSearch ||
      current_surface == TextInputSurface::BufferReplaceReplace ||
      current_surface == TextInputSurface::ProjectSearchOverlay ||
      current_surface == TextInputSurface::CommitPicker;
  const auto visual =
      overlay_needs_visual ? BuildActiveTextInputVisual(layout, std::nullopt) : std::nullopt;
  const auto overlay_display_text = [&](TextInputSurface surface,
                                        const std::string& fallback) -> std::string_view {
    if (visual.has_value() && visual->surface == surface && !visual->displayed_text.empty()) {
      return visual->displayed_text;
    }
    return fallback;
  };

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

  // The completion popup anchors to the caret and stays compact: it must not dim the
  // editor (that hides the code being completed) or carry a title bar. Code actions
  // are a centered, titled menu like the other pickers.
  const bool caret_anchored = overlay_vm.mode == OverlayMode::Completion;
  if (!caret_anchored) {
    DrawFilledRect(renderer, layout.editor_area, theme_.overlay_backdrop);
  }
  const SDL_FRect overlay = ComputeOverlayRect(layout.editor_area);
  constexpr float kOverlayInset = 18.0f;
  if (caret_anchored) {
    render::DrawCardFrame(renderer, theme_, overlay, render::CardStyle::Overlay);
  } else {
    DrawTitledCardFrame(renderer, theme_, overlay, 32.0f, CardStyle::Overlay);
  }
  const auto overlay_field_rect = [&](float text_y) {
    return MakeRect(overlay.x + 12.0f, text_y - 4.0f, std::max(0.0f, overlay.w - 24.0f), 18.0f);
  };
  const auto overlay_field_text_y = [&](float row_y) {
    const SDL_FRect field = overlay_field_rect(row_y);
    return field.y + std::floor((field.h - text_renderer_.LineHeight()) * 0.5f);
  };

  ClampOverlayScrollRow(overlay);
  const auto overlay_list_layout = ComputeOverlayListLayout(overlay);
  const auto draw_overlay_row = [&](int row_index, int selected_index, std::string_view label) {
    const bool selected = row_index == selected_index;
    SDL_FRect row = ScrollableListRowRect(overlay_list_layout, row_index);
    // Keyboard selection keeps the accent strip; the pointer hovering a different
    // row still lifts that row's background so the click target is obvious.
    const bool emphasized = selected || PointerOver(row);
    DrawSelectableRowBackground(renderer, theme_, row, theme_.surface_raised, emphasized, selected);
    // Accent left-bar on the selected row, matching the two-column pickers, so the
    // keyboard focus is unmistakable in the finder / completion / code-action lists.
    if (selected) {
      DrawFilledRect(renderer, MakeRect(row.x, row.y + 2.0f, 3.0f, row.h - 4.0f), theme_.accent);
    }
    DrawVCenteredTextOn(text_renderer_, renderer, row, 10.0f,
                        emphasized ? theme_.text_primary : theme_.text_secondary,
                        emphasized ? theme_.row_highlight : theme_.surface_raised,
                        TruncateLabel(label, row.w - 16.0f));
  };

  // Shared two-column fuzzy-picker chrome (title + optional context line + query
  // field + summary/hint + accent-marked rows + scrollbar). Used by the commit/ref
  // picker and the launch-config picker; `rows` carries prebuilt (primary,
  // secondary) display strings so this stays a pure draw.
  const auto draw_two_column_picker =
      [&](TextInputSurface surface, std::string_view title, std::string_view context_label,
          const editor::SingleLineEditor& query, std::string_view summary_line,
          const std::vector<std::pair<std::string_view, std::string_view>>& rows,
          std::size_t selected_index, const std::string& empty_label) {
        const auto draw_picker_row = [&](int row_index, int sel_index, std::string_view primary,
                                         std::string_view secondary) {
          const bool selected = row_index == sel_index;
          SDL_FRect row = ScrollableListRowRect(overlay_list_layout, row_index);
          const bool emphasized = selected || PointerOver(row);
          DrawSelectableRowBackground(renderer, theme_, row, theme_.surface_raised, emphasized,
                                      selected);
          const SDL_Color row_bg = emphasized ? theme_.row_highlight : theme_.surface_raised;
          if (selected) {
            DrawFilledRect(renderer, MakeRect(row.x, row.y + 2.0f, 3.0f, row.h - 4.0f),
                           theme_.accent);
          }
          float secondary_width = 0.0f;
          if (!secondary.empty()) {
            secondary_width = text_renderer_.MeasureWidth(secondary) + 12.0f;
            DrawVCenteredTextOn(
                text_renderer_, renderer,
                MakeRect(row.x + row.w - secondary_width, row.y, secondary_width, row.h), 0.0f,
                theme_.text_muted, row_bg, secondary);
          }
          const float primary_width = std::max(20.0f, row.w - 12.0f - secondary_width);
          DrawVCenteredTextOn(text_renderer_, renderer,
                              MakeRect(row.x + 10.0f, row.y, primary_width, row.h), 0.0f,
                              emphasized ? theme_.text_primary : theme_.text_secondary, row_bg,
                              TruncateLabel(primary, primary_width));
        };

        DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
                   theme_.text_primary, theme_.chrome_background, title);
        if (!context_label.empty()) {
          DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 30.0f,
                     theme_.text_muted, theme_.overlay_background,
                     TruncateLabel(context_label, overlay.w - kOverlayInset * 2.0f));
        }
        const std::string fallback = "> " + query.text();
        DrawTextFieldFrame(renderer, theme_, overlay_field_rect(overlay.y + 52.0f),
                           current_surface == surface);
        DrawSingleLineTextTail(renderer, overlay.x + kOverlayInset,
                               overlay_field_text_y(overlay.y + 52.0f),
                               std::max(1.0f, overlay.w - kOverlayInset * 2.0f),
                               theme_.text_secondary, theme_.surface_background,
                               overlay_display_text(surface, fallback));
        DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 78.0f,
                   theme_.text_muted, theme_.overlay_background,
                   summary_line.empty() ? std::string_view("0 of 0") : summary_line);
        constexpr std::string_view kPickerHint = "↑↓ select · Enter choose · Esc cancel";
        const float hint_x =
            overlay.x + overlay.w - kOverlayInset - text_renderer_.MeasureWidth(kPickerHint);
        DrawTextOn(text_renderer_, renderer, hint_x, overlay.y + 78.0f, theme_.text_disabled,
                   theme_.overlay_background, kPickerHint);
        for (int row = 0; row < overlay_list_layout.visible_rows; ++row) {
          const int item_index = overlay_vm.scroll_row + row;
          if (item_index >= static_cast<int>(rows.size())) {
            break;
          }
          const auto& item = rows[static_cast<std::size_t>(item_index)];
          draw_picker_row(row, static_cast<int>(selected_index) - overlay_vm.scroll_row, item.first,
                          item.second);
        }
        if (rows.empty()) {
          DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset,
                     overlay_list_layout.list_rect.y + 6.0f, theme_.text_muted,
                     theme_.overlay_background, empty_label);
        }
        draw_vertical_scrollbar(overlay_list_layout.list_rect, static_cast<float>(rows.size()),
                                overlay_list_layout.visible_units,
                                static_cast<float>(overlay_vm.scroll_row));
      };

  if (overlay_vm.mode == OverlayMode::ProjectSearch) {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
               theme_.text_primary, theme_.chrome_background, "Project Search");
    const std::string ps_fallback =
        "> " + overlay_state.workflow.project_search.query.text();
    DrawTextFieldFrame(renderer, theme_, overlay_field_rect(overlay.y + 44.0f),
                       current_surface == TextInputSurface::ProjectSearchOverlay);
    DrawSingleLineTextTail(
        renderer, overlay.x + kOverlayInset, overlay_field_text_y(overlay.y + 44.0f),
        std::max(1.0f, overlay.w - kOverlayInset * 2.0f), theme_.text_secondary,
        theme_.surface_background,
        overlay_display_text(TextInputSurface::ProjectSearchOverlay, ps_fallback));
    const std::string progress_suffix =
        overlay_state.workflow.project_search.running
            ? BuildSearchProgressSuffix(overlay_state.workflow.project_search.searched_files,
                                         overlay_state.workflow.project_search.total_files)
            : std::string{};
    const std::string summary =
        (overlay_state.workflow.project_search.results.empty()
             ? FormatEmptyState("results")
         : overlay_state.workflow.project_search.truncated
             ? BuildSelectionSummary(
                   overlay_state.workflow.project_search.selected_index,
                   overlay_state.workflow.project_search.results.size(),
                   " shown (capped)")
             : BuildSelectionSummary(
                   overlay_state.workflow.project_search.selected_index,
                   overlay_state.workflow.project_search.results.size(),
                   " results")) +
        progress_suffix;
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 62.0f,
               theme_.text_muted, theme_.overlay_background, summary);
    // Reused across rows so the per-row label build keeps its capacity instead of
    // reallocating a fresh std::string each iteration.
    std::string label;
    for (int row = 0; row < overlay_list_layout.visible_rows; ++row) {
      const int item_index = overlay_vm.scroll_row + row;
      if (item_index >= static_cast<int>(overlay_state.workflow.project_search.results.size())) {
        break;
      }
      const auto& result =
          overlay_state.workflow.project_search.results[static_cast<std::size_t>(item_index)];
      label.clear();
      if (result.relative_path_string.empty()) {
        label += result.relative_path.string();
      } else {
        label += result.relative_path_string;
      }
      label += ":";
      AppendUnsigned(label, result.line + 1);
      label += ":";
      AppendUnsigned(label, result.column + 1);
      label += "  ";
      label += TruncateLabel(result.preview, overlay.w - 220.0f);
      draw_overlay_row(row,
                       static_cast<int>(overlay_state.workflow.project_search.selected_index) -
                           overlay_vm.scroll_row,
                       label);
    }
  } else if (overlay_vm.mode == OverlayMode::CommitPicker) {
    const ComparePickerState& picker = overlay_state.workflow.compare_picker;
    std::vector<std::pair<std::string_view, std::string_view>> rows;
    rows.reserve(picker.matches.size());
    for (const auto& item : picker.matches) {
      rows.emplace_back(item.primary_label, item.secondary_label);
    }
    draw_two_column_picker(
        TextInputSurface::CommitPicker,
        picker.title.empty() ? std::string_view("Compare against") : picker.title,
        picker.context_label, picker.query, picker.summary_line, rows, picker.selected_index,
        FormatEmptyState("matching refs"));
  } else if (overlay_vm.mode == OverlayMode::LaunchConfigPicker) {
    const LaunchConfigPickerState& picker = overlay_state.workflow.launch_config_picker;
    std::vector<std::pair<std::string_view, std::string_view>> rows;
    rows.reserve(picker.matches.size());
    for (const auto& item : picker.matches) {
      rows.emplace_back(item.primary_label, item.secondary_label);
    }
    draw_two_column_picker(
        TextInputSurface::LaunchConfigPicker,
        picker.title.empty() ? std::string_view("Select Launch Configuration") : picker.title,
        std::string_view{}, picker.query, picker.summary_line, rows, picker.selected_index,
        FormatEmptyState("launch configurations"));
  } else if (overlay_vm.mode == OverlayMode::CommandPalette) {
    const CommandPaletteState& palette = overlay_state.workflow.command_palette;
    std::vector<std::pair<std::string_view, std::string_view>> rows;
    rows.reserve(palette.matches.size());
    for (const auto& item : palette.matches) {
      rows.emplace_back(item.primary_label, item.secondary_label);
    }
    draw_two_column_picker(
        TextInputSurface::CommandPalette,
        palette.title.empty() ? std::string_view("Commands") : palette.title,
        std::string_view{}, palette.query, palette.summary_line, rows, palette.selected_index,
        FormatEmptyState("commands"));
  } else if (overlay_vm.mode == OverlayMode::Completion) {
    if (!overlay_state.workflow.completion.error.empty()) {
      DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
                 theme_.diff_deleted, theme_.overlay_background,
                 TruncateLabel(overlay_state.workflow.completion.error, overlay.w - 36.0f));
    }
    for (int row = 0; row < overlay_list_layout.visible_rows; ++row) {
      const int item_index = overlay_vm.scroll_row + row;
      if (item_index >= static_cast<int>(overlay_state.workflow.completion.items.size())) {
        break;
      }
      const auto& item =
          overlay_state.workflow.completion.items[static_cast<std::size_t>(item_index)];
      const std::string label =
          item.detail.empty() ? item.label : item.label + "  " + item.detail;
      draw_overlay_row(
          row,
          static_cast<int>(overlay_state.workflow.completion.selected_index) -
              overlay_vm.scroll_row,
          TruncateLabel(label, overlay.w - 36.0f));
    }
  } else if (overlay_vm.mode == OverlayMode::CodeActions) {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
               theme_.text_primary, theme_.chrome_background, "Code Actions");
    // "Loading..." / "No code actions available" render in the list area, below the
    // title, so they read as the menu's status rather than colliding with the title.
    if (!overlay_state.workflow.code_actions.error.empty()) {
      DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset,
                 overlay_list_layout.list_rect.y + 6.0f, theme_.text_muted,
                 theme_.overlay_background,
                 TruncateLabel(overlay_state.workflow.code_actions.error, overlay.w - 36.0f));
    }
    for (int row = 0; row < overlay_list_layout.visible_rows; ++row) {
      const int item_index = overlay_vm.scroll_row + row;
      if (item_index >= static_cast<int>(overlay_state.workflow.code_actions.items.size())) {
        break;
      }
      const auto& item =
          overlay_state.workflow.code_actions.items[static_cast<std::size_t>(item_index)];
      draw_overlay_row(
          row,
          static_cast<int>(overlay_state.workflow.code_actions.selected_index) -
              overlay_vm.scroll_row,
          TruncateLabel(item.title, overlay.w - 36.0f));
    }
  } else {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
               theme_.text_primary, theme_.chrome_background, "Find File");
    DrawTextFieldFrame(renderer, theme_, overlay_field_rect(overlay.y + 44.0f),
                       current_surface == TextInputSurface::FileFinder);
    DrawSingleLineTextTail(renderer, overlay.x + kOverlayInset, overlay_field_text_y(overlay.y + 44.0f),
                           std::max(1.0f, overlay.w - kOverlayInset * 2.0f),
                           theme_.text_secondary, theme_.surface_background,
                           "> " + project_state.file_finder.query());

    const auto& results = project_state.file_finder.results();
    for (int row = 0; row < overlay_list_layout.visible_rows; ++row) {
      const int item_index = overlay_vm.scroll_row + row;
      if (item_index >= static_cast<int>(results.size())) {
        break;
      }
      draw_overlay_row(
          row,
          static_cast<int>(project_state.file_finder.selected_index()) - overlay_vm.scroll_row,
          results[static_cast<std::size_t>(item_index)].path_string);
    }
    if (results.empty()) {
      DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 80.0f,
                 theme_.text_muted, theme_.overlay_background, FormatEmptyState("matching files"));
    }
  }

  draw_vertical_scrollbar(overlay_list_layout.list_rect, static_cast<float>(OverlayItemCount()),
                          overlay_list_layout.visible_units,
                          static_cast<float>(overlay_vm.scroll_row),
                         context_.interaction_state.drag_target == DragTarget::OverlayScrollbar);
}

void WorkspaceShell::RenderFindWidget(SDL_Renderer* renderer,
                                      const WorkspaceLayout& layout,
                                      const OverlaySurfaceViewModel& overlay_vm) {
  const OverlayState& overlay_state = *overlay_vm.state;
  const auto& buffer_search = overlay_state.workflow.buffer_search;
  const bool replace_mode = overlay_vm.mode == OverlayMode::BufferReplace;
  const FindWidgetLayout fw = ComputeFindWidgetLayout(layout.editor_surface, replace_mode);
  const TextInputSurface current_surface = overlay_vm.current_surface;

  // Floating card only — no backdrop, so the editor underneath stays visible and
  // editable while the widget floats above it.
  render::DrawCardFrame(renderer, theme_, fw.widget, render::CardStyle::Overlay);

  // A focused field shows the scrolled/truncated caret-relative tail; an
  // unfocused field (editor has focus) shows the query from the start.
  const auto visual = BuildActiveTextInputVisual(layout, std::nullopt);
  const auto field_text = [&](TextInputSurface surface,
                              std::string_view raw) -> std::string_view {
    if (visual.has_value() && visual->surface == surface && !visual->displayed_text.empty()) {
      return visual->displayed_text;
    }
    return raw;
  };
  const auto draw_field = [&](const SDL_FRect& field, bool focused, std::string_view text) {
    DrawTextFieldFrame(renderer, theme_, field, focused);
    const float text_y = field.y + std::floor((field.h - text_renderer_.LineHeight()) * 0.5f);
    DrawSingleLineTextTail(renderer, field.x + 6.0f, text_y, std::max(1.0f, field.w - 12.0f),
                           focused ? theme_.text_primary : theme_.text_secondary,
                           theme_.surface_background, text);
  };

  enum class Icon { Prev, Next, Close };
  const auto icon_button = [&](const SDL_FRect& rect, Icon icon, bool enabled) {
    const ButtonTone tone = icon == Icon::Close ? ButtonTone::Destructive : ButtonTone::Neutral;
    const ButtonColors colors =
        ResolveButtonColors(theme_, tone, ButtonVisualState{.enabled = enabled});
    FillRect(renderer, rect, colors.fill);
    OutlineRect(renderer, rect, colors.border);
    switch (icon) {
      case Icon::Prev:
        DrawArrowGlyph(renderer, rect, /*up=*/true, colors.text);
        break;
      case Icon::Next:
        DrawArrowGlyph(renderer, rect, /*up=*/false, colors.text);
        break;
      case Icon::Close:
        DrawCloseGlyph(renderer, rect, colors.text);
        break;
    }
  };

  const bool search_focused = current_surface == TextInputSurface::BufferSearch ||
                              current_surface == TextInputSurface::BufferReplaceSearch;
  draw_field(fw.search_field, search_focused,
             field_text(replace_mode ? TextInputSurface::BufferReplaceSearch
                                      : TextInputSurface::BufferSearch,
                        buffer_search.query.text()));

  const bool has_matches = !buffer_search.matches.empty();
  const bool has_query = !buffer_search.query.text().empty();
  std::string count;
  if (has_query) {
    if (has_matches) {
      AppendUnsigned(count, buffer_search.selected_index + 1);
      count += "/";
      AppendUnsigned(count, buffer_search.matches.size());
    } else {
      count = "0/0";
    }
  }
  if (!count.empty()) {
    DrawCenteredTextOn(text_renderer_, renderer, fw.count_rect,
                       has_matches ? theme_.text_secondary : theme_.text_muted,
                       theme_.overlay_background, count);
  }

  icon_button(fw.prev_button, Icon::Prev, has_matches);
  icon_button(fw.next_button, Icon::Next, has_matches);
  icon_button(fw.close_button, Icon::Close, true);

  if (replace_mode) {
    draw_field(fw.replace_field, current_surface == TextInputSurface::BufferReplaceReplace,
               field_text(TextInputSurface::BufferReplaceReplace, buffer_search.replace_text.text()));
    DrawButtonCentered(text_renderer_, renderer, theme_, fw.replace_button, "Replace",
                       ButtonTone::Neutral, ButtonVisualState{.enabled = has_matches});
    DrawButtonCentered(text_renderer_, renderer, theme_, fw.replace_all_button, "All",
                       ButtonTone::Neutral, ButtonVisualState{.enabled = has_query});
  }
}

}  // namespace microide::workspace
