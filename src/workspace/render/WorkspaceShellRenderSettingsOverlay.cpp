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


struct SettingsPainter {
  SDL_Renderer* renderer;
  const render::TextRenderer& text;
  const render::Theme& theme;

  void Fill(const SDL_FRect& rect, SDL_Color color) const { render::FillRect(renderer, rect, color); }
  void Outline(const SDL_FRect& rect, SDL_Color color) const { render::OutlineRect(renderer, rect, color); }
  std::string_view Truncate(std::string_view label, float width) const {
    return text.TruncateToWidthEphemeralView(label, width);
  }
};

void PaintSettingsFrame(const SettingsPainter& p, const PreparedFrameState::SettingsCardPaint& card) {
  const SettingsOverlayViewModel& vm = card.vm;
  const render::Theme& theme = p.theme;
  p.Fill(card.backdrop, theme.overlay_backdrop);
  p.Fill(vm.rect, theme.surface_background);
  p.Outline(vm.rect, theme.border);
  p.Fill(vm.header_rect, theme.chrome_background);
  p.Fill(MakeRect(vm.header_rect.x, vm.header_rect.y + vm.header_rect.h - 1.0f, vm.header_rect.w, 1.0f),
         theme.border);
  p.text.DrawStringOn(p.renderer, vm.header_rect.x + 14.0f, vm.header_rect.y + 11.0f, theme.accent,
                      theme.chrome_background, vm.title);
  const bool focused = vm.focused_pane == SettingsPane::Filter;
  p.Fill(vm.filter_rect, theme.editor_background);
  p.Outline(vm.filter_rect, focused ? theme.accent : theme.border);
  const float filter_x = vm.filter_rect.x + 8.0f;
  const float filter_y = vm.filter_rect.y + 5.0f;
  const std::string_view shown = vm.query_empty ? vm.filter_placeholder : vm.query_display();
  p.text.DrawStringOn(p.renderer, filter_x, filter_y, vm.query_empty ? theme.text_disabled : theme.text_primary,
                      theme.editor_background, shown);
  if (focused) {
    const float caret_x = filter_x + vm.query_caret_x;
    render::SetDrawColor(p.renderer, theme.accent);
    render::RenderClipStableLine(p.renderer, caret_x, vm.filter_rect.y + 4.0f, caret_x,
                                 vm.filter_rect.y + vm.filter_rect.h - 4.0f);
  }
  p.Fill(vm.footer_rect, theme.chrome_background);
  p.Fill(MakeRect(vm.footer_rect.x, vm.footer_rect.y, vm.footer_rect.w, 1.0f), theme.border);
  const float footer_y = vm.footer_rect.y + 5.0f;
  p.text.DrawStringOn(p.renderer, vm.footer_rect.x + 14.0f, footer_y, theme.text_muted, theme.chrome_background,
                      vm.footer_summary);
  const float hint_w = p.text.MeasureWidth(vm.footer_hint);
  const float hint_x = vm.footer_rect.x + vm.footer_rect.w - 14.0f - hint_w;
  if (hint_x > vm.footer_rect.x + 14.0f + p.text.MeasureWidth(vm.footer_summary) + 12.0f) {
    p.text.DrawStringOn(p.renderer, hint_x, footer_y, theme.text_disabled, theme.chrome_background, vm.footer_hint);
  }
}

