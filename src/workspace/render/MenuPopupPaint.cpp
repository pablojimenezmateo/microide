#include "workspace/render/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <cstring>
#include <string_view>

#include "render/DevicePixelSnap.h"
#include "render/PaintElement.h"
#include "workspace/render/EditorRowUnits.h"
#include "workspace/render/MenuPopupPaint.h"

// The open menus. A row repaints when what it shows changes, whoever changed it:
// the pointer moving along the rows or leaving for a toast, the keyboard's active
// row, a terminal's shell exiting under the open menu and enabling Relaunch. The
// popup used to be one unit keyed on its rows' enabled and checked flags, with the
// hover repainted by the menu's motion path and by a probe in the hover unit.

namespace microide::workspace {

using namespace detail;

namespace {

SDL_FRect UnitRect(SDL_Renderer* renderer, const SDL_FRect& rect) {
  const SDL_Rect snapped = render::SnapToDevicePixels(renderer, rect);
  return SDL_FRect{static_cast<float>(snapped.x), static_cast<float>(snapped.y), static_cast<float>(snapped.w),
                   static_cast<float>(snapped.h)};
}

std::uint64_t FloatBits(float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof bits);
  return bits;
}
std::uint64_t MixRect(std::uint64_t key, const SDL_FRect& rect) {
  key = MixRowKey(key, FloatBits(rect.x) | FloatBits(rect.y) << 32);
  return MixRowKey(key, FloatBits(rect.w) | FloatBits(rect.h) << 32);
}
std::uint64_t MixText(std::uint64_t key, std::string_view text) {
  return MixRowKeyBytes(key, text.data(), text.size());
}

// Everything DrawMenuRow (or the separator's rule) reads of a row.
std::uint64_t RowKey(const MenuPopupPaint& popup, const MenuRowPaint& row, std::uint64_t style) {
  std::uint64_t key = MixRect(style, row.rect);
  key = MixRowKey(key, (row.enabled ? 1u : 0u) | (row.checked ? 2u : 0u) | (row.hovered ? 4u : 0u) |
                           (row.separator ? 8u : 0u));
  return MixText(MixText(key, popup.Label(row)), popup.Accelerator(row));
}

}  // namespace

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
      // The card under the rows: its fill and outline.
      units.push_back(DerivedDamage::Unit{.rect = UnitRect(renderer, popup.rect), .fingerprint = MixRect(style, popup.rect)});
      for (const MenuRowPaint& row : popup.rows) {
        units.push_back(DerivedDamage::Unit{.rect = UnitRect(renderer, row.rect), .fingerprint = RowKey(popup, row, style)});
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

}  // namespace microide::workspace
