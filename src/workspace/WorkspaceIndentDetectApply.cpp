#include "workspace/WorkspaceIndentDetectApply.h"

#include "editor/IndentDetect.h"
#include "editor/TextViewport.h"

namespace microide::workspace {

void ApplyDetectedIndentAfterPreferences(
    editor::TextViewport& viewport,
    const std::function<std::optional<std::string>(std::string_view)>& get_setting) {
  if (viewport.path().empty()) {
    return;
  }

  const std::optional<std::string> toggle = get_setting("editor.indent.detect_on_open");
  const bool enabled =
      !toggle.has_value() ||
      (*toggle != "false" && *toggle != "0" && *toggle != "off");

  if (!enabled) {
    return;
  }

  const editor::IndentDetection detected = editor::DetectIndent(viewport.lines());
  if (!detected.detected) {
    return;
  }

  viewport.SetSoftTabs(detected.soft_tabs);
  viewport.SetIndentWidth(detected.indent_width);
  viewport.SetTabSize(detected.indent_width);
}

}  // namespace microide::workspace
