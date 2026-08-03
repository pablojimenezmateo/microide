#include "workspace/EditorPreferenceSettings.h"

#include "workspace/SettingFlags.h"

namespace microide::workspace {

EditorPreferenceSettings ResolveEditorPreferenceSettings(
    const std::function<std::optional<std::string>(std::string_view)>& get_setting) {
  EditorPreferenceSettings resolved;
  if (!get_setting) {
    return resolved;
  }

  const auto flag = [&get_setting](std::string_view id, bool default_value) {
    return SettingFlagEnabled(get_setting(id), default_value);
  };

  resolved.editorconfig_enabled = flag("editor.editorconfig.enabled", true);
  resolved.trim_trailing_whitespace = flag("editor.save.trim_trailing_whitespace", true);
  resolved.ensure_final_newline = flag("editor.save.ensure_final_newline", true);
  resolved.auto_close = flag("editor.brackets.auto_close.enabled", true);
  resolved.surround = flag("editor.brackets.surround.enabled", true);
  resolved.smart_indent = flag("editor.indent.smart.enabled", true);

  // "auto" (and anything unrecognized) keeps the file's detected ending.
  const std::optional<std::string> line_endings = get_setting("editor.line_endings");
  if (line_endings.has_value()) {
    if (*line_endings == "lf") {
      resolved.save_line_ending = util::LineEnding::LF;
    } else if (*line_endings == "crlf") {
      resolved.save_line_ending = util::LineEnding::CRLF;
    }
  }
  return resolved;
}

}  // namespace microide::workspace
