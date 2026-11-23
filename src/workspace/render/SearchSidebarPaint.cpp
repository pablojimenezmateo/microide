#include "workspace/render/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <string_view>

#include "render/DevicePixelSnap.h"
#include "render/PaintElement.h"
#include "workspace/render/EditorRowUnits.h"
#include "workspace/render/SearchSidebarPaint.h"
#include "workspace/render/SingleLineViewMetrics.h"

// The sidebar's Search panel. Prep lays it out into the frame's snapshot
// (PrepareSearchSidebarFrame), copies the text it shows and derives its damage
// from it; paint draws the snapshot only. A row repaints when what it shows
// changes, whoever changed it: a batch of hits from the search's workers, the
// selection a key moved, the pointer.

namespace microide::workspace {

using namespace detail;

namespace {

// A unit's rect: the element's, snapped as its PaintElement clips (clip-discipline D2).
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

std::uint64_t FieldKey(const SearchSidebarFieldPaint& field, std::uint64_t caret_key, std::uint64_t style) {
  return MixRowKey(MixRowKey(MixText(MixRect(MixRect(style, field.rect), field.bounds), field.text),
                             (field.focused ? 1u : 0u) | (field.editing ? 2u : 0u)),
                   caret_key);
}

std::uint64_t ButtonKey(const SearchSidebarButtonPaint& button, std::uint64_t style) {
  return MixRowKey(MixText(MixRect(style, button.rect), button.label),
                   (button.enabled ? 1u : 0u) | (button.hovered ? 2u : 0u) | (button.active ? 4u : 0u));
}

// Everything PaintRow reads of a row.
std::uint64_t RowKey(const SearchSidebarRowPaint& row, std::uint64_t style) {
  std::uint64_t key = MixText(MixRect(style, row.rect), row.text);
  key = MixRowKey(key, (row.file ? 1u : 0u) | (row.selected ? 2u : 0u) | (row.hovered ? 4u : 0u));
  return MixRowKey(key, static_cast<std::uint64_t>(row.match_start) << 32 ^ row.match_length);
}

std::uint64_t ScrollbarKey(const PanelScrollbarPaint& bar, std::uint64_t style) {
  return MixRowKey(MixRect(MixRect(style, bar.track), bar.thumb), bar.active ? 1u : 2u);
}

// Where a field's text starts under its frame's top (the text-input pass puts
// the focused field's caret and selection on the same line).
constexpr float kFieldTextTop = 3.0f;

std::size_t DecimalDigits(std::size_t value) {
  std::size_t digits = 1;
  while (value >= 10) {
    value /= 10;
    ++digits;
  }
  return digits;
}

// One visible line of the list: a file's header (the path of the result that
// follows it) or a match. Null when the map names a result that is not there.
struct RowSource {
  const project::ProjectSearchResult* result = nullptr;
  bool file = false;
};
RowSource SourceOfLine(const SearchSidebarInputs& inputs, std::size_t line) {
  const auto result_at = [&](int index) -> const project::ProjectSearchResult* {
    return index >= 0 && static_cast<std::size_t>(index) < inputs.results.size()
               ? &inputs.results[static_cast<std::size_t>(index)]
               : nullptr;
  };
  const int index = inputs.line_map[line];
  if (index >= 0) {
    return RowSource{.result = result_at(index), .file = false};
  }
  const std::size_t next = std::min(line + 1, inputs.line_map.size() - 1);
  return RowSource{.result = result_at(inputs.line_map[next]), .file = true};
}

void PaintField(SDL_Renderer* renderer, const SearchSidebarFieldPaint& field, const render::Theme& theme,
                const render::TextRenderer& tr) {
  DrawTextFieldFrame(renderer, theme, field.rect, field.focused);
  // The tail of its value that fits, as every unfocused single-line field draws
  // (the focused one's text arrives already cut to its caret's window).
  const float available = std::max(1.0f, field.rect.w - 12.0f);
  if (field.text.empty()) {
    return;
  }
  const std::size_t start =
      FittingTailStart(field.text, available, [&tr](std::string_view tail) { return tr.MeasureWidth(tail); });
  if (start < field.text.size()) {
    tr.DrawString(renderer, field.rect.x + 6.0f, field.rect.y + kFieldTextTop,
                  field.editing ? theme.text_primary : theme.text_secondary, field.text.substr(start));
  }
}

void PaintRow(SDL_Renderer* renderer, const SearchSidebarRowPaint& row, const render::Theme& theme,
              const render::TextRenderer& tr) {
  const SDL_FRect& rect = row.rect;
  if (row.file) {
    DrawVCenteredTextOn(tr, renderer, rect, 4.0f, theme.text_primary, theme.surface_background,
                        tr.TruncateToWidthEphemeralView(row.text, rect.w - 8.0f));
    return;
  }
  const bool emphasized = row.selected || row.hovered;
  DrawSelectableRowBackground(renderer, theme, rect, theme.surface_background, emphasized, row.selected);
  // The matched span inside the preview, in the editor's search-match colour.
  // Positions assume the monospace sidebar font; a non-ASCII preview degrades to
  // a slight horizontal offset.
  if (row.match_length > 0) {
    const float char_width = std::max(1.0f, tr.CharWidth());
    const float text_x = rect.x + 6.0f;
    const float text_right = rect.x + rect.w - 6.0f;
    const float highlight_x = text_x + static_cast<float>(row.match_start) * char_width;
    const float highlight_w = static_cast<float>(row.match_length) * char_width;
    if (highlight_x < text_right && highlight_w > 0.0f) {
      FillRect(renderer,
               MakeRect(highlight_x, rect.y + 2.0f, std::min(highlight_w, text_right - highlight_x), rect.h - 4.0f),
               theme.search_match);
    }
  }
  DrawVCenteredTextOn(tr, renderer, rect, 6.0f, emphasized ? theme.text_primary : theme.text_secondary,
                      emphasized ? theme.row_highlight : theme.surface_background,
                      tr.TruncateToWidthEphemeralView(row.text, rect.w - 12.0f));
}

}  // namespace

void PrepareSearchSidebarFrame(PreparedFrameState& frame, const SearchSidebarInputs& inputs) {
  PreparedFrameState::SearchSidebarPaint& panel = frame.search_sidebar;
  panel.visible = false;
  panel.fields = {};
  panel.buttons = {};
  panel.status_rect = {};
  panel.status = {};
  panel.rows.clear();
  panel.placeholder_rect = {};
  panel.placeholder = {};
  panel.placeholder_error = false;
  panel.scrollbar.reset();
  panel.text.clear();
  // Every frame, shown or not: a panel that is not shown has no units, so the
  // frame that hides it repaints where its rows were.
  std::vector<DerivedDamage::Unit>& units = frame.derived_damage.Begin(DerivedSurface::SidebarSearch);
  if (inputs.visible) {
    panel.visible = true;
    const render::Theme& theme = *inputs.theme;
    const ScrollableListLayout& list = inputs.list;
    const std::size_t first_line = static_cast<std::size_t>(std::max(0, list.scroll_row));
    const std::size_t shown_lines =
        first_line < inputs.line_map.size()
            ? std::min(inputs.line_map.size() - first_line, static_cast<std::size_t>(std::max(0, list.visible_rows)))
            : 0;
    const bool empty = inputs.line_map.empty();

    // The panel owns the text it paints. Reserved whole first: the views taken
    // below are into `panel.text`, which must not grow under them.
    std::size_t text_size = inputs.status.size() + (empty ? inputs.empty_text.size() : 0);
    for (const SearchSidebarFieldInputs& field : inputs.fields) {
      text_size += field.text.size();
    }
    for (const SearchSidebarButtonPaint& button : inputs.buttons) {
      text_size += button.label.size();
    }
    for (std::size_t offset = 0; offset < shown_lines; ++offset) {
      const RowSource source = SourceOfLine(inputs, first_line + offset);
      if (source.result == nullptr) {
        continue;
      }
      text_size += source.file ? source.result->relative_path_string.size()
                               : DecimalDigits(source.result->line + 1) + 1 +
                                     DecimalDigits(source.result->column + 1) + 2 + source.result->preview.size();
    }
    panel.text.reserve(text_size);
    const auto own = [&panel](std::string_view text) {
      const std::size_t at = panel.text.size();
      panel.text.append(text);
      return std::string_view(panel.text).substr(at, text.size());
    };
    const auto append_number = [&panel](std::size_t value) {
      char digits[24];
      const auto end = std::to_chars(digits, digits + sizeof digits, value).ptr;
      panel.text.append(digits, static_cast<std::size_t>(end - digits));
    };

    for (std::size_t index = 0; index < inputs.fields.size(); ++index) {
      const SearchSidebarFieldInputs& field = inputs.fields[index];
      if (field.rect.w > 0.0f && field.rect.h > 0.0f) {
        SDL_FRect bounds = field.rect;
        bounds.h = std::max(bounds.h, kFieldTextTop + inputs.text_renderer->LineHeight());
        panel.fields[index] = SearchSidebarFieldPaint{.rect = field.rect,
                                                      .bounds = bounds,
                                                      .text = own(field.text),
                                                      .focused = field.focused,
                                                      .editing = field.editing};
      }
    }
    for (std::size_t index = 0; index < inputs.buttons.size(); ++index) {
      SearchSidebarButtonPaint button = inputs.buttons[index];
      button.label = own(button.label);
      button.hovered = button.rect.w > 0.0f && button.rect.h > 0.0f && inputs.pointer_over(button.rect);
      panel.buttons[index] = button;
    }
    panel.status_rect = inputs.status_rect;
    panel.status = own(inputs.status);
    for (std::size_t offset = 0; offset < shown_lines; ++offset) {
      const RowSource source = SourceOfLine(inputs, first_line + offset);
      if (source.result == nullptr) {
        continue;
      }
      SearchSidebarRowPaint row{.rect = ScrollableListRowRect(list, static_cast<int>(offset)), .file = source.file};
      if (source.file) {
        row.text = own(source.result->relative_path_string);
      } else {
        // "line:col  preview", the match's place counted from the label's start.
        const std::size_t at = panel.text.size();
        append_number(source.result->line + 1);
        panel.text.append(":");
        append_number(source.result->column + 1);
        panel.text.append("  ");
        const std::size_t prefix = panel.text.size() - at;
        panel.text.append(source.result->preview);
        row.text = std::string_view(panel.text).substr(at);
        row.selected =
            static_cast<std::size_t>(inputs.line_map[first_line + offset]) == inputs.selected_index;
        row.hovered = inputs.pointer_over(row.rect);
        if (source.result->match_preview_length > 0) {
          row.match_start = prefix + source.result->match_preview_start;
          row.match_length = source.result->match_preview_length;
        }
      }
      panel.rows.push_back(row);
    }
    if (empty && !inputs.empty_text.empty()) {
      panel.placeholder_rect = inputs.placeholder_rect;
      panel.placeholder = own(inputs.empty_text);
      panel.placeholder_error = inputs.empty_is_error;
    }
    if (const auto geometry =
            MakeVerticalScrollbarGeometry(list.list_rect, static_cast<float>(inputs.line_map.size()),
                                          list.visible_units, static_cast<float>(list.scroll_row), false);
        geometry.has_value()) {
      panel.scrollbar =
          PanelScrollbarPaint{.track = geometry->track, .thumb = geometry->thumb, .active = inputs.scrollbar_active};
    }

    // What the panel draws with besides each element's own values.
    const std::uint64_t style = PaintFingerprint()
                                    .Add(theme.surface_background)
                                    .Add(theme.surface_raised)
                                    .Add(theme.row_highlight)
                                    .Add(theme.selection_fill)
                                    .Add(theme.search_match)
                                    .Add(theme.accent)
                                    .Add(theme.border)
                                    .Add(theme.text_primary)
                                    .Add(theme.text_secondary)
                                    .Add(theme.text_muted)
                                    .Add(theme.text_disabled)
                                    .Add(theme.diff_deleted)
                                    .Add(inputs.text_renderer->MetricsGeneration())
                                    .value();
    const auto unit = [&](const SDL_FRect& rect, std::uint64_t fingerprint) {
      units.push_back(DerivedDamage::Unit{.rect = UnitRect(inputs.renderer, rect), .fingerprint = fingerprint});
    };
    // The fixed pieces first, each in its own slot whatever the list holds.
    for (std::size_t index = 0; index < panel.fields.size(); ++index) {
      unit(panel.fields[index].bounds, FieldKey(panel.fields[index], inputs.fields[index].caret_key, style));
    }
    for (const SearchSidebarButtonPaint& button : panel.buttons) {
      unit(button.rect, ButtonKey(button, style));
    }
    unit(panel.status_rect, MixText(MixRect(style, panel.status_rect), panel.status));
    unit(panel.scrollbar.has_value() ? panel.scrollbar->track : SDL_FRect{},
         panel.scrollbar.has_value() ? ScrollbarKey(*panel.scrollbar, style) : 0);
    unit(panel.placeholder_rect, MixRowKey(MixText(MixRect(style, panel.placeholder_rect), panel.placeholder),
                                           panel.placeholder_error ? 1u : 2u));
    for (const SearchSidebarRowPaint& row : panel.rows) {
      unit(row.rect, RowKey(row, style));
    }
  }
  frame.derived_damage.Commit(DerivedSurface::SidebarSearch, inputs.damage);
}

void PaintSearchSidebar(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                        const render::TextRenderer& text_renderer) {
  const PreparedFrameState::SearchSidebarPaint& panel = frame.search_sidebar;
  if (!panel.visible) {
    return;
  }
  for (const SearchSidebarFieldPaint& field : panel.fields) {
    if (field.rect.w <= 0.0f || field.rect.h <= 0.0f) {
      continue;
    }
    const render::PaintElement element(renderer, field.bounds, "search-field");
    PaintField(renderer, field, theme, text_renderer);
  }
  for (const SearchSidebarButtonPaint& button : panel.buttons) {
    if (button.rect.w <= 0.0f || button.rect.h <= 0.0f) {
      continue;
    }
    const render::PaintElement element(renderer, button.rect, "search-button");
    DrawButtonCentered(text_renderer, renderer, theme, button.rect, button.label, ButtonTone::Neutral,
                       ButtonVisualState{.enabled = button.enabled, .hovered = button.hovered, .active = button.active});
  }
  if (!panel.status.empty() && panel.status_rect.w > 0.0f && panel.status_rect.h > 0.0f) {
    const render::PaintElement element(renderer, panel.status_rect, "search-status");
    DrawTextOn(text_renderer, renderer, panel.status_rect.x, panel.status_rect.y, theme.text_muted,
               theme.surface_background,
               text_renderer.TruncateToWidthEphemeralView(panel.status, panel.status_rect.w));
  }
  for (const SearchSidebarRowPaint& row : panel.rows) {
    const render::PaintElement element(renderer, row.rect, "search-row");
    PaintRow(renderer, row, theme, text_renderer);
  }
  if (!panel.placeholder.empty()) {
    const render::PaintElement element(renderer, panel.placeholder_rect, "search-placeholder");
    DrawWrappedPlaceholderIn(text_renderer, renderer, panel.placeholder_rect,
                             panel.placeholder_error ? theme.diff_deleted : theme.text_muted, theme.surface_background,
                             panel.placeholder);
  }
  if (panel.scrollbar.has_value()) {
    const render::PaintElement element(renderer, panel.scrollbar->track, "search-scrollbar");
    DrawScrollbar(renderer, theme, panel.scrollbar->track, panel.scrollbar->thumb, panel.scrollbar->active);
  }
}

}  // namespace microide::workspace
