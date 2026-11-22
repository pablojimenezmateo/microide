#include "workspace/render/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <cstring>
#include <string_view>

#include "render/DevicePixelSnap.h"
#include "render/PaintElement.h"
#include "workspace/registries/WorkspaceSidebarRegistry.h"
#include "workspace/render/EditorRowUnits.h"
#include "workspace/render/SidebarModeRowPaint.h"

// The sidebar's mode tab row. Prep settles what each tab shows -- which view is
// up, what the pointer is over, whether the overflow's menu is open -- into the
// frame's snapshot and derives the row's damage from it; paint draws the snapshot.
// The overflow button lights while its menu is up, which nothing compared before:
// it was repainted by whoever asked for the whole sidebar.

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

void DrawModeGlyph(SDL_Renderer* renderer, SidebarMode mode, const SDL_FRect& icon_rect, SDL_Color color) {
  switch (mode) {
    case SidebarMode::Tree:
      DrawFolderGlyph(renderer, icon_rect, color);
      break;
    case SidebarMode::Search:
      DrawSearchGlyph(renderer, icon_rect, color);
      break;
    case SidebarMode::Git:
      DrawBranchGlyph(renderer, icon_rect, color);
      break;
    default:
      break;
  }
}

}  // namespace

void PrepareSidebarModeRowFrame(PreparedFrameState& frame, const SidebarModeRowInputs& inputs) {
  PreparedFrameState::SidebarModeRowPaint& row = frame.sidebar_mode_row;
  row = PreparedFrameState::SidebarModeRowPaint{};
  // Every frame, shown or not: a hidden sidebar has no units.
  std::vector<DerivedDamage::Unit>& units = frame.derived_damage.Begin(DerivedSurface::SidebarModeRow);
  if (inputs.visible) {
    row.visible = true;
    const SidebarModeRowLayout& layout = inputs.row;
    row.tab_count = std::clamp(layout.tab_count, 0, static_cast<int>(row.tabs.size()));
    bool active_view_has_tab = false;
    for (int i = 0; i < row.tab_count; ++i) {
      const SidebarModeTab& tab = layout.tabs[static_cast<std::size_t>(i)];
      const SidebarViewSpec* spec = FindBuiltinSidebarView(tab.mode);
      const bool active = tab.id == inputs.active_view_id;
      active_view_has_tab = active_view_has_tab || active;
      row.tabs[static_cast<std::size_t>(i)] = SidebarModeTabPaint{
          .rect = tab.rect,
          .mode = tab.mode,
          // Icon only when the labels do not fit; the name shows on hover.
          .label = layout.icon_only || spec == nullptr ? std::string_view{} : spec->label,
          .hovered = inputs.pointer_over(tab.rect),
          .active = active,
      };
    }
    if (layout.has_overflow) {
      row.has_overflow = true;
      row.overflow_rect = layout.overflow_rect;
      row.overflow_hovered = inputs.pointer_over(layout.overflow_rect);
      // Lit whenever the current view lives behind the overflow rather than in a tab
      // (Problems / Tests / Outline / any plugin view), so the rail always shows where
      // you are, and while its menu is open.
      row.overflow_active = inputs.overflow_menu_open || !active_view_has_tab;
    }

    const render::Theme& theme = *inputs.theme;
    const std::uint64_t style = PaintFingerprint()
                                    .Add(theme.surface_background)
                                    .Add(theme.surface_raised)
                                    .Add(theme.row_highlight)
                                    .Add(theme.accent)
                                    .Add(theme.border)
                                    .Add(theme.text_primary)
                                    .Add(theme.text_secondary)
                                    .Add(theme.text_muted)
                                    .Add(inputs.text_renderer->MetricsGeneration())
                                    .value();
    // A slot per tab the row can hold, laid out or not, then the overflow's.
    for (std::size_t i = 0; i < row.tabs.size(); ++i) {
      const SidebarModeTabPaint& tab = row.tabs[i];
      if (static_cast<int>(i) >= row.tab_count) {
        units.push_back(DerivedDamage::Unit{});
        continue;
      }
      std::uint64_t key = MixRowKeyBytes(MixRect(style, tab.rect), tab.label.data(), tab.label.size());
      key = MixRowKey(key, static_cast<std::uint64_t>(tab.mode) << 8 | (tab.hovered ? 1u : 0u) | (tab.active ? 2u : 0u));
      units.push_back(DerivedDamage::Unit{.rect = UnitRect(inputs.renderer, tab.rect), .fingerprint = key});
    }
    if (row.has_overflow) {
      units.push_back(DerivedDamage::Unit{
          .rect = UnitRect(inputs.renderer, row.overflow_rect),
          .fingerprint = MixRowKey(MixRect(style, row.overflow_rect),
                                   (row.overflow_hovered ? 1u : 0u) | (row.overflow_active ? 2u : 0u))});
    } else {
      units.push_back(DerivedDamage::Unit{});
    }
  }
  frame.derived_damage.Commit(DerivedSurface::SidebarModeRow, inputs.damage);
}

void PaintSidebarModeRow(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                         const render::TextRenderer& text_renderer) {
  const PreparedFrameState::SidebarModeRowPaint& row = frame.sidebar_mode_row;
  if (!row.visible) {
    return;
  }
  for (int i = 0; i < row.tab_count; ++i) {
    const SidebarModeTabPaint& tab = row.tabs[static_cast<std::size_t>(i)];
    if (tab.rect.w <= 0.0f || tab.rect.h <= 0.0f) {
      continue;
    }
    const render::PaintElement element(renderer, tab.rect, "sidebar-mode-tab");
    DrawModeTab(text_renderer, renderer, theme, tab.rect, tab.hovered, tab.active, tab.label,
                [&](const SDL_FRect& icon_rect, SDL_Color color) { DrawModeGlyph(renderer, tab.mode, icon_rect, color); });
  }
  if (row.has_overflow && row.overflow_rect.w > 0.0f && row.overflow_rect.h > 0.0f) {
    const render::PaintElement element(renderer, row.overflow_rect, "sidebar-mode-overflow");
    const ButtonColors colors = ResolveButtonColors(
        theme, ButtonTone::Neutral,
        ButtonVisualState{.enabled = true, .hovered = row.overflow_hovered, .active = row.overflow_active});
    FillRect(renderer, row.overflow_rect, colors.fill);
    OutlineRect(renderer, row.overflow_rect, colors.border);
    DrawEllipsisGlyph(renderer, row.overflow_rect, colors.text);
  }
}

}  // namespace microide::workspace
