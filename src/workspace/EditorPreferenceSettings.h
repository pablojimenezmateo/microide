#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "util/StringUtil.h"

namespace microide::workspace {

struct EditorPreferenceSettings {
  bool editorconfig_enabled = true;
  bool trim_trailing_whitespace = false;
  bool ensure_final_newline = true;
  std::optional<util::LineEnding> save_line_ending;
  bool auto_close = true;
  bool surround = true;
  bool smart_indent = true;
};

EditorPreferenceSettings ResolveEditorPreferenceSettings(
    const std::function<std::optional<std::string>(std::string_view)>& get_setting);

}
