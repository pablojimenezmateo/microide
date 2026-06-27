#include "workspace/WorkspaceIndentDetectApply.h"

#include "editor/IndentDetect.h"
#include "editor/TextViewport.h"
#include "workspace/SettingFlags.h"

namespace microide::workspace {

void ApplyDetectedIndentAfterPreferences(
    editor::TextViewport& viewport,
    const std::function<std::optional<std::string>(std::string_view)>& get_setting) {
  if (viewport.path().empty()) {
    return;
  }

  if (!SettingFlagEnabled(get_setting("editor.indent.detect_on_open"), /*default_value=*/true)) {
    return;
  }

  const editor::IndentDetection detected = editor::DetectIndent(viewport.lines().Snapshot());
  if (!detected.detected) {
    return;
  }

  viewport.SetSoftTabs(detected.soft_tabs);
  viewport.SetIndentWidth(detected.indent_width);
  viewport.SetTabSize(detected.indent_width);
}

}  // namespace microide::workspace
