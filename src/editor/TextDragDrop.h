#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

#include "editor/EditTypes.h"

namespace microide::editor {

class TextViewport;

namespace text_drag_drop {

using editor::PositionBefore;

bool PositionWithin(const SelectionRange& range, const TextPosition& position);

TextPosition AdjustDropForRemovedRange(const SelectionRange& source, const TextPosition& drop);

TextPosition PositionAfterInsertedText(const TextPosition& start, std::string_view text);

std::optional<SelectionRange> Apply(TextViewport& viewport,
                                    const SelectionRange& source,
                                    const TextPosition& drop,
                                    bool copy);

}

}
