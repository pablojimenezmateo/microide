#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "project/EditorConfig.h"

namespace microide::editor {
class TextViewport;
}

namespace microide::workspace {

// Applies `DetectIndent` to `viewport`'s lines when `editor.indent.detect_on_open`
// resolves true (unset defaults to enabled). Updates `tab_size`, `indent_width`, and
// `soft_tabs` on the viewport only — never modifies buffer contents or persistence.
//
// `editor_config` carries any resolved `.editorconfig` opinion for this file.
// Detection is a guess about the file; EditorConfig is the project author stating
// the answer, so a property EditorConfig pinned is never overwritten by detection
// (this is VSCode's precedence too). Detection still fills in whatever
// EditorConfig left unsaid, and is skipped entirely when it said everything.
void ApplyDetectedIndentAfterPreferences(
    editor::TextViewport& viewport,
    const std::function<std::optional<std::string>(std::string_view)>& get_setting,
    const project::EditorConfigProperties& editor_config = {});

}  // namespace microide::workspace
