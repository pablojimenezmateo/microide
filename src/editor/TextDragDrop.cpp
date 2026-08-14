#include "editor/TextDragDrop.h"

#include <string>

#include "editor/TextViewport.h"

namespace microide::editor::text_drag_drop {

bool PositionBefore(const TextPosition& lhs, const TextPosition& rhs) {
  return lhs.line != rhs.line ? lhs.line < rhs.line : lhs.column < rhs.column;
}

bool PositionWithin(const SelectionRange& range, const TextPosition& position) {
  const SelectionRange normalized = TextViewport::NormalizeRange(range);
  return !PositionBefore(position, normalized.start) && !PositionBefore(normalized.end, position);
}

TextPosition AdjustDropForRemovedRange(const SelectionRange& source, const TextPosition& drop) {
  const SelectionRange range = TextViewport::NormalizeRange(source);
  if (PositionBefore(drop, range.start)) {
    // Removing the source moves nothing that precedes it. (A drop INSIDE the
    // source is the caller's to refuse — Apply does, as a no-op.)
    return drop;
  }
  if (drop.line > range.end.line) {
    // Whole lines vanished above it; the column is untouched.
    return TextPosition{drop.line - (range.end.line - range.start.line), drop.column};
  }
  // On the source's last line, past its end: that line's tail is spliced onto the
  // source's first line, so the drop lands at the join plus its offset into the
  // tail.
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
    return std::nullopt;  // dropped onto itself
  }
  // Read from the RANGE, not from the live selection: the drop gesture is the
  // one edit here whose source is captured at press time and applied at release.
  const std::string text = viewport.TextInRange(range);
  if (text.empty()) {
    return std::nullopt;
  }

  // One undo entry covering both halves. Without the group, Ctrl+Z after a move
  // undoes the insert and leaves the text deleted from where it came from.
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

}  // namespace microide::editor::text_drag_drop
