#include "workspace/shell/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>

#include "render/ScopedRenderClip.h"
#include "workspace/render/RenderViewModelBuilder.h"
#include "workspace/render/WorkspaceShellRenderPrimitives.h"

namespace microide::workspace {

using namespace detail;

namespace {

void DrawStepperArrow(SDL_Renderer* renderer, const SDL_FRect& rect, bool left, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }
  render::SetDrawColor(renderer, color);
  const float cx = std::floor(rect.x + rect.w * 0.5f);
  const float cy = std::floor(rect.y + rect.h * 0.5f);
  if (left) {
    render::RenderClipStableLine(renderer, cx + 3.0f, cy - 4.0f, cx - 3.0f, cy);
    render::RenderClipStableLine(renderer, cx - 3.0f, cy, cx + 3.0f, cy + 4.0f);
  } else {
    render::RenderClipStableLine(renderer, cx - 3.0f, cy - 4.0f, cx + 3.0f, cy);
    render::RenderClipStableLine(renderer, cx + 3.0f, cy, cx - 3.0f, cy + 4.0f);
  }
}

void DrawResetGlyph(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }
  render::SetDrawColor(renderer, color);
  const float cx = std::floor(rect.x + rect.w * 0.5f);
  const float cy = std::floor(rect.y + rect.h * 0.5f);
  render::RenderClipStableLine(renderer, cx - 3.0f, cy - 3.0f, cx + 3.0f, cy - 3.0f);
  render::RenderClipStableLine(renderer, cx + 3.0f, cy - 3.0f, cx + 3.0f, cy + 3.0f);
  render::RenderClipStableLine(renderer, cx + 3.0f, cy + 3.0f, cx - 2.0f, cy + 3.0f);
  render::RenderClipStableLine(renderer, cx - 3.0f, cy - 3.0f, cx - 1.0f, cy - 5.0f);
  render::RenderClipStableLine(renderer, cx - 3.0f, cy - 3.0f, cx, cy - 1.0f);
}

void DrawFocusRing(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) {
  if (renderer == nullptr) {
    return;
  }
  const SDL_FRect inner = {rect.x + 1.0f, rect.y + 1.0f, rect.w - 2.0f, rect.h - 2.0f};
  render::OutlineRect(renderer, inner, color);
}

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
                                         text_renderer.DrawStringOn(
                                             renderer, vm.help_detail_x, detail_y, theme.text_muted,
                                             theme.surface_background,
                                             text_renderer.TruncateToWidthEphemeralView(line, vm.help_detail_width));
                                       }
                                       detail_y += line_height;
                                     });
    y = std::max(y + line_height, detail_y) + vm.help_entry_gap;
  }

  if (vm.scrollbar.has_value()) {
    DrawScrollbar(renderer, theme, vm.scrollbar->track, vm.scrollbar->thumb, scrollbar_active);
  }
}

}

