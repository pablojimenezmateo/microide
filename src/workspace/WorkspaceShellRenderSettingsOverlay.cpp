#include "workspace/WorkspaceShell.h"

#include <algorithm>

#include "workspace/RenderViewModelBuilder.h"
#include "workspace/WorkspaceShellRenderPrimitives.h"

namespace microide::workspace {

using namespace detail;

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
                        theme_.surface_background,
                        "Tip: left-click increases/cycles, right-click decreases/reverses");
    list_top += 16.0f;
  }
  const float list_bottom = vm.rect.y + vm.rect.h - 10.0f;
  int row_index = 0;
  const auto draw_row = [&](std::string_view label, std::string_view value, std::string_view detail,
                            bool active) {
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
    text_renderer_.DrawStringOn(renderer, row.x + 8.0f, row.y + 5.0f, theme_.text_primary,
                                active ? theme_.selection_fill : theme_.surface_background,
                                label);
    const float detail_x = row.x + std::min(290.0f, row.w * 0.42f);
    text_renderer_.DrawStringOn(renderer, detail_x, row.y + 5.0f, theme_.text_disabled,
                                active ? theme_.selection_fill : theme_.surface_background,
                                detail);
    const float value_x = row.x + std::min(520.0f, row.w * 0.72f);
    text_renderer_.DrawStringOn(renderer, value_x, row.y + 5.0f, theme_.accent,
                                active ? theme_.selection_fill : theme_.surface_background,
                                value);
  };

  if (vm.mode == SettingsOverlayMode::Settings) {
    for (const SettingsOverlayRow& row : vm.settings_rows) {
      draw_row(row.label, row.value, row.detail, false);
    }
  } else if (vm.mode == SettingsOverlayMode::AiProvider) {
    for (const AiProviderPickerRow& row : vm.provider_rows) {
      draw_row(row.label, row.model, row.auth_method, row.active);
    }
  } else {
    for (const HelpAboutRow& row : vm.help_rows) {
      draw_row(row.label, row.detail, {}, false);
    }
  }
}

}  // namespace microide::workspace