void PaintSettingsCategories(const SettingsPainter& p, const PreparedFrameState::SettingsCardPaint& card) {
  const SettingsOverlayViewModel& vm = card.vm;
  const render::Theme& theme = p.theme;
  p.Fill(MakeRect(vm.right_pane_rect.x - 1.0f, vm.left_pane_rect.y, 1.0f, vm.left_pane_rect.h), theme.border);
  const bool pane_focused = vm.focused_pane == SettingsPane::Categories;
  for (std::size_t index = 0; index < vm.categories.size(); ++index) {
    const SettingsCategoryViewModel& cat = vm.categories[index];
    if (!SettingsRowShownIn(vm.left_pane_rect, cat.rect)) {
      continue;
    }
    const bool hovered = static_cast<int>(index) == card.hovered_category;
    const SDL_Color background = cat.selected ? theme.selection_strong
                                 : hovered    ? theme.row_highlight
                                              : theme.surface_background;
    if (cat.selected || hovered) {
      p.Fill(cat.rect, background);
      if (cat.selected && pane_focused) {
        DrawFocusRing(p.renderer, cat.rect, theme.accent);
      }
    }
    p.text.DrawStringOn(p.renderer, cat.rect.x + 12.0f, cat.rect.y + 7.0f,
                        cat.selected ? theme.text_primary : theme.text_muted, background,
                        p.Truncate(cat.label, cat.rect.w - 24.0f));
  }
  if (vm.category_scrollbar.has_value()) {
    DrawScrollbar(p.renderer, theme, vm.category_scrollbar->track, vm.category_scrollbar->thumb,
                  card.category_scrollbar_active);
  }
}

void PaintSettingsSectionHeader(const SettingsPainter& p, const SettingsOverlayViewModel& vm) {
  if (vm.section_title.empty()) {
    return;
  }
  const render::Theme& theme = p.theme;
  const float line_height = p.text.LineHeight();
  p.Fill(vm.section_header_rect, theme.surface_background);
  const float header_x = vm.section_header_rect.x + 12.0f;
  p.text.DrawStringOn(p.renderer, header_x, vm.section_header_rect.y + 6.0f, theme.accent, theme.surface_background,
                      p.Truncate(vm.section_title, vm.section_header_rect.w - 24.0f));
  if (!vm.section_subtitle.empty()) {
    p.text.DrawStringOn(p.renderer, header_x, vm.section_header_rect.y + 6.0f + line_height, theme.text_muted,
                        theme.surface_background, p.Truncate(vm.section_subtitle, vm.section_header_rect.w - 24.0f));
  }
  p.Fill(MakeRect(vm.section_header_rect.x, vm.section_header_rect.y + vm.section_header_rect.h - 1.0f,
                  vm.section_header_rect.w, 1.0f),
         theme.border);
}

float SettingsControlLeft(const SettingsRowViewModel& row) {
  const SettingsControlViewModel& control = row.control;
  switch (control.kind) {
    case SettingsControlKind::Checkbox:
      return control.checkbox_rect.x;
    case SettingsControlKind::Segmented:
    case SettingsControlKind::TextEdit:
      return control.value_rect.x;
    case SettingsControlKind::Stepper:
      return control.dec_rect.x;
    case SettingsControlKind::None:
      return row.row_rect.x + row.row_rect.w - 8.0f;
  }
  return row.row_rect.x + row.row_rect.w;
}

void PaintSettingsControl(const SettingsPainter& p, const SettingsRowViewModel& row, float control_left,
                          SDL_Color background) {
  const render::Theme& theme = p.theme;
  const SettingsControlViewModel& control = row.control;
  const float line_height = p.text.LineHeight();
  switch (control.kind) {
    case SettingsControlKind::Checkbox:
      p.Outline(control.checkbox_rect, theme.border);
      if (control.checkbox_on) {
        DrawCheckGlyph(p.renderer, control.checkbox_rect, theme.accent);
      }
      break;
    case SettingsControlKind::Segmented:
      p.Fill(control.value_rect, theme.chrome_background);
      p.Outline(control.value_rect, theme.border);
      DrawCenteredTextOn(p.text, p.renderer, control.value_rect, theme.accent, theme.chrome_background,
                         control.shown_value);
      break;
    case SettingsControlKind::Stepper:
      p.Outline(control.dec_rect, theme.border);
      DrawStepperArrow(p.renderer, control.dec_rect, true, theme.text_primary);
      p.Fill(control.value_rect, theme.chrome_background);
      p.Outline(control.value_rect, theme.border);
      DrawCenteredTextOn(p.text, p.renderer, control.value_rect, theme.accent, theme.chrome_background,
                         control.shown_value);
      p.Outline(control.inc_rect, theme.border);
      DrawStepperArrow(p.renderer, control.inc_rect, false, theme.text_primary);
      break;
    case SettingsControlKind::TextEdit: {
      p.Fill(control.value_rect, theme.chrome_background);
      p.Outline(control.value_rect, control.editing ? theme.accent : theme.border);
      const float text_x = control.value_rect.x + 6.0f;
      const float text_y = control.value_rect.y + (control.value_rect.h - line_height) * 0.5f;
      const SDL_Color text_color = control.value_is_placeholder ? theme.text_disabled : theme.text_primary;
      p.text.DrawStringOn(p.renderer, text_x, text_y, text_color, theme.chrome_background, control.shown_value);
      if (control.editing) {
        const float caret_x = text_x + control.caret_offset_x;
        p.Fill(MakeRect(std::min(caret_x, control.value_rect.x + control.value_rect.w - 2.0f),
                        control.value_rect.y + 3.0f, 1.5f, control.value_rect.h - 6.0f),
               theme.accent);
      }
      break;
    }
    case SettingsControlKind::None:
      if (!control.display_value.empty()) {
        const float vw = p.text.MeasureWidth(control.display_value);
        p.text.DrawStringOn(p.renderer, control_left - vw, row.row_rect.y + 6.0f, theme.accent, background,
                            control.display_value);
      }
      break;
  }
}

