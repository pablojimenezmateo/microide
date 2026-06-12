#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <optional>
#include <string_view>

#include "workspace/RenderViewModelBuilder.h"
#include "workspace/WorkspaceShellRenderPrimitives.h"
#include "workspace/WorkspaceSettingsRegistry.h"

namespace microide::workspace {

using namespace detail;

std::string WorkspaceShell::SettingsOverlayInputHintLabel() const {
  return "Note: left-click increases/cycles, right-click decreases/reverses";
}

void WorkspaceShell::RenderSettingsOverlay(SDL_Renderer* renderer,
                                           const WorkspaceLayout& layout) const {
  const SettingsOverlayViewModel vm =
      RenderViewModelBuilder(context_).BuildSettingsOverlay(layout, settings_overlay_service_);
  if (!vm.visible) {
    return;
  }

  DrawFilledRect(renderer, vm.rect, theme_.surface_background);
  DrawRect(renderer, vm.rect, theme_.border);
  const SDL_FRect header = MakeRect(vm.rect.x, vm.rect.y, vm.rect.w, 34.0f);
  DrawFilledRect(renderer, header, theme_.chrome_background);
  DrawFilledRect(renderer, MakeRect(header.x, header.y + header.h - 1.0f, header.w, 1.0f),
                 theme_.border);
  text_renderer_.DrawStringOn(renderer, header.x + 14.0f, header.y + 9.0f, theme_.accent,
                              theme_.chrome_background, vm.title);

  const std::string_view query_label = vm.query.empty() ? std::string_view("type to filter settings...")
                                                        : std::string_view(vm.query);
  const SDL_FRect search = MakeRect(vm.rect.x + vm.rect.w - 270.0f, vm.rect.y + 7.0f, 252.0f, 20.0f);
  DrawFilledRect(renderer, search, theme_.editor_background);
  DrawRect(renderer, search, theme_.border);
  text_renderer_.DrawStringOn(renderer, search.x + 7.0f, search.y + 3.0f,
                              vm.query.empty() ? theme_.text_disabled : theme_.text_primary,
                              theme_.editor_background, query_label);

  const float row_height = 24.0f;
  float list_top = vm.rect.y + header.h + 8.0f;
  if (vm.mode == SettingsOverlayMode::Settings) {
    const SDL_FRect hint_rect =
        MakeRect(vm.rect.x + 12.0f, vm.rect.y + header.h + 4.0f, vm.rect.w - 24.0f, 16.0f);
    DrawVCenteredTextOn(text_renderer_, renderer, hint_rect, 0.0f, theme_.text_muted,
                        theme_.surface_background, SettingsOverlayInputHintLabel());
    list_top += 16.0f;
  }
  const float list_bottom = vm.rect.y + vm.rect.h - 10.0f;

  // --- Help/About layout (also used to measure its variable-height rows). In
  //     Settings mode vm.help_rows is empty, so these are cheap no-ops. ---
  const float content_x = vm.rect.x + 18.0f;
  const float content_right = vm.rect.x + vm.rect.w - 16.0f;
  const float line_height = text_renderer_.LineHeight();
  const float help_entry_gap = 6.0f;
  const float help_inner_width = std::max(1.0f, content_right - content_x);
  float label_col = 0.0f;
  for (const HelpAboutRow& row : vm.help_rows) {
    label_col = std::max(label_col, text_renderer_.MeasureWidth(row.label));
  }
  label_col = std::clamp(label_col, 100.0f, help_inner_width * 0.40f);
  const float column_gap = 16.0f;
  const float detail_x = content_x + label_col + column_gap;
  const float detail_width = std::max(40.0f, content_right - detail_x);

  // Greedy word wrap that yields string_view slices of the original detail text
  // (no per-line allocation in the paint path).
  const auto for_each_wrapped_line = [&](std::string_view text, float max_width,
                                         const auto& emit_line) {
    if (text.empty()) {
      return;
    }
    std::size_t line_start = 0;
    std::size_t line_end = 0;  // end of the words committed to the current line
    std::size_t word_start = 0;
    while (word_start < text.size()) {
      const std::size_t space = text.find(' ', word_start);
      const std::size_t word_end = space == std::string_view::npos ? text.size() : space;
      const std::string_view candidate = text.substr(line_start, word_end - line_start);
      if (line_start != word_start && text_renderer_.MeasureWidth(candidate) > max_width) {
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

  // --- Resolve scroll bounds so scrolling clamps to content and the scrollbar
  //     reflects position. Settings rows (and group headers) are fixed-height;
  //     Help/About rows vary, so its visible count is measured from the bottom. ---
  const float available_height = std::max(0.0f, list_bottom - list_top);
  int total_rows = 0;
  int visible_rows = 0;
  if (vm.mode == SettingsOverlayMode::Settings) {
    int group_headers = 0;
    std::string_view prev_group;
    bool have_prev = false;
    for (const SettingsOverlayRow& row : vm.settings_rows) {
      if (!have_prev || row.group != prev_group) {
        prev_group = row.group;
        have_prev = true;
        if (!row.group.empty()) {
          ++group_headers;
        }
      }
    }
    total_rows = static_cast<int>(vm.settings_rows.size()) + group_headers;
    visible_rows = std::max(1, static_cast<int>(available_height / row_height));
  } else {
    total_rows = static_cast<int>(vm.help_rows.size());
    float accumulated = 0.0f;
    int fit = 0;
    for (int i = static_cast<int>(vm.help_rows.size()) - 1; i >= 0; --i) {
      accumulated += help_entry_height(vm.help_rows[static_cast<std::size_t>(i)].detail);
      if (accumulated > available_height) {
        break;
      }
      ++fit;
    }
    visible_rows = std::max(1, fit);
  }
  const int max_scroll = std::max(0, total_rows - visible_rows);
  const int effective_scroll = std::clamp(vm.scroll_row, 0, max_scroll);
  // Surface the resolved bound to the wheel handler (which clamps live scrolling).
  settings_overlay_max_scroll_row_ = max_scroll;

  int row_index = 0;
  const auto bool_is_on = [](std::string_view value) {
    return !(value == "false" || value == "0" || value == "off" || value.empty());
  };
  const auto draw_row = [&](std::string_view label, std::string_view value, std::string_view detail,
                            bool active, std::optional<bool> bool_state) {
    if (row_index++ < effective_scroll) {
      return;
    }
    const float y = list_top + static_cast<float>(row_index - effective_scroll - 1) * row_height;
    if (y + row_height > list_bottom) {
      return;
    }
    const SDL_FRect row = MakeRect(vm.rect.x + 10.0f, y, vm.rect.w - 20.0f, row_height);
    if (active) {
      DrawFilledRect(renderer, row, theme_.selection_fill);
    }
    const SDL_Color row_background = active ? theme_.selection_fill : theme_.surface_background;
    float label_x = row.x + 8.0f;
    if (bool_state.has_value()) {
      // Reserve a fixed gutter on the left for a checkbox-style glyph so
      // boolean settings read as on/off at a glance instead of as the
      // literal "true"/"false" text in the value column.
      const SDL_FRect box = MakeRect(row.x + 6.0f, row.y + 5.0f, 14.0f, 14.0f);
      DrawRect(renderer, box, theme_.border);
      if (*bool_state) {
        DrawCheckGlyph(renderer, box, theme_.accent);
      }
      label_x = box.x + box.w + 8.0f;
    }
    text_renderer_.DrawStringOn(renderer, label_x, row.y + 5.0f, theme_.text_primary,
                                row_background, label);
    const float detail_x = row.x + std::min(290.0f, row.w * 0.42f);
    text_renderer_.DrawStringOn(renderer, detail_x, row.y + 5.0f, theme_.text_disabled,
                                row_background, detail);
    if (!bool_state.has_value()) {
      const float value_x = row.x + std::min(520.0f, row.w * 0.72f);
      text_renderer_.DrawStringOn(renderer, value_x, row.y + 5.0f, theme_.accent,
                                  row_background, value);
    }
  };

  if (vm.mode == SettingsOverlayMode::Settings) {
    std::string current_group;
    const auto draw_group_header = [&](std::string_view text) {
      if (row_index++ < effective_scroll) {
        return;
      }
      const float y = list_top + static_cast<float>(row_index - effective_scroll - 1) * row_height;
      if (y + row_height > list_bottom) {
        return;
      }
      const SDL_FRect header_rect = MakeRect(vm.rect.x + 10.0f, y, vm.rect.w - 20.0f, row_height);
      DrawFilledRect(renderer, header_rect, theme_.chrome_background);
      text_renderer_.DrawStringOn(renderer, header_rect.x + 8.0f, header_rect.y + 5.0f,
                                  theme_.accent, theme_.chrome_background, text);
    };
    for (const SettingsOverlayRow& row : vm.settings_rows) {
      if (row.group != current_group) {
        current_group = row.group;
        if (!current_group.empty()) {
          draw_group_header(current_group);
        }
      }
      std::optional<bool> bool_state;
      if (row.type == SettingType::Bool) {
        bool_state = bool_is_on(row.value);
      }
      draw_row(row.label, row.value, row.detail, false, bool_state);
    }
  } else {
    // Help / About is a two-column read-only list: a left label column sized to
    // the widest label (clamped so the detail keeps room) and a right detail
    // column that word-wraps within the panel instead of overflowing past its
    // right edge. Rows have variable height, so vertical scrolling is tracked by
    // whole entries (effective_scroll counts entries skipped from the top).
    float y = list_top;
    int entry_index = 0;
    for (const HelpAboutRow& row : vm.help_rows) {
      if (entry_index++ < effective_scroll) {
        continue;
      }
      if (y + line_height > list_bottom) {
        break;
      }
      const std::string label_text = text_renderer_.TruncateToWidth(row.label, label_col);
      text_renderer_.DrawStringOn(renderer, content_x, y, theme_.text_primary,
                                  theme_.surface_background, label_text);
      float detail_y = y;
      for_each_wrapped_line(row.detail, detail_width, [&](std::string_view line) {
        if (detail_y + line_height <= list_bottom) {
          text_renderer_.DrawStringOn(renderer, detail_x, detail_y, theme_.text_muted,
                                      theme_.surface_background, line);
        }
        detail_y += line_height;
      });
      y = std::max(y + line_height, detail_y) + help_entry_gap;
    }
  }

  // A scrollbar in the right margin signals (and reflects) that more rows exist
  // below/above the visible window. Drawn for both modes once content overflows,
  // routed through the shared geometry + tone so it matches every other scrollbar in
  // the shell rather than reading as a thinner, differently-inset widget.
  if (max_scroll > 0) {
    const float track_h = std::max(1.0f, list_bottom - list_top);
    const SDL_FRect list_area = MakeRect(vm.rect.x, list_top, vm.rect.w, track_h);
    if (const auto geometry = MakeVerticalScrollbarGeometry(
            list_area, static_cast<float>(total_rows), static_cast<float>(visible_rows),
            static_cast<float>(effective_scroll), false);
        geometry.has_value()) {
      DrawScrollbar(renderer, theme_, geometry->track, geometry->thumb, false);
    }
  }
}

}  // namespace microide::workspace
