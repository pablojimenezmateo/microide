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
// whole-entry vertical scrolling. Returns the resolved max scroll so the caller
// can feed the wheel handler. Kept a free function so the shell stays thin.
int RenderHelpAboutRows(SDL_Renderer* renderer, const SettingsOverlayViewModel& vm,
                        const render::TextRenderer& text_renderer, const render::Theme& theme) {
  const float content_x = vm.rect.x + 18.0f;
  const float content_right = vm.rect.x + vm.rect.w - 16.0f;
  const float line_height = text_renderer.LineHeight();
  const float help_entry_gap = 6.0f;
  const float help_inner_width = std::max(1.0f, content_right - content_x);
  float label_col = 0.0f;
  for (const HelpAboutRow& row : vm.help_rows) {
    label_col = std::max(label_col, text_renderer.MeasureWidth(row.label));
  }
  label_col = std::clamp(label_col, 100.0f, help_inner_width * 0.40f);
  const float column_gap = 16.0f;
  const float detail_x = content_x + label_col + column_gap;
  const float detail_width = std::max(40.0f, content_right - detail_x);

  const auto for_each_wrapped_line = [&](std::string_view text, float max_width,
                                         const auto& emit_line) {
    if (text.empty()) {
      return;
    }
    std::size_t line_start = 0;
    std::size_t line_end = 0;
    std::size_t word_start = 0;
    while (word_start < text.size()) {
      const std::size_t space = text.find(' ', word_start);
      const std::size_t word_end = space == std::string_view::npos ? text.size() : space;
      const std::string_view candidate = text.substr(line_start, word_end - line_start);
      if (line_start != word_start && text_renderer.MeasureWidth(candidate) > max_width) {
        emit_line(text.substr(line_start, line_end - line_start));
        line_start = word_start;
      }
      line_end = word_end;
      word_start = space == std::string_view::npos ? text.size() : space + 1;
    }
    emit_line(text.substr(line_start, text.size() - line_start));
  };
  const auto help_entry_height = [&](std::string_view detail) {
    int lines = 0;
    for_each_wrapped_line(detail, detail_width, [&](std::string_view) { ++lines; });
    return static_cast<float>(std::max(1, lines)) * line_height + help_entry_gap;
  };

  const float list_top = vm.rect.y + vm.header_rect.h + 10.0f;
  const float list_bottom = vm.rect.y + vm.rect.h - 10.0f;
  const float available_height = std::max(0.0f, list_bottom - list_top);
  const int total_rows = static_cast<int>(vm.help_rows.size());
  float accumulated = 0.0f;
  int fit = 0;
  for (int i = total_rows - 1; i >= 0; --i) {
    accumulated += help_entry_height(vm.help_rows[static_cast<std::size_t>(i)].detail);
    if (accumulated > available_height) {
      break;
    }
    ++fit;
  }
  const int visible_rows = std::max(1, fit);
  const int max_scroll = std::max(0, total_rows - visible_rows);
  const int effective_scroll = std::clamp(vm.scroll_row, 0, max_scroll);

  float y = list_top;
  int entry_index = 0;
  for (const HelpAboutRow& row : vm.help_rows) {
    if (entry_index++ < effective_scroll) {
      continue;
    }
    if (y + line_height > list_bottom) {
      break;
    }
    const std::string label_text = text_renderer.TruncateToWidth(row.label, label_col);
    text_renderer.DrawStringOn(renderer, content_x, y, theme.text_primary, theme.surface_background,
                               label_text);
    float detail_y = y;
    for_each_wrapped_line(row.detail, detail_width, [&](std::string_view line) {
      if (detail_y + line_height <= list_bottom) {
        text_renderer.DrawStringOn(renderer, detail_x, detail_y, theme.text_muted,
                                   theme.surface_background, line);
      }
      detail_y += line_height;
    });
    y = std::max(y + line_height, detail_y) + help_entry_gap;
  }

  if (max_scroll > 0) {
    const float track_h = std::max(1.0f, list_bottom - list_top);
    const SDL_FRect list_area = MakeRect(vm.rect.x, list_top, vm.rect.w, track_h);
    if (const auto geometry = MakeVerticalScrollbarGeometry(
            list_area, static_cast<float>(total_rows), static_cast<float>(visible_rows),
            static_cast<float>(effective_scroll), false);
        geometry.has_value()) {
      DrawScrollbar(renderer, theme, geometry->track, geometry->thumb, false);
    }
  }
  return max_scroll;
}

}  // namespace

