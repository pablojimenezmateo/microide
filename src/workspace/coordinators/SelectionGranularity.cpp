#include "workspace/coordinators/SelectionGranularity.h"

#include "editor/TextViewport.h"

namespace microide::workspace::selection_granularity {

void RecordSeed(InteractionState& interaction_state,
                InteractionState::SelectionGranularity granularity,
                const editor::TextViewport& viewport) {
  const auto selected = viewport.selection_range();
  if (!selected.has_value()) {
    // The click landed somewhere with no word under it (whitespace, empty line).
    // Character granularity is then exactly right, and leaving the seed unset
    // avoids a drag that snaps to a word the user never selected.
    return;
  }
  const editor::SelectionRange range = editor::TextViewport::NormalizeRange(*selected);
  interaction_state.selection_granularity = granularity;
  interaction_state.selection_seed_start_line = range.start.line;
  interaction_state.selection_seed_start_column = range.start.column;
  interaction_state.selection_seed_end_line = range.end.line;
  interaction_state.selection_seed_end_column = range.end.column;
}

void ApplyClick(InteractionState& interaction_state,
                editor::TextViewport& viewport,
                int clicks) {
  interaction_state.selection_granularity = InteractionState::SelectionGranularity::Character;
  if (clicks == 2) {
    viewport.SelectWordAtCursor();
    RecordSeed(interaction_state, InteractionState::SelectionGranularity::Word, viewport);
    return;
  }
  if (clicks >= 3) {
    viewport.SelectLineAtCursor();
    RecordSeed(interaction_state, InteractionState::SelectionGranularity::Line, viewport);
  }
}

void ExtendToPointer(const InteractionState& interaction_state,
                     editor::TextViewport& viewport,
                     editor::TextPosition hit_position) {
  const editor::TextPosition seed_start{interaction_state.selection_seed_start_line,
                                        interaction_state.selection_seed_start_column};
  const editor::TextPosition seed_end{interaction_state.selection_seed_end_line,
                                      interaction_state.selection_seed_end_column};

  editor::TextPosition unit_start = hit_position;
  editor::TextPosition unit_end = hit_position;
  if (interaction_state.selection_granularity ==
      InteractionState::SelectionGranularity::Word) {
    if (const auto word = viewport.WordRangeAt(hit_position); word.has_value()) {
      unit_start = word->start;
      unit_end = word->end;
    }
  } else {
    const editor::SelectionRange line = viewport.LineRangeAt(hit_position.line);
    unit_start = line.start;
    unit_end = line.end;
  }

  const auto before = [](const editor::TextPosition& a, const editor::TextPosition& b) {
    return a.line != b.line ? a.line < b.line : a.column < b.column;
  };
  // Dragging below/after the seed anchors at the seed's start and puts the caret
  // at the far end of the pointer's unit; dragging above/before mirrors it.
  // No SetSelection(anchor, caret) exists; the established idiom is a collapse
  // followed by an extending move, which is what every other selection writer
  // here does.
  const editor::TextPosition anchor = before(unit_start, seed_start) ? seed_end : seed_start;
  const editor::TextPosition caret = before(unit_start, seed_start) ? unit_start : unit_end;
  viewport.MoveCursorTo(anchor.line, anchor.column);
  viewport.MoveCursorTo(caret.line, caret.column, /*extend_selection=*/true);
}

}  // namespace microide::workspace::selection_granularity
