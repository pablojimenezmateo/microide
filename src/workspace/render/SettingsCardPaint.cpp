#include "workspace/render/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <utility>

#include "render/DevicePixelSnap.h"
#include "workspace/render/EditorRowUnits.h"
#include "workspace/render/SettingsCardPaint.h"

namespace microide::workspace {

using namespace detail;

namespace {

// A key under construction: one multiply per value, eight bytes of text to one.
class Key {
 public:
  explicit Key(std::uint64_t seed) : hash_(seed) {}
  Key& Add(std::uint64_t value) {
    hash_ = MixRowKey(hash_, value);
    return *this;
  }
  Key& Add(bool value) { return Add(std::uint64_t{value ? 1u : 2u}); }
  Key& Add(int value) { return Add(static_cast<std::uint64_t>(static_cast<std::int64_t>(value))); }
  Key& Add(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    return Add(std::uint64_t{bits});
  }
  Key& Add(std::string_view text) {
    hash_ = MixRowKeyBytes(hash_, text.data(), text.size());
    return *this;
  }
  Key& Add(const SDL_FRect& rect) { return Add(rect.x).Add(rect.y).Add(rect.w).Add(rect.h); }
  Key& Add(SDL_Color color) {
    return Add((static_cast<std::uint64_t>(color.r) << 24) | (static_cast<std::uint64_t>(color.g) << 16) |
               (static_cast<std::uint64_t>(color.b) << 8) | color.a);
  }
  Key& Add(const std::optional<ScrollbarGeometry>& bar, bool active) {
    Add(bar.has_value());
    if (bar.has_value()) {
      Add(bar->track).Add(bar->thumb).Add(active);
    }
    return *this;
  }
  std::uint64_t value() const { return hash_; }

