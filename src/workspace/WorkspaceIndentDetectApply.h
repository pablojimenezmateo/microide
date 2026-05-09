#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace microide::editor {
class TextViewport;
}

namespace microide::workspace {

// Applies `DetectIndent` to `viewport`'s lines when `editor.indent.detect_on_open`
// resolves true (unset defaults to enabled). Updates `tab_size`, `indent_width`, and
// `soft_tabs` on the viewport only — never modifies buffer contents or persistence.
void ApplyDetectedIndentAfterPreferences(
    editor::TextViewport& viewport,
    const std::function<std::optional<std::string>(std::string_view)>& get_setting);

}  // namespace microide::workspace
