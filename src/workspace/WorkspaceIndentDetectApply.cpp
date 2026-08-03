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
  // file open (TD-2026-07-17A-003).
  const editor::IndentDetection detected = editor::DetectIndent(viewport.lines());
  if (!detected.detected) {
    return;
  }

  if (!pins_style) {
    viewport.SetSoftTabs(detected.soft_tabs);
  }
  if (!pins_width) {
    viewport.SetIndentWidth(detected.indent_width);
  }
  if (!pins_tab_size) {
    viewport.SetTabSize(detected.indent_width);
  }
}

}  // namespace microide::workspace
