#include "workspace/render/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string_view>

#include "render/DevicePixelSnap.h"
#include "render/PaintElement.h"
#include "workspace/render/EditorRowUnits.h"
#include "workspace/render/SidebarListPaint.h"

// The sidebar's shared two-column list (Problems, Tests, Outline, plugin item
// views). Prep lays it out into the frame's snapshot and derives its damage from
// it; paint draws the snapshot only. A row repaints when what it shows changes,
// whoever changed it: diagnostics republished, a test run's status landing off the
// shell thread, a plugin's items replaced, a pointer resting on a row.

namespace microide::workspace {

using namespace detail;

namespace {

constexpr float kSidebarInset = 10.0f;

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
  key = MixRowKey(key, FloatBits(rect.x) << 32 | FloatBits(rect.y));
  return MixRowKey(key, FloatBits(rect.w) << 32 | FloatBits(rect.h));
}

std::uint64_t MixText(std::uint64_t key, std::string_view text) {
  return MixRowKeyBytes(key, text.data(), text.size());
}

std::uint64_t ColorBits(SDL_Color color) {
  return static_cast<std::uint64_t>(color.r) | static_cast<std::uint64_t>(color.g) << 8 |
         static_cast<std::uint64_t>(color.b) << 16 | static_cast<std::uint64_t>(color.a) << 24;
}

}  // namespace

SDL_FRect SidebarPlaceholderBox(const SDL_FRect& sidebar, const ScrollableListLayout& list) {
  const float top = list.row_y + 4.0f;
  return MakeRect(sidebar.x + kSidebarInset, top, sidebar.w - kSidebarInset * 2.0f,
                  std::max(0.0f, list.list_rect.y + list.list_rect.h - top));
}

void PrepareSidebarListFrame(PreparedFrameState& frame, const SidebarListInputs& inputs) {
  PreparedFrameState::SidebarListPaint& list = frame.sidebar_list;
  list.visible = false;
  list.rows.clear();
  list.text.clear();
  list.placeholder_length = 0;
  list.scrollbar.reset();
  std::vector<DerivedDamage::Unit>& units = frame.derived_damage.Begin(DerivedSurface::SidebarList);
  if (inputs.visible) {
    list.visible = true;
    const render::Theme& theme = *inputs.theme;
    std::uint64_t style = MixRowKey(0x5157u, inputs.text_renderer->MetricsGeneration());
    for (const SDL_Color color : {theme.row_highlight, theme.surface_background, theme.text_primary,
                                  theme.text_secondary, theme.text_muted, theme.diff_deleted, theme.accent}) {
      style = MixRowKey(style, ColorBits(color));
    }
    const ScrollableListLayout& layout = inputs.list;
    const int scroll_row = layout.scroll_row;
    // The visible rows' labels and the placeholder, copied into the snapshot's own
    // text: one pass to size it (its capacity is kept across frames), one to fill.
    std::size_t bytes = inputs.placeholder.size();
    for (int row = 0; row < layout.visible_rows; ++row) {
      const std::size_t index = static_cast<std::size_t>(scroll_row + row);
      if (index >= inputs.row_count) {
        break;
      }
      const SidebarListRow item = inputs.row_at(index);
      bytes += item.primary.size() + item.secondary.size();
    }
    list.text.reserve(bytes);
    // The placeholder and the scrollbar come first, at fixed places: the rows that
    // follow come and go with the list.
    list.placeholder_is_error = inputs.placeholder_is_error;
    list.placeholder_box = inputs.placeholder.empty() ? SDL_FRect{} : SidebarPlaceholderBox(inputs.sidebar, layout);
    list.placeholder_at = list.text.size();
    list.text.append(inputs.placeholder);
    list.placeholder_length = inputs.placeholder.size();
    units.push_back(DerivedDamage::Unit{
        .rect = UnitRect(inputs.renderer, list.placeholder_box),
        .fingerprint = MixRowKey(MixText(MixRect(style, list.placeholder_box), inputs.placeholder),
                                 inputs.placeholder_is_error ? 3u : 2u)});
    if (const auto geometry = MakeVerticalScrollbarGeometry(
            layout.list_rect, static_cast<float>(inputs.row_count), layout.visible_units,
            static_cast<float>(scroll_row));
        geometry.has_value()) {
      list.scrollbar = PanelScrollbarPaint{
          .track = geometry->track, .thumb = geometry->thumb, .active = inputs.scrollbar_active};
    }
    units.push_back(DerivedDamage::Unit{
        .rect = list.scrollbar.has_value() ? UnitRect(inputs.renderer, list.scrollbar->track) : SDL_FRect{},
        .fingerprint = list.scrollbar.has_value()
                           ? MixRowKey(MixRect(MixRect(style, list.scrollbar->track), list.scrollbar->thumb),
                                       list.scrollbar->active ? 3u : 2u)
                           : 1u});
    for (int row = 0; row < layout.visible_rows; ++row) {
      const std::size_t index = static_cast<std::size_t>(scroll_row + row);
      if (index >= inputs.row_count) {
        break;
      }
      const SidebarListRow item = inputs.row_at(index);
      PreparedFrameState::SidebarListRowPaint& painted = list.rows.emplace_back();
      painted.rect = ScrollableListRowRect(layout, row);
      painted.selected = index == inputs.selected_index;
      painted.emphasized = painted.selected || inputs.pointer_over(painted.rect);
      // Tree rows: indentation by depth plus a disclosure twisty on collapsible
      // rows; flat rows keep the list's left inset.
      painted.label_x = painted.rect.x + 6.0f;
      if (item.depth > 0 || item.collapsible) {
        painted.tree_x = painted.rect.x + 6.0f + static_cast<float>(item.depth) * kWorkspaceSidebarTreeIndentWidth;
        painted.label_x = painted.tree_x + kWorkspaceSidebarTreeChevronSlotWidth + 4.0f;
        painted.chevron = item.collapsible;
        painted.collapsed = item.collapsed;
      }
      painted.primary_at = list.text.size();
      list.text.append(item.primary);
      painted.primary_length = item.primary.size();
      painted.secondary_at = list.text.size();
      list.text.append(item.secondary);
      painted.secondary_length = item.secondary.size();
      std::uint64_t key = MixRect(style, painted.rect);
      key = MixText(MixText(key, item.primary), item.secondary);
      key = MixRowKey(key, FloatBits(painted.label_x) << 32 | FloatBits(painted.tree_x));
      key = MixRowKey(key, (painted.selected ? 1u : 0u) | (painted.emphasized ? 2u : 0u) |
                               (painted.chevron ? 4u : 0u) | (painted.collapsed ? 8u : 0u));
      units.push_back(DerivedDamage::Unit{.rect = UnitRect(inputs.renderer, painted.rect), .fingerprint = key});
    }
  }
  frame.derived_damage.Commit(DerivedSurface::SidebarList, inputs.damage);
}

