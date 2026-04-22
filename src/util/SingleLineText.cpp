#include "util/SingleLineText.h"

#include <algorithm>

#include "util/StringUtil.h"

namespace microide::util {

namespace {

std::size_t ClampCursor(std::string_view text, std::size_t offset) {
  return std::min(offset, text.size());
}

void ClearSelection(SingleLineTextState* state) {
  if (state != nullptr) {
    state->selection_anchor.reset();
  }
}

void BeginSelectionIfNeeded(SingleLineTextState* state, bool extend_selection) {
  if (state == nullptr) {
    return;
  }
  if (extend_selection) {
    if (!state->selection_anchor.has_value()) {
      state->selection_anchor = state->cursor;
    }
  } else {
    ClearSelection(state);
  }
}

}  // namespace

std::size_t PreviousUtf8Boundary(std::string_view text, std::size_t offset) {
  offset = ClampCursor(text, offset);
  if (offset == 0) {
    return 0;
  }
  std::size_t previous = offset - 1;
  while (previous > 0 &&
         (static_cast<unsigned char>(text[previous]) & 0xC0u) == 0x80u) {
    --previous;
  }
  return previous;
}

std::size_t NextUtf8Boundary(std::string_view text, std::size_t offset) {
  offset = ClampCursor(text, offset);
  if (offset >= text.size()) {
    return text.size();
  }
  return std::min(text.size(), offset + Utf8SequenceLength(text, offset));
}

void NormalizeSingleLineTextState(SingleLineTextState* state) {
  if (state == nullptr) {
    return;
  }
  state->cursor = ClampCursor(state->text, state->cursor);
  if (state->selection_anchor.has_value()) {
    *state->selection_anchor = ClampCursor(state->text, *state->selection_anchor);
    if (*state->selection_anchor == state->cursor) {
      state->selection_anchor.reset();
    }
  }
}

void SetSingleLineText(SingleLineTextState* state, std::string text) {
  if (state == nullptr) {
    return;
  }
  state->text = std::move(text);
  state->cursor = state->text.size();
  state->selection_anchor.reset();
}

std::optional<SingleLineTextSelection> SingleLineSelection(const SingleLineTextState& state) {
  if (!state.selection_anchor.has_value() || *state.selection_anchor == state.cursor) {
    return std::nullopt;
  }
  return SingleLineTextSelection{
      .start = std::min(*state.selection_anchor, state.cursor),
      .end = std::max(*state.selection_anchor, state.cursor),
  };
}

bool HasSingleLineSelection(const SingleLineTextState& state) {
  return SingleLineSelection(state).has_value();
}

std::string SelectedSingleLineText(const SingleLineTextState& state) {
  const auto selection = SingleLineSelection(state);
  if (!selection.has_value()) {
    return {};
  }
  return state.text.substr(selection->start, selection->end - selection->start);
}

bool DeleteSelectedSingleLineText(SingleLineTextState* state) {
  if (state == nullptr) {
    return false;
  }
  NormalizeSingleLineTextState(state);
  const auto selection = SingleLineSelection(*state);
  if (!selection.has_value()) {
    return false;
  }
  state->text.erase(selection->start, selection->end - selection->start);
  state->cursor = selection->start;
  state->selection_anchor.reset();
  return true;
}

bool InsertSingleLineText(SingleLineTextState* state, std::string_view input) {
  if (state == nullptr || input.empty()) {
    return false;
  }
  NormalizeSingleLineTextState(state);
  (void)DeleteSelectedSingleLineText(state);
  state->text.insert(state->cursor, input);
  state->cursor += input.size();
  state->selection_anchor.reset();
  return true;
}

bool BackspaceSingleLineText(SingleLineTextState* state) {
  if (state == nullptr) {
    return false;
  }
  NormalizeSingleLineTextState(state);
  if (DeleteSelectedSingleLineText(state)) {
    return true;
  }
  if (state->cursor == 0) {
    return false;
  }
  const std::size_t previous = PreviousUtf8Boundary(state->text, state->cursor);
  state->text.erase(previous, state->cursor - previous);
  state->cursor = previous;
  return true;
}

bool DeleteForwardSingleLineText(SingleLineTextState* state) {
  if (state == nullptr) {
    return false;
  }
  NormalizeSingleLineTextState(state);
  if (DeleteSelectedSingleLineText(state)) {
    return true;
  }
  if (state->cursor >= state->text.size()) {
    return false;
  }
  const std::size_t next = NextUtf8Boundary(state->text, state->cursor);
  state->text.erase(state->cursor, next - state->cursor);
  return true;
}

bool MoveSingleLineCursorLeft(SingleLineTextState* state, bool extend_selection) {
  if (state == nullptr) {
    return false;
  }
  NormalizeSingleLineTextState(state);
  if (!extend_selection && HasSingleLineSelection(*state)) {
    const auto selection = SingleLineSelection(*state);
    state->cursor = selection->start;
    state->selection_anchor.reset();
    return true;
  }
  if (state->cursor == 0) {
    return false;
  }
  BeginSelectionIfNeeded(state, extend_selection);
  state->cursor = PreviousUtf8Boundary(state->text, state->cursor);
  NormalizeSingleLineTextState(state);
  return true;
}

bool MoveSingleLineCursorRight(SingleLineTextState* state, bool extend_selection) {
  if (state == nullptr) {
    return false;
  }
  NormalizeSingleLineTextState(state);
  if (!extend_selection && HasSingleLineSelection(*state)) {
    const auto selection = SingleLineSelection(*state);
    state->cursor = selection->end;
    state->selection_anchor.reset();
    return true;
  }
  if (state->cursor >= state->text.size()) {
    return false;
  }
  BeginSelectionIfNeeded(state, extend_selection);
  state->cursor = NextUtf8Boundary(state->text, state->cursor);
  NormalizeSingleLineTextState(state);
  return true;
}

bool MoveSingleLineCursorHome(SingleLineTextState* state, bool extend_selection) {
  if (state == nullptr) {
    return false;
  }
  NormalizeSingleLineTextState(state);
  if (state->cursor == 0 && (!extend_selection || !HasSingleLineSelection(*state))) {
    return false;
  }
  BeginSelectionIfNeeded(state, extend_selection);
  state->cursor = 0;
  NormalizeSingleLineTextState(state);
  return true;
}

bool MoveSingleLineCursorEnd(SingleLineTextState* state, bool extend_selection) {
  if (state == nullptr) {
    return false;
  }
  NormalizeSingleLineTextState(state);
  if (state->cursor == state->text.size() &&
      (!extend_selection || !HasSingleLineSelection(*state))) {
    return false;
  }
  BeginSelectionIfNeeded(state, extend_selection);
  state->cursor = state->text.size();
  NormalizeSingleLineTextState(state);
  return true;
}

bool SelectAllSingleLineText(SingleLineTextState* state) {
  if (state == nullptr) {
    return false;
  }
  NormalizeSingleLineTextState(state);
  if (state->text.empty()) {
    state->cursor = 0;
    state->selection_anchor.reset();
    return false;
  }
  state->selection_anchor = 0;
  state->cursor = state->text.size();
  return true;
}

}  // namespace microide::util
