#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "workspace/RenderViewModelBuilder.h"
#include "workspace/WorkspaceShellRenderPrimitives.h"

namespace microide::workspace {

using namespace detail;

namespace {

// A small left/right pointing arrowhead for stepper buttons.
void DrawStepperArrow(SDL_Renderer* renderer, const SDL_FRect& rect, bool left, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  const float cx = std::floor(rect.x + rect.w * 0.5f);
  const float cy = std::floor(rect.y + rect.h * 0.5f);
  if (left) {
    SDL_RenderLine(renderer, cx + 3.0f, cy - 4.0f, cx - 3.0f, cy);
    SDL_RenderLine(renderer, cx - 3.0f, cy, cx + 3.0f, cy + 4.0f);
  } else {
    SDL_RenderLine(renderer, cx - 3.0f, cy - 4.0f, cx + 3.0f, cy);
    SDL_RenderLine(renderer, cx + 3.0f, cy, cx - 3.0f, cy + 4.0f);
  }
}

// An "undo / revert" glyph for the reset-to-default affordance.
void DrawResetGlyph(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  const float cx = std::floor(rect.x + rect.w * 0.5f);
  const float cy = std::floor(rect.y + rect.h * 0.5f);
  SDL_RenderLine(renderer, cx - 3.0f, cy - 3.0f, cx + 3.0f, cy - 3.0f);
  SDL_RenderLine(renderer, cx + 3.0f, cy - 3.0f, cx + 3.0f, cy + 3.0f);
  SDL_RenderLine(renderer, cx + 3.0f, cy + 3.0f, cx - 2.0f, cy + 3.0f);
  SDL_RenderLine(renderer, cx - 3.0f, cy - 3.0f, cx - 1.0f, cy - 5.0f);
  SDL_RenderLine(renderer, cx - 3.0f, cy - 3.0f, cx, cy - 1.0f);
}

void DrawFocusRing(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  const SDL_FRect inner = {rect.x + 1.0f, rect.y + 1.0f, rect.w - 2.0f, rect.h - 2.0f};
  SDL_RenderRect(renderer, &inner);
}

// Help / About is a two-column read-only list with word-wrapped detail and
// whole-entry vertical scrolling. Pure paint: the column geometry, the fitted
// row count, the clamped scroll and the scrollbar all arrive on the view model,
// so the wheel handler and the scrollbar grab share this pass's scroll model
// instead of waiting for it to be published back out of paint.
void RenderHelpAboutRows(SDL_Renderer* renderer, const SettingsOverlayViewModel& vm,
                         const render::TextRenderer& text_renderer, const render::Theme& theme,
                         bool scrollbar_active) {
  const float content_x = vm.rect.x + 18.0f;
  const float line_height = text_renderer.LineHeight();
  const float list_top = vm.help_list_rect.y;
  const float list_bottom = vm.help_list_rect.y + vm.help_list_rect.h;

  float y = list_top;
  int entry_index = 0;
  for (const HelpAboutRow& row : vm.help_rows) {
    if (entry_index++ < vm.scroll_row) {
      continue;
    }
    if (y + line_height > list_bottom) {
      break;
    }
    text_renderer.DrawStringOn(
        renderer, content_x, y, theme.text_primary, theme.surface_background,
        text_renderer.TruncateToWidthEphemeralView(row.label, vm.help_label_column));
    float detail_y = y;
    text_renderer.ForEachWrappedLine(row.detail, vm.help_detail_width,
                                     [&](std::string_view line) {
                                       if (detail_y + line_height <= list_bottom) {
                                         text_renderer.DrawStringOn(renderer, vm.help_detail_x,
                                                                    detail_y, theme.text_muted,
                                                                    theme.surface_background, line);
                                       }
                                       detail_y += line_height;
                                     });
    y = std::max(y + line_height, detail_y) + vm.help_entry_gap;
  }

  if (vm.scrollbar.has_value()) {
    DrawScrollbar(renderer, theme, vm.scrollbar->track, vm.scrollbar->thumb, scrollbar_active);
  }
}

}  // namespace

void WorkspaceShell::RenderSettingsOverlay(SDL_Renderer* renderer,
                                           const WorkspaceLayout& layout) const {
  const SettingsOverlayViewModel vm =
      RenderViewModelBuilder(context_).BuildSettingsOverlay(layout, settings_overlay_service_,
                                                            text_renderer_);
  if (!vm.visible) {
    return;
  }

  DrawFilledRect(renderer, vm.rect, theme_.surface_background);
  DrawRect(renderer, vm.rect, theme_.border);
  DrawFilledRect(renderer, vm.header_rect, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(vm.header_rect.x, vm.header_rect.y + vm.header_rect.h - 1.0f,
                          vm.header_rect.w, 1.0f),
                 theme_.border);
  text_renderer_.DrawStringOn(renderer, vm.header_rect.x + 14.0f, vm.header_rect.y + 11.0f,
                              theme_.accent, theme_.chrome_background, vm.title);

  if (vm.mode == SettingsOverlayMode::HelpAbout) {
    RenderHelpAboutRows(renderer, vm, text_renderer_, theme_,
                        context_.interaction_state.drag_target == DragTarget::SettingsScrollbar);
    return;
  }

  // --- Filter bar ---
  {
    const bool focused = vm.focused_pane == SettingsPane::Filter;
    DrawFilledRect(renderer, vm.filter_rect, theme_.editor_background);
    DrawRect(renderer, vm.filter_rect, focused ? theme_.accent : theme_.border);
    const float text_x = vm.filter_rect.x + 8.0f;
    const float text_y = vm.filter_rect.y + 5.0f;
    const std::string_view shown = vm.query_empty ? vm.filter_placeholder : vm.query;
    text_renderer_.DrawStringOn(renderer, text_x, text_y,
                                vm.query_empty ? theme_.text_disabled : theme_.text_primary,
                                theme_.editor_background, shown);
    if (focused) {
      const std::size_t caret = settings_overlay_service_.QueryEditor().caret();
      const std::string_view prefix = vm.query.substr(0, std::min(caret, vm.query.size()));
      const float caret_x = text_x + text_renderer_.MeasureWidth(prefix);
      SDL_SetRenderDrawColor(renderer, theme_.accent.r, theme_.accent.g, theme_.accent.b,
                             theme_.accent.a);
      SDL_RenderLine(renderer, caret_x, vm.filter_rect.y + 4.0f, caret_x,
                     vm.filter_rect.y + vm.filter_rect.h - 4.0f);
    }
  }

  // --- Left rail: categories ---
  DrawFilledRect(renderer,
                 MakeRect(vm.right_pane_rect.x - 1.0f, vm.left_pane_rect.y, 1.0f,
                          vm.left_pane_rect.h),
                 theme_.border);
  {
    const float pane_bottom = vm.left_pane_rect.y + vm.left_pane_rect.h;
    const bool pane_focused = vm.focused_pane == SettingsPane::Categories;
    for (const SettingsCategoryViewModel& cat : vm.categories) {
      // The rail is whole-row scrolled: skip rows above the pane top and stop once a
      // row falls past the bottom (categories are ordered top-to-bottom).
      if (cat.rect.y < vm.left_pane_rect.y - 0.5f) {
        continue;
      }
      if (cat.rect.y + cat.rect.h > pane_bottom + 0.5f) {
        break;
      }
      const SDL_Color background = cat.selected ? theme_.selection_strong : theme_.surface_background;
      if (cat.selected) {
        DrawFilledRect(renderer, cat.rect, background);
        if (pane_focused) {
          DrawFocusRing(renderer, cat.rect, theme_.accent);
        }
      }
      text_renderer_.DrawStringOn(renderer, cat.rect.x + 12.0f, cat.rect.y + 7.0f,
                                  cat.selected ? theme_.text_primary : theme_.text_muted, background,
                                  TruncateLabelView(cat.label, cat.rect.w - 24.0f));
    }
    if (vm.category_scrollbar.has_value()) {
      DrawScrollbar(
          renderer, theme_, vm.category_scrollbar->track, vm.category_scrollbar->thumb,
          context_.interaction_state.drag_target == DragTarget::SettingsCategoryScrollbar);
    }
  }

  // --- Section header band: selected category title + one-line subtitle ---
  const float line_height = text_renderer_.LineHeight();
  if (!vm.section_title.empty()) {
    DrawFilledRect(renderer, vm.section_header_rect, theme_.surface_background);
    const float header_x = vm.section_header_rect.x + 12.0f;
    text_renderer_.DrawStringOn(
        renderer, header_x, vm.section_header_rect.y + 6.0f, theme_.accent,
        theme_.surface_background,
        TruncateLabelView(vm.section_title, vm.section_header_rect.w - 24.0f));
    if (!vm.section_subtitle.empty()) {
      text_renderer_.DrawStringOn(
          renderer, header_x, vm.section_header_rect.y + 6.0f + line_height, theme_.text_muted,
          theme_.surface_background,
          TruncateLabelView(vm.section_subtitle, vm.section_header_rect.w - 24.0f));
    }
    DrawFilledRect(renderer,
                   MakeRect(vm.section_header_rect.x,
                            vm.section_header_rect.y + vm.section_header_rect.h - 1.0f,
                            vm.section_header_rect.w, 1.0f),
                   theme_.border);
  }

  // --- Right pane: value rows ---
  const bool values_focused = vm.focused_pane == SettingsPane::Values;
  const float pane_bottom = vm.right_pane_rect.y + vm.right_pane_rect.h;
  // Scope-chip hover tooltip, captured during the row loop and drawn last so it
  // overlays following rows and the scrollbar.
  std::string_view hovered_scope_help;
  SDL_FRect hovered_scope_rect{};
  // Clear the whole rows area first so switching to a category with fewer rows (e.g.
  // the single-row "Plugins" pane) does not leave the previous category's text behind.
  DrawFilledRect(renderer, vm.right_pane_rect, theme_.surface_background);
  for (const SettingsRowViewModel& row : vm.rows) {
    if (row.row_rect.y + row.row_rect.h > pane_bottom + 0.5f ||
        row.row_rect.y < vm.right_pane_rect.y - 0.5f) {
      continue;
    }
    // Subsection sub-header in the reserved strip above the row (first row of each
    // non-empty subsection). Skip when it would paint into the fixed header band.
    if (!row.group_subheader.empty() &&
        row.row_rect.y - line_height >= vm.right_pane_rect.y - 0.5f) {
      text_renderer_.DrawStringOn(
          renderer, row.row_rect.x + 12.0f, row.row_rect.y - line_height + 3.0f, theme_.text_muted,
          theme_.surface_background,
          TruncateLabelView(row.group_subheader, row.row_rect.w - 24.0f));
    }

    const SDL_Color background = row.selected ? theme_.selection_strong : theme_.surface_background;
    if (row.selected) {
      DrawFilledRect(renderer, row.row_rect, background);
      if (values_focused) {
        DrawFocusRing(renderer, row.row_rect, theme_.accent);
      }
    }

    const SettingsControlViewModel& control = row.control;
    float control_left = row.row_rect.x + row.row_rect.w;
    switch (control.kind) {
      case SettingsControlKind::Checkbox:
        control_left = control.checkbox_rect.x;
        break;
      case SettingsControlKind::Segmented:
      case SettingsControlKind::TextEdit:
        control_left = control.value_rect.x;
        break;
      case SettingsControlKind::Stepper:
        control_left = control.dec_rect.x;
        break;
      case SettingsControlKind::None:
        control_left = row.row_rect.x + row.row_rect.w - 8.0f;
        break;
    }
    float text_left = row.resettable ? row.reset_rect.x : control_left;
    if (row.scope_rect.w > 0.0f) {
      text_left = std::min(text_left, row.scope_rect.x);
    }
    const float text_x = row.row_rect.x + 12.0f;
    const float text_width = std::max(40.0f, text_left - 8.0f - text_x);

    text_renderer_.DrawStringOn(renderer, text_x, row.row_rect.y + 6.0f, theme_.text_primary,
                                background, TruncateLabelView(row.label, text_width));
    // The dim scope label ("User" / "Project") sits at the right end of the first
    // description line; the builder resolved its x (< 0 when absent or it did not fit).
    const float desc_top = row.row_rect.y + 6.0f + line_height;
    if (row.scope_label_x >= 0.0f && !row.scope_label.empty()) {
      text_renderer_.DrawStringOn(renderer, row.scope_label_x, desc_top, theme_.text_disabled,
                                  background, row.scope_label);
    }
    // Full help text, word-wrapped by the builder so every line is visible and the
    // row was sized to fit them all.
    float desc_y = desc_top;
    for (std::string_view desc_line : row.description_lines) {
      text_renderer_.DrawStringOn(renderer, text_x, desc_y, theme_.text_muted, background, desc_line);
      desc_y += line_height;
    }

    if (row.resettable) {
      DrawRect(renderer, row.reset_rect, theme_.border);
      DrawResetGlyph(renderer, row.reset_rect, theme_.text_muted);
    }

    // Per-row scope chip: "Project" (a per-project override is active, drawn in
    // the accent color) or "Default" (following the user-level / spec default).
    if (row.scope_rect.w > 0.0f) {
      const SDL_Color scope_color = row.scope_is_project ? theme_.accent : theme_.text_muted;
      DrawFilledRect(renderer, row.scope_rect, theme_.chrome_background);
      DrawRect(renderer, row.scope_rect, theme_.border);
      DrawCenteredTextOn(text_renderer_, renderer, row.scope_rect, scope_color,
                         theme_.chrome_background,
                         TruncateLabelView(row.scope_text, row.scope_rect.w - 8.0f));
      if (!row.scope_help.empty() && last_mouse_position_valid_ &&
          Contains(row.scope_rect, last_mouse_x_, last_mouse_y_)) {
        hovered_scope_help = row.scope_help;
        hovered_scope_rect = row.scope_rect;
      }
    }

    switch (control.kind) {
      case SettingsControlKind::Checkbox:
        DrawRect(renderer, control.checkbox_rect, theme_.border);
        if (control.checkbox_on) {
          DrawCheckGlyph(renderer, control.checkbox_rect, theme_.accent);
        }
        break;
      case SettingsControlKind::Segmented:
        DrawFilledRect(renderer, control.value_rect, theme_.chrome_background);
        DrawRect(renderer, control.value_rect, theme_.border);
        DrawCenteredTextOn(text_renderer_, renderer, control.value_rect, theme_.accent,
                           theme_.chrome_background, control.shown_value);
        break;
      case SettingsControlKind::Stepper:
        DrawRect(renderer, control.dec_rect, theme_.border);
        DrawStepperArrow(renderer, control.dec_rect, true, theme_.text_primary);
        DrawFilledRect(renderer, control.value_rect, theme_.chrome_background);
        DrawRect(renderer, control.value_rect, theme_.border);
        DrawCenteredTextOn(text_renderer_, renderer, control.value_rect, theme_.accent,
                           theme_.chrome_background, control.shown_value);
        DrawRect(renderer, control.inc_rect, theme_.border);
        DrawStepperArrow(renderer, control.inc_rect, false, theme_.text_primary);
        break;
      case SettingsControlKind::TextEdit: {
        DrawFilledRect(renderer, control.value_rect, theme_.chrome_background);
        DrawRect(renderer, control.value_rect,
                 control.editing ? theme_.accent : theme_.border);
        const float text_x = control.value_rect.x + 6.0f;
        const float text_y = control.value_rect.y + (control.value_rect.h - line_height) * 0.5f;
        // shown_value + placeholder flag + caret offset are precomputed in the view
        // model (TD-2026-07-17A-007), so render neither builds "(default)" nor
        // truncates/measures per paint.
        const SDL_Color text_color =
            control.value_is_placeholder ? theme_.text_disabled : theme_.text_primary;
        text_renderer_.DrawStringOn(renderer, text_x, text_y, text_color,
                                    theme_.chrome_background, control.shown_value);
        if (control.editing) {
          // Static caret bar at the editor caret offset (measured in the builder).
          const float caret_x = text_x + control.caret_offset_x;
          const SDL_FRect caret_rect =
              MakeRect(std::min(caret_x, control.value_rect.x + control.value_rect.w - 2.0f),
                       control.value_rect.y + 3.0f, 1.5f, control.value_rect.h - 6.0f);
          DrawFilledRect(renderer, caret_rect, theme_.accent);
        }
        break;
      }
      case SettingsControlKind::None:
        if (!control.display_value.empty()) {
          const float vw = text_renderer_.MeasureWidth(control.display_value);
          text_renderer_.DrawStringOn(renderer, control_left - vw, row.row_rect.y + 6.0f,
                                      theme_.accent, background, control.display_value);
        }
        break;
    }
  }

  if (vm.scrollbar.has_value()) {
    const bool dragging =
        context_.interaction_state.drag_target == DragTarget::SettingsScrollbar;
    DrawScrollbar(renderer, theme_, vm.scrollbar->track, vm.scrollbar->thumb, dragging);
  }

  if (!hovered_scope_help.empty()) {
    const auto tooltip = BuildWrappedTooltipLayout(text_renderer_, hovered_scope_help,
                                                   std::max(240.0f, vm.rect.w * 0.45f));
    // Prefer below the chip; place above when below would overflow the overlay.
    const float below_y = hovered_scope_rect.y + hovered_scope_rect.h + 6.0f;
    const float above_y = hovered_scope_rect.y - 6.0f - tooltip.rect.h;
    const float y = (below_y + tooltip.rect.h <= vm.rect.y + vm.rect.h - 6.0f || above_y < vm.rect.y)
                        ? below_y
                        : above_y;
    const float x = std::clamp(hovered_scope_rect.x, vm.rect.x + 6.0f,
                               vm.rect.x + vm.rect.w - tooltip.rect.w - 6.0f);
    DrawWrappedTooltip(text_renderer_, renderer, theme_,
                       MakeRect(x, y, tooltip.rect.w, tooltip.rect.h), tooltip.lines);
  }

  // Font-picker dropdown (drawn last so it overlays following rows).
  if (vm.value_picker.visible) {
    const SettingsPickerViewModel& picker = vm.value_picker;
    DrawFilledRect(renderer, picker.rect, theme_.chrome_background);
    DrawRect(renderer, picker.rect, theme_.accent);
    for (const SettingsPickerItemViewModel& item : picker.items) {
      const SDL_Color background =
          item.highlighted ? theme_.selection_strong : theme_.chrome_background;
      if (item.highlighted) {
        DrawFilledRect(renderer, item.rect, background);
      }
      if (item.is_choose_file) {
        // Separator above the pinned "Choose file…" footer.
        DrawFilledRect(renderer, MakeRect(item.rect.x, item.rect.y, item.rect.w, 1.0f),
                       theme_.border);
      }
      const SDL_Color foreground = item.is_choose_file ? theme_.accent : theme_.text_primary;
      const float text_x = item.rect.x + 8.0f;
      const float text_y = item.rect.y + (item.rect.h - line_height) * 0.5f;
      text_renderer_.DrawStringOn(renderer, text_x, text_y, foreground, background,
                                  TruncateLabelView(item.text, item.rect.w - 16.0f));
    }
    if (picker.scrollbar.has_value()) {
      DrawScrollbar(renderer, theme_, picker.scrollbar->track, picker.scrollbar->thumb,
                    context_.interaction_state.drag_target == DragTarget::SettingsPickerScrollbar);
    }
  }
}

}  // namespace microide::workspace
