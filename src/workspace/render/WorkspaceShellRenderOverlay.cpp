#include "workspace/render/WorkspaceShellRenderPrimitives.h"

#include "workspace/render/RenderViewModelBuilder.h"

#include <algorithm>
#include <cmath>

namespace microide::workspace {

using namespace detail;

void WorkspaceShell::RenderOverlaySurface(SDL_Renderer* renderer,
                                          const WorkspaceLayout& layout,
                                          const OverlaySurfaceViewModel& overlay_vm) {
  if (!overlay_vm.visible) {
    return;
  }

  if (overlay_vm.mode == OverlayMode::BufferSearch ||
      overlay_vm.mode == OverlayMode::BufferReplace) {
    RenderFindWidget(renderer, overlay_vm.find_widget);
    return;
  }

  if (!overlay_vm.caret_anchored) {
    DrawFilledRect(renderer, layout.overlay_anchor, theme_.overlay_backdrop);
  }
  const SDL_FRect overlay = overlay_vm.overlay_rect;
  if (overlay_vm.caret_anchored) {
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

  if (!overlay_vm.title.empty()) {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
               theme_.text_primary, theme_.chrome_background, overlay_vm.title);
  }
  if (!overlay_vm.note.empty()) {
    DrawTextOn(text_renderer_, renderer, overlay_vm.note_x, overlay.y + 8.0f, theme_.text_muted,
               theme_.overlay_background, overlay_vm.note);
  }
  if (!overlay_vm.context_label.empty()) {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 30.0f,
               theme_.text_muted, theme_.overlay_background, overlay_vm.context_label);
  }
  if (overlay_vm.has_query_field) {
    DrawTextFieldFrame(renderer, theme_, overlay_field_rect(overlay_vm.query_row_y),
                       overlay_vm.current_surface == overlay_vm.query_surface);
    DrawSingleLineTextTail(renderer, overlay.x + kOverlayInset,
                           overlay_field_text_y(overlay_vm.query_row_y),
                           std::max(1.0f, overlay.w - kOverlayInset * 2.0f),
                           theme_.text_secondary, theme_.surface_background,
                           overlay_vm.query_display_text);
  }
  if (!overlay_vm.summary_line.empty()) {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay_vm.summary_y,
               theme_.text_muted, theme_.overlay_background, overlay_vm.summary_line);
  }
  if (!overlay_vm.hint.empty()) {
    DrawTextOn(text_renderer_, renderer, overlay_vm.hint_x, overlay_vm.summary_y,
               theme_.text_disabled, theme_.overlay_background, overlay_vm.hint);
  }
  const ScrollableListLayout& list_layout = overlay_vm.list_layout;
  if (!overlay_vm.error_line.empty()) {
    if (overlay_vm.error_at_title_row) {
      DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset, overlay.y + 8.0f,
                 theme_.diff_deleted, theme_.overlay_background, overlay_vm.error_line);
    } else {
      DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset,
                 list_layout.list_rect.y + 6.0f, theme_.text_muted, theme_.overlay_background,
                 overlay_vm.error_line);
    }
  }
  if (!overlay_vm.empty_label.empty()) {
    DrawTextOn(text_renderer_, renderer, overlay.x + kOverlayInset,
               list_layout.list_rect.y + 6.0f, theme_.text_muted, theme_.overlay_background,
               overlay_vm.empty_label);
  }

  for (std::size_t row = 0; row < overlay_vm.rows.size(); ++row) {
    const OverlayListRowViewModel& item = overlay_vm.rows[row];
    const bool selected =
        overlay_vm.scroll_row + static_cast<int>(row) == overlay_vm.selected_row;
    SDL_FRect row_rect = ScrollableListRowRect(list_layout, static_cast<int>(row));
    const bool emphasized =
        selected || (row < frame_state_.overlay_pointer.row_hovered.size() && frame_state_.overlay_pointer.row_hovered[row]);
    DrawSelectableRowBackground(renderer, theme_, row_rect, theme_.surface_raised, emphasized,
                                selected);
    if (selected) {
      DrawFilledRect(renderer, MakeRect(row_rect.x, row_rect.y + 2.0f, 3.0f, row_rect.h - 4.0f),
                     theme_.accent);
    }
    const SDL_Color row_bg = emphasized ? theme_.row_highlight : theme_.surface_raised;
    if (!item.secondary.empty()) {
      DrawVCenteredTextOn(
          text_renderer_, renderer,
          MakeRect(row_rect.x + row_rect.w - item.secondary_width, row_rect.y,
                   item.secondary_width, row_rect.h),
          0.0f, theme_.text_muted, row_bg, item.secondary);
    }
    const float primary_width = std::max(20.0f, row_rect.w - 12.0f - item.secondary_width);
    DrawVCenteredTextOn(text_renderer_, renderer,
                        MakeRect(row_rect.x + 10.0f, row_rect.y, primary_width, row_rect.h), 0.0f,
                        emphasized ? theme_.text_primary : theme_.text_secondary, row_bg,
                        item.primary);
  }

  if (const auto geometry = MakeVerticalScrollbarGeometry(
          list_layout.list_rect, static_cast<float>(overlay_vm.total_rows),
          list_layout.visible_units, static_cast<float>(overlay_vm.scroll_row));
      geometry.has_value()) {
    DrawScrollbar(renderer, theme_, geometry->track, geometry->thumb,
                  frame_state_.overlay_pointer.scrollbar_dragged);
  }
}