void PaintSettingsRow(const SettingsPainter& p, const SettingsOverlayViewModel& vm, const SettingsRowViewModel& row,
                      bool hovered, bool values_focused) {
  const render::Theme& theme = p.theme;
  const float line_height = p.text.LineHeight();
  if (!row.group_subheader.empty() && row.row_rect.y - line_height >= vm.right_pane_rect.y - 0.5f) {
    p.text.DrawStringOn(p.renderer, row.row_rect.x + 12.0f, row.row_rect.y - line_height + 3.0f, theme.text_muted,
                        theme.surface_background, p.Truncate(row.group_subheader, row.row_rect.w - 24.0f));
  }
  const SDL_Color background = row.selected ? theme.selection_strong
                               : hovered    ? theme.row_highlight
                                            : theme.surface_background;
  if (row.selected || hovered) {
    p.Fill(row.row_rect, background);
    if (row.selected && values_focused) {
      DrawFocusRing(p.renderer, row.row_rect, theme.accent);
    }
  }
  const float control_left = SettingsControlLeft(row);
  float text_left = row.resettable ? row.reset_rect.x : control_left;
  if (row.scope_rect.w > 0.0f) {
    text_left = std::min(text_left, row.scope_rect.x);
  }
  const float text_x = row.row_rect.x + 12.0f;
  const float text_width = std::max(40.0f, text_left - 8.0f - text_x);
  p.text.DrawStringOn(p.renderer, text_x, row.row_rect.y + 6.0f, theme.text_primary, background,
                      p.Truncate(row.label, text_width));
  const float desc_top = row.row_rect.y + 6.0f + line_height;
  if (row.scope_label_x >= 0.0f && !row.scope_label.empty()) {
    p.text.DrawStringOn(p.renderer, row.scope_label_x, desc_top, theme.text_disabled, background, row.scope_label);
  }
  float desc_y = desc_top;
  for (std::string_view desc_line : row.description_lines) {
    p.text.DrawStringOn(p.renderer, text_x, desc_y, theme.text_muted, background, desc_line);
    desc_y += line_height;
  }
  if (row.resettable) {
    p.Outline(row.reset_rect, theme.border);
    DrawResetGlyph(p.renderer, row.reset_rect, theme.text_muted);
  }
  if (row.scope_rect.w > 0.0f) {
    const SDL_Color scope_color = row.scope_is_project ? theme.accent : theme.text_muted;
    p.Fill(row.scope_rect, theme.chrome_background);
    p.Outline(row.scope_rect, theme.border);
    DrawCenteredTextOn(p.text, p.renderer, row.scope_rect, scope_color, theme.chrome_background,
                       p.Truncate(row.scope_text, row.scope_rect.w - 8.0f));
  }
  PaintSettingsControl(p, row, control_left, background);
}

