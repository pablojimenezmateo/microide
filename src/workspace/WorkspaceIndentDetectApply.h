#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "project/EditorConfig.h"

namespace microide::editor {
class TextViewport;
struct IndentDetection;
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
// Re-applies the viewport's own indentation (TextViewport::own_indent: what
// detection read, or what an indentation command chose) over whatever the
// preferences just set, for each property `editor_config` does not pin. Called by
// every preference apply, so a tab activation or a settings change keeps a
// file's detected indentation.
void ApplyOwnIndent(editor::TextViewport& viewport, const project::EditorConfigProperties& editor_config);

// The indentation a viewport ends up with, property by property: what
// `.editorconfig` pins, else the buffer's own (TextViewport::own_indent), else the
// preferences. A space-indented buffer's step is its tab size too. Resolved once
// and set once: setting the preference and then the buffer's own changed the tab
// size twice on every tab activation, and each change re-revealed the caret --
// switching back to a tab whose caret was scrolled off screen jumped the view.
struct ResolvedIndent {
  std::size_t tab_size = 4;
  std::size_t indent_width = 4;
  bool soft_tabs = false;
};
ResolvedIndent ResolveIndent(const editor::IndentDetection& own,
                             const project::EditorConfigProperties& editor_config,
                             const ResolvedIndent& preferences);
void ApplyResolvedIndent(editor::TextViewport& viewport, const ResolvedIndent& indent);

void ApplyDetectedIndentAfterPreferences(
    editor::TextViewport& viewport,
    const std::function<std::optional<std::string>(std::string_view)>& get_setting,
    const project::EditorConfigProperties& editor_config = {});

}  // namespace microide::workspace