void WorkspaceShell::RenderFindWidget(SDL_Renderer* renderer,
                                      const OverlayFindWidgetViewModel& fw_vm) {
  const FindWidgetLayout& fw = fw_vm.fw;
  if (fw.widget.w <= 0.0f || fw.widget.h <= 0.0f) {
    return;
  }

  render::DrawCardFrame(renderer, theme_, fw.widget, render::CardStyle::Overlay);

  const auto draw_field = [&](const SDL_FRect& field, bool focused, std::string_view text) {
    if (field.w <= 0.0f || field.h <= 0.0f) {
      return;
    }
    DrawTextFieldFrame(renderer, theme_, field, focused);
    const float text_y = field.y + std::floor((field.h - text_renderer_.LineHeight()) * 0.5f);
    DrawSingleLineTextTail(renderer, field.x + 6.0f, text_y, std::max(1.0f, field.w - 12.0f),
                           focused ? theme_.text_primary : theme_.text_secondary,
                           theme_.surface_background, text);
  };

  enum class Icon { Prev, Next, Close };
  const PreparedFrameState::OverlayPointerPaint& pointer = frame_state_.overlay_pointer;
  const auto icon_button = [&](const SDL_FRect& rect, Icon icon, bool enabled, bool pointer_over) {
    if (rect.w <= 0.0f) {
      return;
    }
    const ButtonTone tone = icon == Icon::Close ? ButtonTone::Destructive : ButtonTone::Neutral;
    const ButtonColors colors = ResolveButtonColors(
        theme_, tone,
        ButtonVisualState{.enabled = enabled, .hovered = enabled && pointer_over});
    FillRect(renderer, rect, colors.fill);
    OutlineRect(renderer, rect, colors.border);
    switch (icon) {
      case Icon::Prev:
        DrawArrowGlyph(renderer, rect, true, colors.text);
        break;
      case Icon::Next:
        DrawArrowGlyph(renderer, rect, false, colors.text);
        break;
      case Icon::Close:
        DrawCloseGlyph(renderer, rect, colors.text);
        break;
    }
  };

  draw_field(fw.search_field, fw_vm.search_focused, fw_vm.search_display_text);

  for (std::size_t index = 0; index < fw.toggle_count; ++index) {
    if (fw.toggle_buttons[index].w <= 0.0f) {
      continue;
    }
    DrawButtonCentered(text_renderer_, renderer, theme_, fw.toggle_buttons[index],
                       fw_vm.toggles[index].label,
                       fw_vm.toggles[index].active ? ButtonTone::Accent : ButtonTone::Neutral,
                       ButtonVisualState{.enabled = true,
                                         .hovered = pointer.toggle_hovered[index],
                                         .active = fw_vm.toggles[index].active});
  }

  if (!fw_vm.count_text.empty() && fw.count_rect.w > 0.0f) {
    DrawCenteredTextOn(text_renderer_, renderer, fw.count_rect,
                       fw_vm.has_matches ? theme_.text_secondary : theme_.text_muted,
                       theme_.overlay_background, fw_vm.count_text);
  }

  icon_button(fw.prev_button, Icon::Prev, fw_vm.has_matches, pointer.prev_hovered);
  icon_button(fw.next_button, Icon::Next, fw_vm.has_matches, pointer.next_hovered);
  icon_button(fw.close_button, Icon::Close, true, pointer.close_hovered);

  if (fw_vm.replace_mode) {
    draw_field(fw.replace_field, fw_vm.replace_focused, fw_vm.replace_display_text);
    DrawButtonCentered(text_renderer_, renderer, theme_, fw.replace_button, "Replace",
                       ButtonTone::Neutral,
                       ButtonVisualState{.enabled = fw_vm.has_matches,
                                         .hovered = fw_vm.has_matches && pointer.replace_hovered});
    DrawButtonCentered(
        text_renderer_, renderer, theme_, fw.replace_all_button, "All", ButtonTone::Neutral,
        ButtonVisualState{.enabled = fw_vm.has_query,
                          .hovered = fw_vm.has_query && pointer.replace_all_hovered});
  }
}

}
