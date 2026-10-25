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
  ApplyResolvedIndent(viewport, ResolveIndent(viewport.own_indent(), editor_config,
                                              ResolvedIndent{.tab_size = viewport.tab_size(),
                                                             .indent_width = viewport.indent_width(),
                                                             .soft_tabs = viewport.soft_tabs()}));
}

ResolvedIndent ResolveIndent(const editor::IndentDetection& own,
                             const project::EditorConfigProperties& editor_config,
                             const ResolvedIndent& preferences) {
  // .editorconfig widths are parsed as ints; only a positive one is a width.
  const auto width = [](const std::optional<int>& configured) -> std::optional<std::size_t> {
    return configured.has_value() && *configured > 0 ? std::optional(static_cast<std::size_t>(*configured))
                                                     : std::nullopt;
  };
  // A tab-indented buffer says nothing about how wide a tab is: the width stays
  // whatever the preferences chose (VS Code keeps its default tab size too).
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

}  // namespace microide::workspace
