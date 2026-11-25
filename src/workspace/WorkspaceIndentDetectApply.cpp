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

  const bool pins_style = editor_config.soft_tabs.has_value();
  const bool pins_width = editor_config.indent_width.has_value();
  const bool pins_tab_size = editor_config.tab_size.has_value();
  if (pins_style && pins_width && pins_tab_size) {
    return;
  }

  if (!SettingFlagEnabled(get_setting("editor.indent.detect_on_open"), true)) {
    return;
  }

  viewport.SetOwnIndent(editor::DetectIndent(viewport.lines()));
  ApplyOwnIndent(viewport, editor_config);
}

void ApplyOwnIndent(editor::TextViewport& viewport, const project::EditorConfigProperties& editor_config) {
  ApplyResolvedIndent(viewport, ResolveIndent(viewport.own_indent(), editor_config,
                                              ResolvedIndent{.tab_size = viewport.tab_size(),
                                                             .indent_width = viewport.indent_width(),
                                                             .soft_tabs = viewport.soft_tabs()}));
}

ResolvedIndent ResolveIndent(const editor::IndentDetection& own,
                             const project::EditorConfigProperties& editor_config,
                             const ResolvedIndent& preferences) {
  const auto width = [](const std::optional<int>& configured) -> std::optional<std::size_t> {
    return configured.has_value() && *configured > 0 ? std::optional(static_cast<std::size_t>(*configured))
                                                     : std::nullopt;
  };
  return ResolvedIndent{
      .tab_size = width(editor_config.tab_size).value_or(own.indent_width.value_or(preferences.tab_size)),
      .indent_width =
          width(editor_config.indent_width).value_or(own.indent_width.value_or(preferences.indent_width)),
      .soft_tabs = editor_config.soft_tabs.value_or(own.soft_tabs.value_or(preferences.soft_tabs)),
  };
}

void ApplyResolvedIndent(editor::TextViewport& viewport, const ResolvedIndent& indent) {
  viewport.SetTabSize(indent.tab_size);
  viewport.SetIndentWidth(indent.indent_width);
  viewport.SetSoftTabs(indent.soft_tabs);
}

}
