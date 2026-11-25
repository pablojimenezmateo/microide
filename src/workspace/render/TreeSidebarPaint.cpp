#include "workspace/render/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string_view>

#include "editor/GutterIconRegistry.h"
#include "render/DevicePixelSnap.h"
#include "render/PaintElement.h"
#include "workspace/render/EditorRowUnits.h"
#include "workspace/render/TreeSidebarPaint.h"

namespace microide::workspace {

using namespace detail;

namespace {

std::uint64_t ButtonKey(const TreeSidebarButtonPaint& button, std::uint64_t style) {
  return MixRowKey(MixRowKeyText(MixRowKeyRect(style, button.rect), button.label),
                   (button.enabled ? 1u : 0u) | (button.hovered ? 2u : 0u));
}

std::uint64_t RowKey(const TreeSidebarRowPaint& row, std::uint64_t style) {
  std::uint64_t key = MixRowKeyText(MixRowKeyRect(style, row.rect), row.label);
  key = MixRowKey(key, RowKeyFloatBits(row.indent));
  key = MixRowKey(key, (row.is_directory ? 1u : 0u) | (row.expanded ? 2u : 0u) | (row.selected ? 4u : 0u) |
                           (row.hovered ? 8u : 0u) | (row.muted ? 16u : 0u) | (row.has_icon ? 32u : 0u) |
                           static_cast<std::uint64_t>(row.git_status) << 8);
  key = MixRowKey(key, RowKeyColorBits(row.git_color));
  if (row.has_icon) {
    key = MixRowKey(key, static_cast<std::uint64_t>(row.icon_shape) << 32 | RowKeyColorBits(row.icon_color));
  }
  return key;
}

std::uint64_t ScrollbarKey(const PanelScrollbarPaint& bar, std::uint64_t style) {
  return MixRowKey(MixRowKeyRect(MixRowKeyRect(style, bar.track), bar.thumb), bar.active ? 1u : 2u);
}

void PaintTreeRow(SDL_Renderer* renderer, const TreeSidebarRowPaint& row, const render::Theme& theme,
                  const render::TextRenderer& tr) {
  const SDL_FRect& rect = row.rect;
  const bool emphasized = row.selected || row.hovered;
  const SDL_Color background = emphasized ? theme.row_highlight : theme.surface_background;
  DrawSelectableRowBackground(renderer, theme, rect, theme.surface_background, emphasized, row.selected);

  const float right = rect.x + rect.w;
  const float tree_x = rect.x + 6.0f + row.indent;
  const float label_x = tree_x + kWorkspaceSidebarTreeChevronSlotWidth + 4.0f;
  const char git_marker = GitMarker(row.git_status);
  const bool has_git_marker = git_marker != ' ';
  const std::string_view git_marker_text = has_git_marker ? std::string_view(&git_marker, 1) : std::string_view{};
  const float marker_width = has_git_marker ? tr.MeasureWidth(git_marker_text) : 0.0f;
  const float marker_x = right - marker_width - 8.0f;
  const float label_width =
      has_git_marker ? std::max(0.0f, marker_x - label_x - 8.0f) : std::max(0.0f, right - label_x - 8.0f);

  if (label_x <= right) {
    if (row.is_directory) {
      DrawChevron(renderer, tree_x, rect.y + rect.h * 0.5f, row.expanded,
                  row.selected ? theme.text_primary : theme.text_muted);
    } else if (row.has_icon) {
      editor::GutterIconRegistry::Draw(renderer, row.icon_shape, row.selected ? theme.text_primary : row.icon_color,
                                       tree_x, rect.y, kWorkspaceSidebarTreeChevronSlotWidth, rect.h);
    }
  }
  if (label_width > 0.0f) {
    DrawVCenteredTextOn(tr, renderer, MakeRect(label_x, rect.y, label_width, rect.h), 0.0f,
                        row.selected ? theme.text_primary
                                     : (row.muted ? theme.text_muted
                                                  : (row.is_directory ? theme.text_primary : theme.text_secondary)),
                        background, tr.TruncateToWidthEphemeralView(row.label, label_width));
  }
  if (has_git_marker && marker_x >= rect.x) {
    DrawVCenteredTextOn(tr, renderer, MakeRect(marker_x, rect.y, marker_width, rect.h), 0.0f,
                        row.selected ? theme.text_primary : row.git_color, background, git_marker_text);
  }
}

}

void PrepareTreeSidebarFrame(PreparedFrameState& frame, const TreeSidebarInputs& inputs) {
  PreparedFrameState::TreeSidebarPaint& tree = frame.tree_sidebar;
  tree.visible = false;
  tree.buttons = {};
  tree.rows.clear();
  tree.scrollbar.reset();
  std::vector<DerivedDamage::Unit>& units = frame.derived_damage.Begin(DerivedSurface::SidebarTree);
  if (inputs.visible) {
    tree.visible = true;
    const render::Theme& theme = *inputs.theme;
    const bool compact = inputs.collapse_button.w <= 24.0f || inputs.refresh_button.w <= 24.0f;
    const auto button = [&](const SDL_FRect& rect, std::string_view label, bool enabled) {
      return TreeSidebarButtonPaint{
          .rect = rect, .label = label, .enabled = enabled, .hovered = enabled && inputs.pointer_over(rect)};
    };
    tree.buttons[0] = button(inputs.collapse_button, compact ? "C" : "Collapse", inputs.can_collapse_all);
    tree.buttons[1] = button(inputs.refresh_button, compact ? "R" : "Refresh", true);

    const ScrollableListLayout& list = inputs.list;
    for (int visible = 0; visible < list.visible_rows; ++visible) {
      const int index = list.scroll_row + visible;
      if (index < 0 || static_cast<std::size_t>(index) >= inputs.entries.size()) {
        break;
      }
      const auto entry_index = static_cast<std::size_t>(index);
      const project::TreeEntry& entry = inputs.entries[entry_index];
      TreeSidebarRowPaint row{
          .rect = ScrollableListRowRect(list, visible),
          .label = entry.label,
          .indent = static_cast<float>(entry.depth) * kWorkspaceSidebarTreeIndentWidth,
          .is_directory = entry.is_directory,
          .expanded = entry.expanded,
          .selected = entry_index == inputs.selected_index,
          .muted = entry.ignored || entry.pending,
          .git_status = entry.git_status,
          .git_color = GitMarkerColor(theme, entry.git_status),
      };
      row.hovered = inputs.pointer_over(row.rect);
      if (!entry.is_directory && inputs.icons != nullptr && entry_index < inputs.icons->size() &&
          (*inputs.icons)[entry_index].has_value()) {
        row.has_icon = true;
        row.icon_shape = (*inputs.icons)[entry_index]->shape;
        row.icon_color = (*inputs.icons)[entry_index]->color;
      }
      tree.rows.push_back(row);
    }
    if (const auto geometry = MakeVerticalScrollbarGeometry(list.list_rect, static_cast<float>(inputs.entries.size()),
                                                            list.visible_units, static_cast<float>(list.scroll_row),
                                                            false);
        geometry.has_value()) {
      tree.scrollbar =
          PanelScrollbarPaint{.track = geometry->track, .thumb = geometry->thumb, .active = inputs.scrollbar_active};
    }

    const std::uint64_t style = PaintFingerprint()
                                    .Add(theme.surface_background)
                                    .Add(theme.surface_raised)
                                    .Add(theme.row_highlight)
                                    .Add(theme.selection_fill)
                                    .Add(theme.accent)
                                    .Add(theme.border)
                                    .Add(theme.text_primary)
                                    .Add(theme.text_secondary)
                                    .Add(theme.text_muted)
                                    .Add(theme.text_disabled)
                                    .Add(inputs.text_renderer->MetricsGeneration())
                                    .value();
    const auto unit = [&](const SDL_FRect& rect, std::uint64_t fingerprint) {
      units.push_back(
          DerivedDamage::Unit{.rect = render::SnapToDevicePixelsF(inputs.renderer, rect), .fingerprint = fingerprint});
    };
    for (const TreeSidebarButtonPaint& painted : tree.buttons) {
      unit(painted.rect, ButtonKey(painted, style));
    }
    unit(tree.scrollbar.has_value() ? tree.scrollbar->track : SDL_FRect{},
         tree.scrollbar.has_value() ? ScrollbarKey(*tree.scrollbar, style) : 0);
    for (const TreeSidebarRowPaint& row : tree.rows) {
      unit(row.rect, RowKey(row, style));
    }
  }
  frame.derived_damage.Commit(DerivedSurface::SidebarTree, inputs.damage);
}

void PaintTreeSidebar(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                      const render::TextRenderer& text_renderer) {
  const PreparedFrameState::TreeSidebarPaint& tree = frame.tree_sidebar;
  if (!tree.visible) {
    return;
  }
  for (const TreeSidebarButtonPaint& button : tree.buttons) {
    if (button.rect.w <= 0.0f || button.rect.h <= 0.0f) {
      continue;
    }
    const render::PaintElement element(renderer, button.rect, "tree-button");
    DrawButtonCentered(text_renderer, renderer, theme, button.rect, button.label, ButtonTone::Neutral,
                       ButtonVisualState{.enabled = button.enabled, .hovered = button.hovered, .active = false});
  }
  for (const TreeSidebarRowPaint& row : tree.rows) {
    const render::PaintElement element(renderer, row.rect, "tree-row");
    PaintTreeRow(renderer, row, theme, text_renderer);
  }
  if (tree.scrollbar.has_value()) {
    const render::PaintElement element(renderer, tree.scrollbar->track, "tree-scrollbar");
    DrawScrollbar(renderer, theme, tree.scrollbar->track, tree.scrollbar->thumb, tree.scrollbar->active);
  }
}

}