void PaintSidebarList(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                      const render::TextRenderer& text_renderer) {
  const PreparedFrameState::SidebarListPaint& list = frame.sidebar_list;
  if (!list.visible) {
    return;
  }
  const std::string_view text(list.text);
  for (const PreparedFrameState::SidebarListRowPaint& row : list.rows) {
    const render::PaintElement element(renderer, row.rect, "sidebar-list-row");
    const SDL_Color background = row.emphasized ? theme.row_highlight : theme.surface_background;
    DrawSelectableRowBackground(renderer, theme, row.rect, theme.surface_background, row.emphasized, row.selected);
    if (row.chevron) {
      DrawChevron(renderer, row.tree_x, row.rect.y + row.rect.h * 0.5f, !row.collapsed, theme.text_muted);
    }
    DrawPrimarySecondaryRowText(text_renderer, renderer, row.rect, row.label_x, row.rect.x + row.rect.w - 6.0f,
                                row.emphasized ? theme.text_primary : theme.text_secondary,
                                row.emphasized ? theme.text_secondary : theme.text_muted, background,
                                text.substr(row.primary_at, row.primary_length),
                                text.substr(row.secondary_at, row.secondary_length), 0.62f);
  }
  if (list.placeholder_length > 0) {
    const render::PaintElement element(renderer, list.placeholder_box, "sidebar-list-placeholder");
    DrawWrappedPlaceholderIn(text_renderer, renderer, list.placeholder_box,
                             list.placeholder_is_error ? theme.diff_deleted : theme.text_muted,
                             theme.surface_background, text.substr(list.placeholder_at, list.placeholder_length));
  }
  if (list.scrollbar.has_value()) {
    const render::PaintElement element(renderer, list.scrollbar->track, "sidebar-list-scrollbar");
    DrawScrollbar(renderer, theme, list.scrollbar->track, list.scrollbar->thumb, list.scrollbar->active);
  }
}

}  // namespace microide::workspace
