#include "workspace/render/TerminalRowPaint.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "render/ColorMath.h"
#include "render/SdlConvert.h"

namespace microide::workspace {

namespace {

bool SameColor(SDL_Color a, SDL_Color b) {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

enum class Emphasis : std::uint8_t { None, FindMatch, FindCurrent, Selection };

// The column whose emphasis a cell shows: a double-width glyph's trailing half
// shows its lead's, so the glyph is emphasized whole -- and exactly when the copy
// takes it, which keys on the lead (TerminalLineSliceText skips the trailing half).
std::size_t EmphasisColumn(const terminal::TerminalLine& line, std::size_t column) {
  return column > 0 && column < line.cells.size() && line.cells[column].style.wide_trailing()
             ? column - 1
             : column;
}

// An explicit selection outranks a find hit: the user made it, and it is what
// they are about to copy.
Emphasis EmphasisAt(const TerminalRowEmphasis& emphasis, std::size_t column) {
  if (column >= emphasis.selection_begin && column < emphasis.selection_end) {
    return Emphasis::Selection;
  }
  for (std::size_t index = 0; index < emphasis.match_count; ++index) {
    const terminal::TerminalSearchMatch& match = emphasis.matches[index];
    if (column >= match.column && column < match.column + match.length) {
      return index == emphasis.current_match ? Emphasis::FindCurrent : Emphasis::FindMatch;
    }
  }
  return Emphasis::None;
}

SDL_Color WashFor(Emphasis emphasis, const TerminalPaintPalette& palette) {
  switch (emphasis) {
    case Emphasis::Selection:
      return palette.selection;
    case Emphasis::FindMatch:
      return palette.find_match;
    case Emphasis::FindCurrent:
      return palette.find_current;
    case Emphasis::None:
      break;
  }
  return SDL_Color{0, 0, 0, 0};
}

SDL_Color LegibleOn(SDL_Color foreground, SDL_Color background, TerminalRowPaintPlan::Scratch& scratch) {
  const auto pack = [](SDL_Color c) {
    return static_cast<std::uint64_t>(c.r) << 24 | static_cast<std::uint64_t>(c.g) << 16 |
           static_cast<std::uint64_t>(c.b) << 8 | c.a;
  };
  const std::uint64_t key = pack(foreground) << 32 | pack(background);
  auto& entry = scratch.contrast[(key ^ (key >> 29) ^ (key >> 13)) % scratch.contrast.size()];
  if (entry.key != key) {
    entry.key = key;
    entry.result = render::EnsureContrast(foreground, background, kTerminalEmphasisMinimumContrast);
  }
  return entry.result;
}

}  // namespace

SDL_Color InactiveTerminalSelection(SDL_Color selection) {
  selection.a = static_cast<Uint8>(selection.a / 2);
  return selection;
}

SDL_FRect TerminalCursorShapeRect(SDL_FRect cell, terminal::TerminalCursorShape shape) {
  switch (shape) {
    case terminal::TerminalCursorShape::Bar:
      cell.w = std::min(cell.w, std::max(2.0f, std::round(cell.w / 8.0f)));
      break;
    case terminal::TerminalCursorShape::Underline: {
      const float thickness = std::min(cell.h, std::max(2.0f, std::round(cell.h / 10.0f)));
      cell.y += cell.h - thickness;
      cell.h = thickness;
      break;
    }
    case terminal::TerminalCursorShape::Block:
      break;
  }
  return cell;
}

TerminalCellColors ResolveTerminalCellColors(const terminal::TerminalStyle& style,
                                             const TerminalPaintPalette& palette) {
  // The terminal model carries util::Rgba8 (it is kernel code and names no SDL
  // type); this is that boundary.
  TerminalCellColors colors{
      .foreground = style.foreground.has_value() ? render::ToSdlColor(*style.foreground)
                                                 : palette.foreground,
      .background = style.background.has_value() ? render::ToSdlColor(*style.background)
                                                 : palette.background,
  };
  // DECSCNM reverses the whole screen, an explicitly inverse cell included.
  if (style.inverse() != palette.reverse_video) {
    std::swap(colors.foreground, colors.background);
  }
  if (style.dim()) {
    colors.foreground = render::BlendColors(colors.foreground, colors.background, 0.40f);
  }
  if (style.hidden()) {
    colors.foreground = colors.background;
  }
  return colors;
}

TerminalRowEmphasis TerminalRowSelectionEmphasis(const std::optional<TerminalSelectionBounds>& selection,
                                                 std::size_t row) {
  TerminalRowEmphasis emphasis;
  if (!selection.has_value() || row < selection->start.row || row > selection->end.row) {
    return emphasis;
  }
  emphasis.selection_begin = row == selection->start.row ? selection->start.column : 0;
  emphasis.selection_runs_past_row = row < selection->end.row;
  emphasis.selection_end =
      emphasis.selection_runs_past_row ? TerminalRowEmphasis::kToRowEnd : selection->end.column;
  if (emphasis.selection_end < emphasis.selection_begin) {
    emphasis.selection_end = emphasis.selection_begin;
  }
  return emphasis;
}

void PlanTerminalRow(const terminal::TerminalLine& line,
                     std::size_t columns,
                     const TerminalRowEmphasis& emphasis,
                     const TerminalPaintPalette& palette,
                     TerminalRowPaintPlan& plan,
                     std::span<const terminal::TerminalUnderlineColor> underline_colors) {
  plan.fills.clear();
  plan.text_runs.clear();
  plan.decorations.clear();
  plan.text.clear();
  const std::size_t visible = std::min(line.cells.size(), columns);
  const SDL_Color base = palette.base();

  // 1) Every visible cell's final colours, once.
  std::vector<TerminalCellColors>& cells = plan.scratch.cells;
  cells.resize(visible);
  for (std::size_t column = 0; column < visible; ++column) {
    const terminal::TerminalStyle& style = line.cells[column].style;
    TerminalCellColors colors = ResolveTerminalCellColors(style, palette);
    const Emphasis kind = EmphasisAt(emphasis, EmphasisColumn(line, column));
    if (kind != Emphasis::None) {
      colors.background = render::CompositeOver(WashFor(kind, palette), colors.background);
      colors.foreground = style.hidden() ? colors.background
                                         : LegibleOn(colors.foreground, colors.background, plan.scratch);
    }
    cells[column] = colors;
  }

  // 2) Background fills, run-length coalesced; the base colour is already there.
  for (std::size_t column = 0; column < visible;) {
    const SDL_Color color = cells[column].background;
    std::size_t end = column + 1;
    while (end < visible && SameColor(cells[end].background, color)) {
      ++end;
    }
    if (!SameColor(color, base)) {
      plan.fills.push_back({.begin = column, .end = end, .color = color});
    }
    column = end;
  }
  // The selected line break: wash the blank past the row's last cell.
  if (emphasis.selection_runs_past_row) {
    const std::size_t begin = std::max(visible, emphasis.selection_begin);
    if (begin < columns) {
      const SDL_Color wash = render::CompositeOver(palette.selection, base);
      if (!plan.fills.empty() && plan.fills.back().end == begin &&
          SameColor(plan.fills.back().color, wash)) {
        plan.fills.back().end = columns;
      } else {
        plan.fills.push_back({.begin = begin, .end = columns, .color = wash});
      }
    }
  }

  // 3) Glyph runs. A wide glyph sits on its own grid column, so it ends a run and
  // its trailing half (no glyph of its own) is skipped; a run of blanks draws
  // nothing.
  const auto is_wide_lead = [&](std::size_t column) {
    return column + 1 < line.cells.size() && line.cells[column + 1].style.wide_trailing();
  };
  for (std::size_t column = 0; column < visible;) {
    if (line.cells[column].style.wide_trailing()) {
      ++column;
      continue;
    }
    const SDL_Color color = cells[column].foreground;
    const auto face_of = [&](std::size_t index) {
      const terminal::TerminalStyle& style = line.cells[index].style;
      return render::FontStyleFor(style.bold(), style.italic());
    };
    const render::FontStyle face = face_of(column);
    std::size_t end = column + 1;
    if (!is_wide_lead(column)) {
      while (end < visible && !line.cells[end].style.wide_trailing() && !is_wide_lead(end) &&
             SameColor(cells[end].foreground, color) && face_of(end) == face) {
        ++end;
      }
    }
    const std::size_t offset = plan.text.size();
    bool visible_text = false;
    for (std::size_t index = column; index < end; ++index) {
      const std::string_view glyph = line.cells[index].DisplayText();
      if (glyph.empty()) {
        plan.text.push_back(' ');
        continue;
      }
      visible_text = visible_text || glyph != " ";
      plan.text.append(glyph);
    }
    // Trailing blanks draw nothing; leading ones place the text and stay.
    while (plan.text.size() > offset && plan.text.back() == ' ') {
      plan.text.pop_back();
    }
    if (visible_text) {
      plan.text_runs.push_back({.column = column,
                                .end = end,
                                .color = color,
                                .face = face,
                                .offset = offset,
                                .length = plan.text.size() - offset});
    } else {
      plan.text.resize(offset);
    }
    column = end;
  }

  // 4) Underline / double underline / strikethrough, coalesced over (bits,
  // underline shape, both colours). A wide glyph's trailing half carries its
  // lead's style and extra, so the stroke spans both columns. An underline takes
  // the cell's SGR 58 colour when it has one; hidden text's underline stays
  // hidden, like its glyphs.
  constexpr std::uint16_t kUnderlines =
      terminal::cell_attr::kUnderline | terminal::cell_attr::kDoubleUnderline;
  constexpr std::uint16_t kDecorationMask = kUnderlines | terminal::cell_attr::kStrikethrough;
  const auto stroke_at = [&](std::size_t column) {
    const terminal::TerminalCell& cell = line.cells[column];
    const auto bits = static_cast<std::uint16_t>(cell.style.attrs & kDecorationMask);
    TerminalRowPaintPlan::Decoration stroke{.bits = bits};
    if (bits == 0) {
      return stroke;
    }
    stroke.color = cells[column].foreground;
    stroke.underline_color = stroke.color;
    if ((bits & kUnderlines) != 0) {
      if ((bits & terminal::cell_attr::kUnderline) != 0) {
        stroke.variant = cell.style.underline_variant();
      }
      if (const terminal::TerminalColor color = terminal::FindUnderlineColor(underline_colors, cell.extra);
          color.has_value() && !cell.style.hidden()) {
        stroke.underline_color = SDL_Color{color->r, color->g, color->b, 0xff};
      }
    }
    return stroke;
  };
  for (std::size_t column = 0; column < visible;) {
    TerminalRowPaintPlan::Decoration stroke = stroke_at(column);
    if (stroke.bits == 0) {
      ++column;
      continue;
    }
    std::size_t end = column + 1;
    while (end < visible) {
      const TerminalRowPaintPlan::Decoration next = stroke_at(end);
      if (next.bits != stroke.bits || next.variant != stroke.variant ||
          !SameColor(next.color, stroke.color) || !SameColor(next.underline_color, stroke.underline_color)) {
        break;
      }
      ++end;
    }
    stroke.begin = column;
    stroke.end = end;
    plan.decorations.push_back(stroke);
    column = end;
  }
}

}  // namespace microide::workspace
