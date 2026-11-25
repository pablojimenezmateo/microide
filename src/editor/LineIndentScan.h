#pragma once

#include <cstddef>
#include <string_view>

#include "editor/LineSpan.h"

namespace microide::editor {

inline constexpr std::size_t kIndentScanChunkBytes = 256;

bool AdvanceLeadingIndentOverChunk(std::string_view chunk, std::size_t tab_size,
                                   std::size_t& visual);

std::size_t MeasureLeadingIndent(LineSpan lines, std::size_t index, std::size_t tab_size,
                                 bool* found_content = nullptr);

}
