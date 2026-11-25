#include "editor/TextDragDrop.h"

#include <string>

#include "editor/TextViewport.h"

namespace microide::editor::text_drag_drop {

bool PositionWithin(const SelectionRange& range, const TextPosition& position) {
  const SelectionRange normalized = TextViewport::NormalizeRange(range);
  return !PositionBefore(position, normalized.start) && !PositionBefore(normalized.end, position);
}

TextPosition AdjustDropForRemovedRange(const SelectionRange& source, const TextPosition& drop) {
  const SelectionRange range = TextViewport::NormalizeRange(source);
  if (PositionBefore(drop, range.start)) {
    return drop;
  }
  if (drop.line > range.end.line) {
    return TextPosition{drop.line - (range.end.line - range.start.line), drop.column};
  }
  return TextPosition{range.start.line, range.start.column + (drop.column - range.end.column)};
}

TextPosition PositionAfterInsertedText(const TextPosition& start, std::string_view text) {
  const std::size_t last_newline = text.rfind('\n');
  if (last_newline == std::string_view::npos) {
    return TextPosition{start.line, start.column + text.size()};
  }
  std::size_t newlines = 0;
  for (const char c : text) {
    newlines += static_cast<std::size_t>(c == '\n');
  }
  return TextPosition{start.line + newlines, text.size() - last_newline - 1};
}

std::optional<SelectionRange> Apply(TextViewport& viewport,
                                    const SelectionRange& source,
                                    const TextPosition& drop,
                                    bool copy) {
  const SelectionRange range = TextViewport::NormalizeRange(source);
  if (PositionWithin(range, drop)) {
    return std::nullopt;
  }
  const std::string text = viewport.TextInRange(range);
  if (text.empty()) {
    return std::nullopt;
  }

  viewport.BeginUndoGroup();
  TextPosition target = drop;
  if (!copy) {
    viewport.ReplaceRange(range, "");
    target = AdjustDropForRemovedRange(range, drop);
  }
  viewport.ReplaceRange(SelectionRange{target, target}, text);
  viewport.EndUndoGroup();

  return SelectionRange{target, PositionAfterInsertedText(target, text)};
}

}
