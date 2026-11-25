#pragma once

#include <string>
#include <string_view>

namespace microide::editor {

class TextViewport;

bool ToggleLineComment(TextViewport& viewport, std::string_view line_marker);

bool ToggleBlockComment(TextViewport& viewport,
                        std::string_view open,
                        std::string_view close);

bool MoveLineUp(TextViewport& viewport);
bool MoveLineDown(TextViewport& viewport);
bool CopyLines(TextViewport& viewport, bool downward);

bool InsertLineBelow(TextViewport& viewport);
bool InsertLineAbove(TextViewport& viewport);
bool DeleteLine(TextViewport& viewport);
bool IndentSelection(TextViewport& viewport);
bool OutdentSelection(TextViewport& viewport);

enum class TabKeyIntent { kInsertTab, kIndentBlock, kOutdent, kMixed };
[[nodiscard]] TabKeyIntent ClassifyTabKey(const TextViewport& viewport, bool shift_held);

bool ApplyMixedTabKey(TextViewport& viewport);

bool JoinLinesAtCarets(TextViewport& viewport);

bool SortLines(TextViewport& viewport, bool ascending);

}
