#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace microide::editor {
class SingleLineEditor;
}  // namespace microide::editor

namespace microide::render {
class TextRenderer;
}  // namespace microide::render

namespace microide::workspace {

// Caret-relative visible slice of a single-line input field: the widest window of
// the (prefix + editor text) virtual string that fits `available_width` while
// keeping the caret visible, plus the caret x offset and the selection byte range
// clipped to that window. Shared by the shell render paths and
// RenderViewModelBuilder (which precomposes field display text into view models).
struct SingleLineViewMetrics {
  std::string displayed_text;
  float cursor_x = 0.0f;
  std::optional<std::pair<std::size_t, std::size_t>> selection_bytes;
};

SingleLineViewMetrics ComputeSingleLineViewMetrics(const render::TextRenderer& text_renderer,
                                                   const editor::SingleLineEditor& state,
                                                   std::string_view prefix,
                                                   float available_width);

// Where the longest tail of `text` that fits `available_width` begins:
// text.size() when not even its last character fits, 0 when all of it does.
// `measure` is a string's drawn width; a shorter tail is never the wider. An
// unfocused single-line field draws this tail of its value.
std::size_t FittingTailStart(std::string_view text, float available_width,
                             const std::function<float(std::string_view)>& measure);

}  // namespace microide::workspace
