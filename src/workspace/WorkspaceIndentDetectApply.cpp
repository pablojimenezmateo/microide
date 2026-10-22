#include "workspace/WorkspaceIndentDetectApply.h"

#include "editor/IndentDetect.h"
#include "editor/TextViewport.h"
#include "workspace/SettingFlags.h"

namespace microide::workspace {

void ApplyDetectedIndentAfterPreferences(
    editor::TextViewport& viewport,
    const std::function<std::optional<std::string>(std::string_view)>& get_setting,
    const project::EditorConfigProperties& editor_config) {
  if (viewport.path().empty()) {
    return;
  }

  // EditorConfig answered every question detection could: skip the scan entirely.
  // This is the speed case that matters — a repo with `indent_style` and
  // `indent_size` set (the overwhelmingly common pair) never pays for a head-scan
  // of the buffer on open.
  const bool pins_style = editor_config.soft_tabs.has_value();
  const bool pins_width = editor_config.indent_width.has_value();
  const bool pins_tab_size = editor_config.tab_size.has_value();
  if (pins_style && pins_width && pins_tab_size) {
    return;
  }

  if (!SettingFlagEnabled(get_setting("editor.indent.detect_on_open"), /*default_value=*/true)) {
    return;
  }

  // Pass the live TextBuffer as a LineSpan so detection reads lines zero-copy via
  // LineView instead of materializing the whole document with Snapshot() on every
  // file open (TD-2026-07-17A-003). The result is the buffer's own indentation from
  // now on: kept on the viewport so later preference applies do not undo it.
  viewport.SetOwnIndent(editor::DetectIndent(viewport.lines()));
  ApplyOwnIndent(viewport, editor_config);
}

void ApplyOwnIndent(editor::TextViewport& viewport, const project::EditorConfigProperties& editor_config) {
  const editor::IndentDetection& own = viewport.own_indent();
  if (!editor_config.soft_tabs.has_value() && own.soft_tabs) {
    viewport.SetSoftTabs(*own.soft_tabs);
  }
  // A tab-indented buffer says nothing about how wide a tab is: the width stays
  // whatever the preferences chose (VS Code keeps its default tab size too).
  if (!own.indent_width) {
    return;
  }
  if (!editor_config.indent_width.has_value()) {
    viewport.SetIndentWidth(*own.indent_width);
  }
  if (!editor_config.tab_size.has_value()) {
    viewport.SetTabSize(*own.indent_width);
  }
}

}  // namespace microide::workspace