void PaintSettingsRows(const SettingsPainter& p, const PreparedFrameState::SettingsCardPaint& card) {
  const SettingsOverlayViewModel& vm = card.vm;
  p.Fill(vm.right_pane_rect, p.theme.surface_background);
  const render::ScopedRenderClip rows_clip(
      p.renderer, SDL_Rect{static_cast<int>(std::floor(vm.right_pane_rect.x)),
                           static_cast<int>(std::floor(vm.right_pane_rect.y)),
                           static_cast<int>(std::ceil(vm.right_pane_rect.w)),
                           static_cast<int>(std::ceil(vm.right_pane_rect.h))});
  const bool values_focused = vm.focused_pane == SettingsPane::Values;
  for (std::size_t row_index = 0; row_index < vm.rows.size(); ++row_index) {
    const SettingsRowViewModel& row = vm.rows[row_index];
    if (SettingsRowShownIn(vm.right_pane_rect, row.row_rect)) {
      PaintSettingsRow(p, vm, row, static_cast<int>(row_index) == card.hovered_row, values_focused);
    }
  }
}

void PaintSettingsPicker(const SettingsPainter& p, const PreparedFrameState::SettingsCardPaint& card) {
  const SettingsPickerViewModel& picker = card.vm.value_picker;
  if (!picker.visible) {
    return;
  }
  const render::Theme& theme = p.theme;
  const float line_height = p.text.LineHeight();
  p.Fill(picker.rect, theme.chrome_background);
  p.Outline(picker.rect, theme.accent);
  for (const SettingsPickerItemViewModel& item : picker.items) {
    const SDL_Color background = item.highlighted ? theme.selection_strong : theme.chrome_background;
    if (item.highlighted) {
      p.Fill(item.rect, background);
    }
    if (item.is_choose_file) {
      p.Fill(MakeRect(item.rect.x, item.rect.y, item.rect.w, 1.0f), theme.border);
    }
    const SDL_Color foreground = item.is_choose_file ? theme.accent : theme.text_primary;
    p.text.DrawStringOn(p.renderer, item.rect.x + 8.0f, item.rect.y + (item.rect.h - line_height) * 0.5f, foreground,
                        background, p.Truncate(item.text, item.rect.w - 16.0f));
  }
  if (picker.scrollbar.has_value()) {
    DrawScrollbar(p.renderer, theme, picker.scrollbar->track, picker.scrollbar->thumb, card.picker_scrollbar_active);
  }
}

}

void WorkspaceShell::RenderSettingsOverlay(SDL_Renderer* renderer) const {
  const PreparedFrameState::SettingsCardPaint& card = frame_state_.settings_card;
  const SettingsOverlayViewModel& vm = card.vm;
  if (!vm.visible) {
    return;
  }
  const SettingsPainter painter{.renderer = renderer, .text = text_renderer_, .theme = theme_};
  PaintSettingsFrame(painter, card);
  if (!vm.empty_label.empty()) {
    text_renderer_.DrawStringOn(renderer, vm.rect.x + 18.0f, vm.filter_rect.y + vm.filter_rect.h + 12.0f,
                                theme_.text_muted, theme_.surface_background, vm.empty_label);
    return;
  }
  if (vm.mode == SettingsOverlayMode::HelpAbout) {
    RenderHelpAboutRows(renderer, vm, text_renderer_, theme_, card.scrollbar_active);
    return;
  }
  PaintSettingsCategories(painter, card);
  PaintSettingsSectionHeader(painter, vm);
  PaintSettingsRows(painter, card);
  if (vm.scrollbar.has_value()) {
    DrawScrollbar(renderer, theme_, vm.scrollbar->track, vm.scrollbar->thumb, card.scrollbar_active);
  }
  if (card.tooltip_visible) {
    DrawWrappedTooltip(text_renderer_, renderer, theme_, card.tooltip_rect, card.tooltip_lines);
  }
  PaintSettingsPicker(painter, card);
}

}
