#pragma once

#include <optional>

#include "editor/EditTypes.h"
#include "editor/TextBuffer.h"

namespace microide::editor {

std::optional<SelectionRange> ExpandSelectionRange(const TextBuffer& lines, SelectionRange current);

}