 private:
  std::uint64_t hash_;
};

constexpr std::uint64_t kCardSeed = 0x73657474696e6773ull;  // "settings"

std::uint64_t RowKey(const SettingsRowViewModel& row, std::uint64_t style, bool hovered, bool pane_focused) {
  const SettingsControlViewModel& control = row.control;
  Key key(style);
  key.Add(row.row_rect)
      .Add(row.selected)
      .Add(hovered)
      .Add(row.selected && pane_focused)
      .Add(row.label)
      .Add(row.scope_label_x)
      .Add(row.scope_label)
      .Add(row.group_subheader)
      .Add(row.resettable)
      .Add(row.reset_rect)
      .Add(row.scope_rect)
      .Add(row.scope_text)
      .Add(row.scope_is_project)
      .Add(static_cast<int>(control.kind))
      .Add(control.checkbox_on)
      .Add(control.display_value)
      .Add(control.editing)
      .Add(std::string_view(control.shown_value))
      .Add(control.value_is_placeholder)
      .Add(control.caret_offset_x)
      .Add(control.checkbox_rect)
      .Add(control.dec_rect)
      .Add(control.inc_rect)
      .Add(control.value_rect);
  key.Add(static_cast<std::uint64_t>(row.description_lines.size()));
  for (const std::string_view line : row.description_lines) {
    key.Add(line);
  }
  return key.value();
}

}  // namespace

void PrepareSettingsCardFrame(PreparedFrameState& frame, SettingsCardInputs inputs) {
  PreparedFrameState::SettingsCardPaint& card = frame.settings_card;
  card.vm = std::move(inputs.vm);
  card.backdrop = inputs.backdrop;
  card.hovered_category = -1;
  card.hovered_row = -1;
  card.scrollbar_active = inputs.scrollbar_dragged;
  card.category_scrollbar_active = inputs.category_scrollbar_dragged;
  card.picker_scrollbar_active = inputs.picker_scrollbar_dragged;
  card.tooltip_visible = false;
  card.tooltip_lines.clear();
  const SettingsOverlayViewModel& vm = card.vm;
  std::vector<DerivedDamage::Unit>& units = frame.derived_damage.Begin(DerivedSurface::SettingsCard);
  if (vm.visible) {
    const render::Theme& theme = *inputs.theme;
    const render::TextRenderer& text = *inputs.text_renderer;
    const std::uint64_t style = Key(kCardSeed)
                                    .Add(theme.overlay_backdrop)
                                    .Add(theme.surface_background)
                                    .Add(theme.chrome_background)
                                    .Add(theme.editor_background)
                                    .Add(theme.border)
                                    .Add(theme.accent)
                                    .Add(theme.text_primary)
                                    .Add(theme.text_muted)
                                    .Add(theme.text_disabled)
                                    .Add(theme.selection_strong)
                                    .Add(theme.row_highlight)
                                    .Add(static_cast<std::uint64_t>(text.MetricsGeneration()))
                                    .value();
    const auto unit = [&](const SDL_FRect& rect, std::uint64_t fingerprint) {
      units.push_back(
          DerivedDamage::Unit{.rect = render::SnapToDevicePixelsF(inputs.renderer, rect), .fingerprint = fingerprint});
    };
    const bool settings = vm.mode == SettingsOverlayMode::Settings && vm.empty_label.empty();

    // The backdrop the card dims, and the card's own chrome: all it shows that is
    // not a row of its rail or of its values.
    unit(card.backdrop, style);
    {
      Key key(style);
      key.Add(vm.rect)
          .Add(static_cast<int>(vm.mode))
          .Add(vm.header_rect)
          .Add(vm.title)
          .Add(vm.filter_rect)
          .Add(static_cast<int>(vm.focused_pane))
          .Add(vm.query_empty)
          .Add(vm.query_empty ? vm.filter_placeholder : vm.query_display())
          .Add(vm.query_caret_x)
          .Add(vm.footer_rect)
          .Add(std::string_view(vm.footer_summary))
          .Add(vm.footer_hint)
          .Add(vm.empty_label)
          .Add(vm.left_pane_rect)
          .Add(vm.right_pane_rect)
          .Add(vm.section_header_rect)
          .Add(vm.section_title)
          .Add(vm.section_subtitle)
          .Add(vm.scrollbar, card.scrollbar_active)
          .Add(vm.category_scrollbar, card.category_scrollbar_active);
      if (vm.mode == SettingsOverlayMode::HelpAbout) {
        // The Help list is the card's too: its entries from the scroll row on, as
        // many as can show (an entry is at least a line tall).
        key.Add(vm.help_list_rect)
            .Add(vm.scroll_row)
            .Add(vm.help_label_column)
            .Add(vm.help_detail_x)
            .Add(vm.help_detail_width)
            .Add(vm.help_entry_gap);
        const float line_height = std::max(1.0f, text.LineHeight());
        const std::size_t first = static_cast<std::size_t>(std::max(0, vm.scroll_row));
        const std::size_t fit = static_cast<std::size_t>(vm.help_list_rect.h / line_height) + 1;
        for (std::size_t index = first; index < vm.help_rows.size() && index < first + fit; ++index) {
          key.Add(std::string_view(vm.help_rows[index].label)).Add(std::string_view(vm.help_rows[index].detail));
        }
      }
      unit(vm.rect, key.value());
    }

    // The scope chip's help, for the chip under the pointer: laid out here so the
    // card over the rows is a unit and paint draws what was keyed.
    {
      SDL_FRect tooltip_rect{};
      std::uint64_t tooltip_key = 0;
      if (settings) {
        for (const SettingsRowViewModel& row : vm.rows) {
          if (!SettingsRowShownIn(vm.right_pane_rect, row.row_rect) || row.scope_rect.w <= 0.0f ||
              row.scope_help.empty() || !inputs.pointer_over(row.scope_rect)) {
            continue;
          }
          WrappedTooltipLayout tooltip =
              BuildWrappedTooltipLayout(text, row.scope_help, std::max(240.0f, vm.rect.w * 0.45f));
          // Below the chip; above it when below would run past the card.
          const float below_y = row.scope_rect.y + row.scope_rect.h + 6.0f;
          const float above_y = row.scope_rect.y - 6.0f - tooltip.rect.h;
          const float y = (below_y + tooltip.rect.h <= vm.rect.y + vm.rect.h - 6.0f || above_y < vm.rect.y)
                              ? below_y
                              : above_y;
          const float min_x = vm.rect.x + 6.0f;
          const float x = std::clamp(row.scope_rect.x, min_x,
                                     std::max(min_x, vm.rect.x + vm.rect.w - tooltip.rect.w - 6.0f));
          card.tooltip_visible = true;
          card.tooltip_rect = MakeRect(x, y, tooltip.rect.w, tooltip.rect.h);
          card.tooltip_lines = std::move(tooltip.lines);
          tooltip_rect = MakeRect(x - 1.0f, y - 1.0f, tooltip.rect.w + 2.0f, tooltip.rect.h + 2.0f);
          tooltip_key = Key(style).Add(card.tooltip_rect).Add(row.scope_help).value();
        }
      }
      unit(tooltip_rect, tooltip_key);
    }

    // The font picker's dropdown, over the rows below its field.
    if (settings && vm.value_picker.visible) {
      const SettingsPickerViewModel& picker = vm.value_picker;
      Key key(style);
      key.Add(picker.rect).Add(picker.scrollbar, card.picker_scrollbar_active);
      for (const SettingsPickerItemViewModel& item : picker.items) {
        key.Add(item.rect).Add(item.text).Add(item.highlighted).Add(item.is_choose_file);
      }
      unit(picker.rect, key.value());
    } else {
      unit(SDL_FRect{}, 0);
    }

    if (settings) {
      // The rail: a row lit under the pointer unless it is the selected one, which
      // keeps its stronger fill (and the focus ring while the rail has the keys).
      const bool rail_focused = vm.focused_pane == SettingsPane::Categories;
      for (std::size_t index = 0; index < vm.categories.size(); ++index) {
        const SettingsCategoryViewModel& category = vm.categories[index];
        if (!SettingsRowShownIn(vm.left_pane_rect, category.rect)) {
          continue;
        }
        const bool hovered = !category.selected && inputs.pointer_over(category.rect);
        if (hovered) {
          card.hovered_category = static_cast<int>(index);
        }
        unit(category.rect, Key(style)
                                .Add(category.rect)
                                .Add(category.label)
                                .Add(category.selected)
                                .Add(hovered)
                                .Add(category.selected && rail_focused)
                                .value());
      }
      // The values: a row with the strip its sub-header takes above it, inside
      // the pane the rows are clipped to.
      const bool values_focused = vm.focused_pane == SettingsPane::Values;
      const float line_height = text.LineHeight();
      for (std::size_t index = 0; index < vm.rows.size(); ++index) {
        const SettingsRowViewModel& row = vm.rows[index];
        if (!SettingsRowShownIn(vm.right_pane_rect, row.row_rect)) {
          continue;
        }
        const bool hovered = !row.selected && inputs.pointer_over(row.row_rect);
        if (hovered) {
          card.hovered_row = static_cast<int>(index);
        }
        SDL_FRect rect = row.row_rect;
        if (!row.group_subheader.empty()) {
          rect.y -= line_height;
          rect.h += line_height;
        }
        SDL_FRect shown{};
        if (!SDL_GetRectIntersectionFloat(&rect, &vm.right_pane_rect, &shown)) {
          continue;
        }
        unit(shown, RowKey(row, style, hovered, values_focused));
      }
    }
  }
  frame.derived_damage.Commit(DerivedSurface::SettingsCard, inputs.damage);
}

}  // namespace microide::workspace
