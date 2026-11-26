#include "workspace/render/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

#include "render/DevicePixelSnap.h"
#include "render/PaintElement.h"
#include "render/ScopedRenderClip.h"
#include "workspace/render/BottomPanelTerminalGridPaint.h"
#include "workspace/render/EditorRowUnits.h"

namespace microide::workspace {

using namespace detail;

namespace {

static_assert(std::has_unique_object_representations_v<terminal::TerminalCell>,
              "a terminal cell is keyed by its bytes: no padding");

constexpr std::uint64_t kGridKeySeed = 0x7465726d67726964ull;

SDL_FRect InBody(const SDL_FRect& rect, const SDL_FRect& body) {
  SDL_FRect shown{};
  return SDL_GetRectIntersectionFloat(&rect, &body, &shown) ? shown : SDL_FRect{};
}
constexpr std::uint64_t kSelectionTag = 0x53454c;
constexpr std::uint64_t kMatchTag = 0x4d4154;
constexpr std::uint64_t kLinkTag = 0x4c4e4b;

std::uint64_t LineKey(const terminal::TerminalLine& line) {
  return MixRowKeyBytes(kGridKeySeed, reinterpret_cast<const char*>(line.cells.data()),
                        line.cells.size() * sizeof(terminal::TerminalCell));
}

TerminalPaintPalette PanePalette(const TerminalGridPanePaint& pane, const render::Theme& theme) {
  return TerminalPaintPalette{
      .foreground = theme.text_primary,
      .background = theme.surface_background,
      .selection = pane.selection_live ? theme.selection_fill : InactiveTerminalSelection(theme.selection_fill),
      .find_match = theme.search_match,
      .find_current = theme.search_match_active,
      .reverse_video = pane.reverse_video,
  };
}

void PaintRow(SDL_Renderer* renderer, const TerminalGridPanePaint& pane, const TerminalGridRowPaint& row,
              const TerminalPaintPalette& palette, const render::TextRenderer& terminal_text,
              TerminalRowPaintPlan& plan) {
  const terminal::TerminalLine& line = *row.line;
  const float x = pane.text_x;
  const float y = row.text_y;
  const float char_width = std::max(1.0f, terminal_text.CharWidth());
  if (pane.text_width < char_width) {
    return;
  }
  const auto columns = static_cast<std::size_t>(std::floor(pane.text_width / char_width));
  PlanTerminalRow(line, columns, row.emphasis, palette, plan, pane.underline_colors);
  const float line_height = terminal_text.LineHeight();
  const auto column_x = [&](std::size_t column) { return x + static_cast<float>(column) * char_width; };
  const auto span_w = [&](std::size_t begin, std::size_t end) { return static_cast<float>(end - begin) * char_width; };
  for (const TerminalRowPaintPlan::Fill& fill : plan.fills) {
    FillRect(renderer, MakeRect(column_x(fill.begin), y - 1.0f, span_w(fill.begin, fill.end), line_height),
                   fill.color);
  }
  for (const TerminalRowPaintPlan::TextRun& run : plan.text_runs) {
    const render::TextRenderer& face = terminal_text.Face(run.face);
    if (run.face == render::FontStyle::Regular || std::fabs(face.CharWidth() - char_width) < 0.01f) {
      face.DrawString(renderer, column_x(run.column), y, run.color, plan.RunText(run));
      continue;
    }
    for (std::size_t column = run.column; column < run.end && column < line.cells.size(); ++column) {
      const std::string_view glyph = line.cells[column].DisplayText();
      if (!glyph.empty() && glyph != " ") {
        face.DrawString(renderer, column_x(column), y, run.color, glyph);
      }
    }
  }
  const float thickness = std::max(1.0f, std::round(line_height / 14.0f));
  for (const TerminalRowPaintPlan::Decoration& stroke : plan.decorations) {
    const float run_x = column_x(stroke.begin);
    const float run_w = span_w(stroke.begin, stroke.end);
    if (stroke.bits & terminal::cell_attr::kStrikethrough) {
      FillRect(renderer, MakeRect(run_x, y + std::round(line_height * 0.45f), run_w, thickness), stroke.color);
    }
    const float underline_y = y + line_height - thickness - 1.0f;
    if (stroke.bits & terminal::cell_attr::kUnderline) {
      switch (stroke.variant) {
        case terminal::TerminalUnderlineVariant::Straight:
          FillRect(renderer, MakeRect(run_x, underline_y, run_w, thickness), stroke.underline_color);
          break;
        case terminal::TerminalUnderlineVariant::Curly: {
          const float amplitude = thickness * 1.5f;
          const float half = std::max(2.0f, thickness * 2.0f);
          const float top = underline_y - amplitude + thickness;
          SDL_SetRenderDrawColor(renderer, stroke.underline_color.r, stroke.underline_color.g,
                                 stroke.underline_color.b, stroke.underline_color.a);
          bool up = true;
          for (float px = run_x; px < run_x + run_w; px += half, up = !up) {
            const float next = std::min(px + half, run_x + run_w);
            for (float stroke_row = 0.0f; stroke_row < thickness; stroke_row += 1.0f) {
              render::RenderClipStableLine(renderer, px, (up ? top + amplitude : top) + stroke_row, next,
                                           (up ? top : top + amplitude) + stroke_row);
            }
          }
          break;
        }
        case terminal::TerminalUnderlineVariant::Dotted:
        case terminal::TerminalUnderlineVariant::Dashed: {
          const bool dotted = stroke.variant == terminal::TerminalUnderlineVariant::Dotted;
          const float mark = dotted ? thickness : std::max(3.0f, char_width * 0.5f);
          const float pitch = dotted ? thickness * 2.0f : mark + std::max(2.0f, char_width * 0.25f);
          for (float px = run_x; px < run_x + run_w; px += pitch) {
            FillRect(renderer, MakeRect(px, underline_y, std::min(mark, run_x + run_w - px), thickness),
                           stroke.underline_color);
          }
          break;
        }
      }
    }
    if (stroke.bits & terminal::cell_attr::kDoubleUnderline) {
      FillRect(renderer, MakeRect(run_x, y + line_height - thickness * 3.0f - 1.0f, run_w, thickness),
                     stroke.underline_color);
      FillRect(renderer, MakeRect(run_x, underline_y, run_w, thickness), stroke.underline_color);
    }
  }
  if (row.link_begin < row.link_end && row.link_begin < line.cells.size()) {
    const float link_thickness = std::max(1.0f, std::round(pane.line_height / 14.0f));
    const float link_x = column_x(row.link_begin);
    const float link_w =
        std::min(span_w(row.link_begin, row.link_end), std::max(0.0f, pane.text_x + pane.text_width - link_x));
    if (link_w > 0.0f) {
      FillRect(renderer, MakeRect(link_x, y + pane.line_height - link_thickness - 1.0f, link_w, link_thickness),
                     ResolveTerminalCellColors(line.cells[row.link_begin].style, palette).foreground);
    }
  }
}

}

void KeyTerminalGridRows(std::span<const terminal::TerminalLine> lines, std::vector<std::uint64_t>& keys) {
  keys.resize(lines.size());
  for (std::size_t index = 0; index < lines.size(); ++index) {
    keys[index] = LineKey(lines[index]);
  }
}

void PrepareTerminalGridFrame(PreparedFrameState& frame, const TerminalGridInputs& inputs) {
  PreparedFrameState::TerminalGridPaint& grid = frame.terminal_grid;
  grid.visible = false;
  grid.panes.clear();
  grid.rows.clear();
  std::vector<DerivedDamage::Unit>& units = frame.derived_damage.Begin(DerivedSurface::BottomPanelTerminalGrid);
  if (inputs.visible) {
    grid.visible = true;
    const render::Theme& theme = *inputs.theme;
    std::uint64_t style = MixRowKey(kGridKeySeed, RowKeyColorBits(theme.text_primary));
    style = MixRowKey(style, RowKeyColorBits(theme.surface_background));
    style = MixRowKey(style, RowKeyColorBits(theme.selection_fill));
    style = MixRowKey(style, RowKeyColorBits(theme.search_match));
    style = MixRowKey(style, RowKeyColorBits(theme.search_match_active));
    style = MixRowKey(style, inputs.text_metrics);
    style = MixRowKey(style, RowKeyFloatBits(inputs.char_width));
    for (const TerminalGridPaneInputs& pane : inputs.panes) {
      TerminalGridPanePaint& painted = grid.panes.emplace_back();
      painted = TerminalGridPanePaint{
          .body = pane.body,
          .text_x = pane.text_x,
          .text_width = pane.text_width,
          .line_height = pane.line_height,
          .reverse_video = pane.reverse_video,
          .selection_live = pane.selection_live,
          .underline_colors = pane.underline_colors,
          .first = grid.rows.size(),
      };
      units.push_back(DerivedDamage::Unit{
          .rect = render::SnapToDevicePixelsF(inputs.renderer, pane.body),
          .fingerprint = MixRowKey(style, pane.reverse_video ? 1u : 2u),
      });
      TerminalCaretPaint& caret = painted.caret;
      if (pane.caret_cell.has_value() &&
          pane.caret_cell->y + pane.caret_cell->h + 1.0f <= pane.body.y + pane.body.h + 0.01f) {
        caret.style = !pane.caret_owned ? TerminalCaretPaint::Style::Outline
                      : (inputs.caret_phase_on || !pane.cursor.blinking) ? TerminalCaretPaint::Style::Solid
                                                                         : TerminalCaretPaint::Style::None;
      }
      std::uint64_t caret_key = 0;
      if (caret.style != TerminalCaretPaint::Style::None) {
        caret.shape = pane.cursor.shape;
        caret.cell = *pane.caret_cell;
        SDL_FRect bounds = caret.cell;
        bounds.h += 1.0f;
        if (caret.style == TerminalCaretPaint::Style::Solid && caret.shape == terminal::TerminalCursorShape::Block &&
            pane.cursor.row >= pane.first_row && pane.cursor.row - pane.first_row < pane.lines.size()) {
          const terminal::TerminalLine& line = pane.lines[pane.cursor.row - pane.first_row];
          if (pane.cursor.column < line.cells.size() && !line.cells[pane.cursor.column].DisplayText().empty()) {
            caret.glyph = &line.cells[pane.cursor.column];
            if (caret.glyph->length > 1 && inputs.terminal_text != nullptr) {
              bounds.w = std::max(bounds.w, inputs.terminal_text->MeasureWidth(caret.glyph->DisplayText()));
            }
          }
        }
        caret.bounds = bounds;
        caret_key = MixRowKey(style, static_cast<std::uint64_t>(caret.style) << 8 |
                                         static_cast<std::uint64_t>(caret.shape));
        caret_key = MixRowKey(caret_key, RowKeyColorBits(theme.cursor));
        caret_key = MixRowKey(caret_key, RowKeyFloatBits(caret.cell.x) | RowKeyFloatBits(caret.cell.w) << 32);
        if (caret.glyph != nullptr) {
          caret_key = MixRowKeyBytes(caret_key, reinterpret_cast<const char*>(caret.glyph),
                                     sizeof(terminal::TerminalCell));
          caret_key = MixRowKey(caret_key, pane.reverse_video ? 1u : 2u);
        }
      }
      units.push_back(
          DerivedDamage::Unit{.rect = render::SnapToDevicePixelsF(inputs.renderer, InBody(caret.bounds, pane.body)),
                              .fingerprint = caret_key});
      std::uint64_t pane_style = MixRowKey(style, pane.reverse_video ? 1u : 2u);
      pane_style = MixRowKey(pane_style, RowKeyFloatBits(pane.text_x) | RowKeyFloatBits(pane.text_width) << 32);
      for (const terminal::TerminalUnderlineColor& underline : pane.underline_colors) {
        const util::Rgba8 color = underline.color.value_or(util::Rgba8{0, 0, 0, 0});
        pane_style = MixRowKey(pane_style, static_cast<std::uint64_t>(underline.extra) << 32 |
                                               static_cast<std::uint64_t>(color.r) << 24 |
                                               static_cast<std::uint64_t>(color.g) << 16 |
                                               static_cast<std::uint64_t>(color.b) << 8 | color.a);
      }
      std::size_t match_cursor = static_cast<std::size_t>(
          std::lower_bound(pane.find_matches.begin(), pane.find_matches.end(), pane.first_row,
                           [](const terminal::TerminalSearchMatch& match, std::size_t row) { return match.row < row; }) -
          pane.find_matches.begin());
      for (std::size_t slot = 0; slot < pane.visible_rows; ++slot) {
        const std::size_t absolute = pane.first_row + slot;
        if (absolute >= pane.line_count || slot >= pane.lines.size()) {
          units.push_back(DerivedDamage::Unit{});
          continue;
        }
        const float text_y = pane.text_y + static_cast<float>(slot) * pane.line_height;
        if (text_y + pane.line_height > pane.body.y + pane.body.h + 0.01f) {
          units.push_back(DerivedDamage::Unit{});
          continue;
        }
        const terminal::TerminalLine& line = pane.lines[slot];
        TerminalGridRowPaint& row = grid.rows.emplace_back();
        row.rect = MakeRect(pane.body.x, text_y - 1.0f, pane.body.w, pane.line_height + 1.0f);
        row.text_y = text_y;
        row.line = &line;
        row.emphasis = TerminalRowSelectionEmphasis(pane.selection, absolute);
        while (match_cursor < pane.find_matches.size() && pane.find_matches[match_cursor].row < absolute) {
          ++match_cursor;
        }
        std::size_t match_end = match_cursor;
        while (match_end < pane.find_matches.size() && pane.find_matches[match_end].row == absolute) {
          ++match_end;
        }
        if (match_end > match_cursor) {
          row.emphasis.matches = pane.find_matches.data() + match_cursor;
          row.emphasis.match_count = match_end - match_cursor;
          if (pane.find_selected >= match_cursor && pane.find_selected < match_end) {
            row.emphasis.current_match = pane.find_selected - match_cursor;
          }
        }
        if (pane.hovered_link.has_value() && pane.hovered_link->row <= absolute &&
            absolute <= pane.hovered_link->end_row) {
          const TerminalLinkSpan& link = *pane.hovered_link;
          row.link_begin = absolute == link.row ? link.begin_column : 0;
          row.link_end = absolute == link.end_row ? link.end_column : line.cells.size();
        }

        std::uint64_t key = MixRowKey(pane_style, slot < pane.row_keys.size() ? pane.row_keys[slot] : LineKey(line));
        if (row.emphasis.selection_begin != row.emphasis.selection_end || row.emphasis.selection_runs_past_row) {
          key = MixRowKey(key, kSelectionTag);
          key = MixRowKey(key, row.emphasis.selection_begin);
          key = MixRowKey(key, row.emphasis.selection_end);
          key = MixRowKey(key, (row.emphasis.selection_runs_past_row ? 1u : 0u) | (pane.selection_live ? 2u : 0u));
        }
        if (row.emphasis.match_count > 0) {
          key = MixRowKey(key, kMatchTag);
          for (std::size_t match = 0; match < row.emphasis.match_count; ++match) {
            key = MixRowKey(key, static_cast<std::uint64_t>(row.emphasis.matches[match].column) << 32 ^
                                     row.emphasis.matches[match].length);
          }
          key = MixRowKey(key, row.emphasis.current_match);
        }
        if (row.link_begin < row.link_end) {
          key = MixRowKey(key, kLinkTag);
          key = MixRowKey(key, row.link_begin);
          key = MixRowKey(key, row.link_end);
        }
        units.push_back(DerivedDamage::Unit{
            .rect = render::SnapToDevicePixelsF(inputs.renderer, InBody(row.rect, pane.body)), .fingerprint = key});
      }
      painted.count = grid.rows.size() - painted.first;
    }
  }
  frame.derived_damage.Commit(DerivedSurface::BottomPanelTerminalGrid, inputs.damage);
}

void PaintTerminalGridCaret(SDL_Renderer* renderer, const PreparedFrameState& frame, std::size_t pane_index,
                            const render::Theme& theme, const render::TextRenderer& terminal_text) {
  const PreparedFrameState::TerminalGridPaint& grid = frame.terminal_grid;
  if (!grid.visible || pane_index >= grid.panes.size()) {
    return;
  }
  const TerminalGridPanePaint& pane = grid.panes[pane_index];
  const TerminalCaretPaint& caret = pane.caret;
  if (caret.style == TerminalCaretPaint::Style::None) {
    return;
  }
  const render::ScopedRenderClip body_clip(renderer, pane.body);
  const render::PaintElement element(renderer, caret.bounds, "terminal-caret");
  const SDL_FRect shape_rect = TerminalCursorShapeRect(caret.cell, caret.shape);
  const bool block = caret.shape == terminal::TerminalCursorShape::Block;
  if (caret.style == TerminalCaretPaint::Style::Outline) {
    if (block) {
      OutlineRect(renderer, caret.cell, theme.cursor);
    } else {
      FillRect(renderer, shape_rect, theme.cursor);
    }
    return;
  }
  FillRect(renderer, shape_rect, theme.cursor);
  if (caret.glyph != nullptr) {
    terminal_text.DrawString(renderer, caret.cell.x, caret.cell.y + 1.0f,
                             ResolveTerminalCellColors(caret.glyph->style, PanePalette(pane, theme)).background,
                             caret.glyph->DisplayText());
  }
}

void PaintTerminalGridPane(SDL_Renderer* renderer, const PreparedFrameState& frame, std::size_t pane_index,
                           const render::Theme& theme, const render::TextRenderer& terminal_text,
                           TerminalRowPaintPlan& plan) {
  const PreparedFrameState::TerminalGridPaint& grid = frame.terminal_grid;
  if (!grid.visible || pane_index >= grid.panes.size()) {
    return;
  }
  const TerminalGridPanePaint& pane = grid.panes[pane_index];
  const TerminalPaintPalette palette = PanePalette(pane, theme);
  if (pane.reverse_video) {
    const render::PaintElement element(renderer, pane.body, "terminal-body");
    FillRect(renderer, pane.body, theme.text_primary);
  }
  const render::ScopedRenderClip body_clip(renderer, pane.body);
  std::optional<SDL_Rect> clip;
  if (SDL_RenderClipEnabled(renderer)) {
    SDL_Rect current{};
    if (SDL_GetRenderClipRect(renderer, &current)) {
      clip = current;
    }
  }
  for (std::size_t index = pane.first; index < pane.first + pane.count; ++index) {
    const TerminalGridRowPaint& row = grid.rows[index];
    if (clip.has_value() && (row.rect.y + row.rect.h + pane.line_height <= static_cast<float>(clip->y) ||
                             row.rect.y - pane.line_height >= static_cast<float>(clip->y + clip->h))) {
      continue;
    }
    const render::PaintElement element(renderer, row.rect, "terminal-row");
    PaintRow(renderer, pane, row, palette, terminal_text, plan);
  }
}

}