void WorkspaceShell::RenderSettingsOverlay(SDL_Renderer* renderer) const {
  const PreparedFrameState::SettingsCardPaint& card = frame_state_.settings_card;
  const SettingsOverlayViewModel& vm = card.vm;
  if (!vm.visible) {
    return;
  }

  DrawFilledRect(renderer, card.backdrop, theme_.overlay_backdrop);
  DrawFilledRect(renderer, vm.rect, theme_.surface_background);
  DrawRect(renderer, vm.rect, theme_.border);
  DrawFilledRect(renderer, vm.header_rect, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(vm.header_rect.x, vm.header_rect.y + vm.header_rect.h - 1.0f,
                          vm.header_rect.w, 1.0f),
                 theme_.border);
  text_renderer_.DrawStringOn(renderer, vm.header_rect.x + 14.0f, vm.header_rect.y + 11.0f,
                              theme_.accent, theme_.chrome_background, vm.title);

  {
    const bool focused = vm.focused_pane == SettingsPane::Filter;
    DrawFilledRect(renderer, vm.filter_rect, theme_.editor_background);
    DrawRect(renderer, vm.filter_rect, focused ? theme_.accent : theme_.border);
    const float text_x = vm.filter_rect.x + 8.0f;
    const float text_y = vm.filter_rect.y + 5.0f;
    const std::string_view shown = vm.query_empty ? vm.filter_placeholder : vm.query_display();
    text_renderer_.DrawStringOn(renderer, text_x, text_y,
                                vm.query_empty ? theme_.text_disabled : theme_.text_primary,
                                theme_.editor_background, shown);
    if (focused) {
      const float caret_x = text_x + vm.query_caret_x;
      render::SetDrawColor(renderer, theme_.accent);
      render::RenderClipStableLine(renderer, caret_x, vm.filter_rect.y + 4.0f, caret_x,
                     vm.filter_rect.y + vm.filter_rect.h - 4.0f);
    }
  }

  {
    DrawFilledRect(renderer, vm.footer_rect, theme_.chrome_background);
    DrawFilledRect(renderer, MakeRect(vm.footer_rect.x, vm.footer_rect.y, vm.footer_rect.w, 1.0f),
                   theme_.border);
    const float text_y = vm.footer_rect.y + 5.0f;
    text_renderer_.DrawStringOn(renderer, vm.footer_rect.x + 14.0f, text_y, theme_.text_muted,
                                theme_.chrome_background, vm.footer_summary);
    const float hint_w = text_renderer_.MeasureWidth(vm.footer_hint);
    const float hint_x = vm.footer_rect.x + vm.footer_rect.w - 14.0f - hint_w;
    if (hint_x > vm.footer_rect.x + 14.0f + text_renderer_.MeasureWidth(vm.footer_summary) + 12.0f) {
      text_renderer_.DrawStringOn(renderer, hint_x, text_y, theme_.text_disabled,
                                  theme_.chrome_background, vm.footer_hint);
    }
  }

  if (!vm.empty_label.empty()) {
    text_renderer_.DrawStringOn(renderer, vm.rect.x + 18.0f,
                                vm.filter_rect.y + vm.filter_rect.h + 12.0f, theme_.text_muted,
                                theme_.surface_background, vm.empty_label);
    return;
  }

  if (vm.mode == SettingsOverlayMode::HelpAbout) {
    RenderHelpAboutRows(renderer, vm, text_renderer_, theme_, card.scrollbar_active);
    return;
  }

  DrawFilledRect(renderer,
                 MakeRect(vm.right_pane_rect.x - 1.0f, vm.left_pane_rect.y, 1.0f,
                          vm.left_pane_rect.h),
                 theme_.border);
  {
    const bool pane_focused = vm.focused_pane == SettingsPane::Categories;
    for (std::size_t index = 0; index < vm.categories.size(); ++index) {
      const SettingsCategoryViewModel& cat = vm.categories[index];
      if (!SettingsRowShownIn(vm.left_pane_rect, cat.rect)) {
        continue;
      }
      const bool hovered = static_cast<int>(index) == card.hovered_category;
      const SDL_Color background = cat.selected  ? theme_.selection_strong
                                   : hovered     ? theme_.row_highlight
                                                 : theme_.surface_background;
      if (cat.selected || hovered) {
        DrawFilledRect(renderer, cat.rect, background);
        if (cat.selected && pane_focused) {
          DrawFocusRing(renderer, cat.rect, theme_.accent);
        }
      }
      text_renderer_.DrawStringOn(renderer, cat.rect.x + 12.0f, cat.rect.y + 7.0f,
                                  cat.selected ? theme_.text_primary : theme_.text_muted, background,
                                  TruncateLabelView(cat.label, cat.rect.w - 24.0f));
    }
    if (vm.category_scrollbar.has_value()) {
      DrawScrollbar(renderer, theme_, vm.category_scrollbar->track, vm.category_scrollbar->thumb,
                    card.category_scrollbar_active);
    }
  }

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

  const bool values_focused = vm.focused_pane == SettingsPane::Values;
  DrawFilledRect(renderer, vm.right_pane_rect, theme_.surface_background);
  std::optional<render::ScopedRenderClip> rows_clip;
  rows_clip.emplace(renderer, SDL_Rect{static_cast<int>(std::floor(vm.right_pane_rect.x)),
                                       static_cast<int>(std::floor(vm.right_pane_rect.y)),
                                       static_cast<int>(std::ceil(vm.right_pane_rect.w)),
                                       static_cast<int>(std::ceil(vm.right_pane_rect.h))});
  for (std::size_t row_index = 0; row_index < vm.rows.size(); ++row_index) {
    const SettingsRowViewModel& row = vm.rows[row_index];
    if (!SettingsRowShownIn(vm.right_pane_rect, row.row_rect)) {
      continue;
    }
    if (!row.group_subheader.empty() &&
        row.row_rect.y - line_height >= vm.right_pane_rect.y - 0.5f) {
      text_renderer_.DrawStringOn(
          renderer, row.row_rect.x + 12.0f, row.row_rect.y - line_height + 3.0f, theme_.text_muted,
          theme_.surface_background,
          TruncateLabelView(row.group_subheader, row.row_rect.w - 24.0f));
    }

    const bool hovered = static_cast<int>(row_index) == card.hovered_row;
    const SDL_Color background = row.selected ? theme_.selection_strong
                                 : hovered    ? theme_.row_highlight
                                              : theme_.surface_background;
    if (row.selected || hovered) {
      DrawFilledRect(renderer, row.row_rect, background);
      if (row.selected && values_focused) {
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
    const float desc_top = row.row_rect.y + 6.0f + line_height;
    if (row.scope_label_x >= 0.0f && !row.scope_label.empty()) {
      text_renderer_.DrawStringOn(renderer, row.scope_label_x, desc_top, theme_.text_disabled,
                                  background, row.scope_label);
    }
    float desc_y = desc_top;
    for (std::string_view desc_line : row.description_lines) {
      text_renderer_.DrawStringOn(renderer, text_x, desc_y, theme_.text_muted, background, desc_line);
      desc_y += line_height;
    }

    if (row.resettable) {
      DrawRect(renderer, row.reset_rect, theme_.border);
      DrawResetGlyph(renderer, row.reset_rect, theme_.text_muted);
    }

    if (row.scope_rect.w > 0.0f) {
      const SDL_Color scope_color = row.scope_is_project ? theme_.accent : theme_.text_muted;
      DrawFilledRect(renderer, row.scope_rect, theme_.chrome_background);
      DrawRect(renderer, row.scope_rect, theme_.border);
      DrawCenteredTextOn(text_renderer_, renderer, row.scope_rect, scope_color,
                         theme_.chrome_background,
                         TruncateLabelView(row.scope_text, row.scope_rect.w - 8.0f));
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
        const SDL_Color text_color =
            control.value_is_placeholder ? theme_.text_disabled : theme_.text_primary;
        text_renderer_.DrawStringOn(renderer, text_x, text_y, text_color,
                                    theme_.chrome_background, control.shown_value);
        if (control.editing) {
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
  rows_clip.reset();

  if (vm.scrollbar.has_value()) {
    DrawScrollbar(renderer, theme_, vm.scrollbar->track, vm.scrollbar->thumb, card.scrollbar_active);
  }

  if (card.tooltip_visible) {
    DrawWrappedTooltip(text_renderer_, renderer, theme_, card.tooltip_rect, card.tooltip_lines);
  }

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
                    card.picker_scrollbar_active);
    }
  }
}

}
