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

void ApplyOwnIndent(editor::TextViewport& viewport, const project::EditorConfigProperties& editor_config);

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

}
