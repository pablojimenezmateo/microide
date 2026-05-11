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
  int row_index = 0;
  const auto bool_is_on = [](std::string_view value) {
    return !(value == "false" || value == "0" || value == "off" || value.empty());
  };
  const auto draw_row = [&](std::string_view label, std::string_view value, std::string_view detail,
                            bool active, std::optional<bool> bool_state) {
    if (row_index++ < vm.scroll_row) {
      return;
    }
    const float y = list_top + static_cast<float>(row_index - vm.scroll_row - 1) * row_height;
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
      if (row_index++ < vm.scroll_row) {
        return;
      }
      const float y = list_top + static_cast<float>(row_index - vm.scroll_row - 1) * row_height;
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
    for (const HelpAboutRow& row : vm.help_rows) {
      draw_row(row.label, row.detail, {}, false, std::nullopt);
    }
  }
}

}  // namespace microide::workspace
