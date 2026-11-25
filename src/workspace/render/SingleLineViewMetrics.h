#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace microide::editor {
class SingleLineEditor;
}

namespace microide::render {
class TextRenderer;
}

namespace microide::workspace {

struct SingleLineViewMetrics {
  std::string displayed_text;
  float cursor_x = 0.0f;
  std::optional<std::pair<std::size_t, std::size_t>> selection_bytes;
};

SingleLineViewMetrics ComputeSingleLineViewMetrics(const render::TextRenderer& text_renderer,
                                                   const editor::SingleLineEditor& state,
                                                   std::string_view prefix,
                                                   float available_width);

std::size_t FittingTailStart(std::string_view text, float available_width,
                             const std::function<float(std::string_view)>& measure);

}
