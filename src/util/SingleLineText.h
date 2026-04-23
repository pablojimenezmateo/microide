#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "util/StringUtil.h"

namespace microide::util {

struct SingleLineTextState {
  std::string text;
  std::size_t cursor = 0;
  std::optional<std::size_t> selection_anchor;
};

struct SingleLineTextSelection {
  std::size_t start = 0;
  std::size_t end = 0;
};

void NormalizeSingleLineTextState(SingleLineTextState* state);
void SetSingleLineText(SingleLineTextState* state, std::string text);
bool HasSingleLineSelection(const SingleLineTextState& state);
std::optional<SingleLineTextSelection> SingleLineSelection(const SingleLineTextState& state);
std::string SelectedSingleLineText(const SingleLineTextState& state);
bool InsertSingleLineText(SingleLineTextState* state, std::string_view input);
bool BackspaceSingleLineText(SingleLineTextState* state);
bool DeleteForwardSingleLineText(SingleLineTextState* state);
bool MoveSingleLineCursorLeft(SingleLineTextState* state, bool extend_selection = false);
bool MoveSingleLineCursorRight(SingleLineTextState* state, bool extend_selection = false);
bool MoveSingleLineCursorHome(SingleLineTextState* state, bool extend_selection = false);
bool MoveSingleLineCursorEnd(SingleLineTextState* state, bool extend_selection = false);
bool SelectAllSingleLineText(SingleLineTextState* state);
bool DeleteSelectedSingleLineText(SingleLineTextState* state);

}  // namespace microide::util
