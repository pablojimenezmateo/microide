#include "workspace/render/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <string_view>

#include "render/DevicePixelSnap.h"
#include "render/PaintElement.h"
#include "workspace/render/EditorRowUnits.h"
#include "workspace/render/MenuPopupPaint.h"

namespace microide::workspace {

using namespace detail;

namespace {

std::uint64_t RowKey(const MenuPopupPaint& popup, const MenuRowPaint& row, std::uint64_t style) {
  std::uint64_t key = MixRowKeyRect(style, row.rect);
  key = MixRowKey(key, (row.enabled ? 1u : 0u) | (row.checked ? 2u : 0u) | (row.hovered ? 4u : 0u) |
                           (row.separator ? 8u : 0u));
  return MixRowKeyText(MixRowKeyText(key, popup.Label(row)), popup.Accelerator(row));
}

}

void CommitMenuPopupUnits(PreparedFrameState& frame, SDL_Renderer* renderer, const render::Theme& theme,
                          std::uint64_t text_metrics, const std::function<void(const SDL_FRect&)>& damage) {
  const std::uint64_t style = PaintFingerprint()
                                  .Add(theme.overlay_background)
                                  .Add(theme.row_highlight)
                                  .Add(theme.border)
                                  .Add(theme.accent)
                                  .Add(theme.text_primary)
                                  .Add(theme.text_secondary)
                                  .Add(theme.text_muted)
                                  .Add(theme.text_disabled)
                                  .Add(text_metrics)
                                  .value();
  for (std::size_t slot = 0; slot < frame.menu_popups.size(); ++slot) {
    const MenuPopupPaint& popup = frame.menu_popups[slot];
    const DerivedSurface surface = MenuPopupSurface(static_cast<MenuPopupSlot>(slot));
    std::vector<DerivedDamage::Unit>& units = frame.derived_damage.Begin(surface);
    if (popup.visible) {
      units.push_back(DerivedDamage::Unit{.rect = render::SnapToDevicePixelsF(renderer, popup.rect),
                                          .fingerprint = MixRowKeyRect(style, popup.rect)});
      for (const MenuRowPaint& row : popup.rows) {
        units.push_back(DerivedDamage::Unit{.rect = render::SnapToDevicePixelsF(renderer, row.rect),
                                            .fingerprint = RowKey(popup, row, style)});
      }
    }
    frame.derived_damage.Commit(surface, damage);
  }
}

void PaintMenuPopups(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                     const render::TextRenderer& text_renderer) {
  for (const MenuPopupPaint& popup : frame.menu_popups) {
    if (!popup.visible) {
      continue;
    }
    {
      const render::PaintElement element(renderer, popup.rect, "menu-popup");
      DrawCardFrame(renderer, theme, popup.rect, CardStyle::Overlay);
    }
    for (const MenuRowPaint& row : popup.rows) {
      const render::PaintElement element(renderer, row.rect, "menu-row");
      if (row.separator) {
        FillRect(renderer,
                 MakeRect(row.rect.x + 8.0f, row.rect.y + row.rect.h * 0.5f, std::max(0.0f, row.rect.w - 16.0f), 1.0f),
                 theme.border);
        continue;
      }
      DrawMenuRow(text_renderer, renderer, theme, row.rect, popup.Label(row), popup.Accelerator(row), row.enabled,
                  row.hovered, row.checked);
    }
  }
}

}