void WorkspaceShell::RenderSettingsOverlay(SDL_Renderer* renderer,
                                           const WorkspaceLayout& layout) const {
  const SettingsOverlayViewModel vm =
      RenderViewModelBuilder(context_).BuildSettingsOverlay(layout, settings_overlay_service_);
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
    settings_overlay_max_scroll_row_ = RenderHelpAboutRows(renderer, vm, text_renderer_, theme_);
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
                                  text_renderer_.TruncateToWidth(cat.label, cat.rect.w - 24.0f));
    }
  }

  // --- Right pane: value rows ---
  const bool values_focused = vm.focused_pane == SettingsPane::Values;
  const float pane_bottom = vm.right_pane_rect.y + vm.right_pane_rect.h;
  const float line_height = text_renderer_.LineHeight();
  for (const SettingsRowViewModel& row : vm.rows) {
    if (row.row_rect.y + row.row_rect.h > pane_bottom + 0.5f ||
        row.row_rect.y < vm.right_pane_rect.y - 0.5f) {
      continue;
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
        control_left = control.value_rect.x;
        break;
      case SettingsControlKind::Stepper:
        control_left = control.dec_rect.x;
        break;
      case SettingsControlKind::None:
        control_left = row.row_rect.x + row.row_rect.w - 8.0f;
        break;
    }
    const float text_left = row.resettable ? row.reset_rect.x : control_left;
    const float text_x = row.row_rect.x + 12.0f;
    const float text_width = std::max(40.0f, text_left - 8.0f - text_x);

    text_renderer_.DrawStringOn(renderer, text_x, row.row_rect.y + 6.0f, theme_.text_primary,
                                background, text_renderer_.TruncateToWidth(row.label, text_width));
    if (!row.description.empty()) {
      text_renderer_.DrawStringOn(renderer, text_x, row.row_rect.y + 6.0f + line_height,
                                  theme_.text_muted, background,
                                  text_renderer_.TruncateToWidth(row.description, text_width));
    }
    if (!row.scope_label.empty()) {
      const float scope_w = text_renderer_.MeasureWidth(row.scope_label);
      const float scope_x = text_left - 8.0f - scope_w;
      if (scope_x > text_x + 40.0f) {
        text_renderer_.DrawStringOn(renderer, scope_x, row.row_rect.y + 6.0f + line_height,
                                    theme_.text_disabled, background, row.scope_label);
      }
    }

    if (row.resettable) {
      DrawRect(renderer, row.reset_rect, theme_.border);
      DrawResetGlyph(renderer, row.reset_rect, theme_.text_muted);
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
                           theme_.chrome_background,
                           text_renderer_.TruncateToWidth(control.display_value,
                                                          control.value_rect.w - 12.0f));
        break;
      case SettingsControlKind::Stepper:
        DrawRect(renderer, control.dec_rect, theme_.border);
        DrawStepperArrow(renderer, control.dec_rect, true, theme_.text_primary);
        DrawFilledRect(renderer, control.value_rect, theme_.chrome_background);
        DrawRect(renderer, control.value_rect, theme_.border);
        DrawCenteredTextOn(text_renderer_, renderer, control.value_rect, theme_.accent,
                           theme_.chrome_background,
                           text_renderer_.TruncateToWidth(control.display_value,
                                                          control.value_rect.w - 12.0f));
        DrawRect(renderer, control.inc_rect, theme_.border);
        DrawStepperArrow(renderer, control.inc_rect, false, theme_.text_primary);
        break;
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
}

}  // namespace microide::workspace
