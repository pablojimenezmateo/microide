#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/EditTypes.h"
#include "editor/EditorRowYLayout.h"

namespace microide::editor {
class TextViewport;
struct EditorViewMetrics;
}

namespace microide::render {
class TextRenderer;
}

namespace microide::workspace {

struct ProjectWorkspaceState;

using SettingValueGetter = std::function<std::optional<std::string>(std::string_view)>;

void VisibleRowTops(const ProjectWorkspaceState& state, const editor::TextViewport& viewport,
                    const editor::EditorViewMetrics& metrics,
                    const SettingValueGetter& get_setting_value, std::vector<float>& out_tops);

editor::EditorRowYLayout::HitResult ResolveGapAwareRow(
    const ProjectWorkspaceState& state, const editor::TextViewport& viewport,
    const editor::EditorViewMetrics& metrics, float y, const SettingValueGetter& get_setting_value);

int CorrectClickVisualColumnForInlay(const editor::TextViewport& viewport,
                                     const ProjectWorkspaceState& state,
                                     const render::TextRenderer& text_renderer,
                                     std::size_t visual_row, int display_visual_column);

struct EditorPointerHit {
  int visual_row = 0;
  int hit_column = 0;
  editor::LogicalPosition position;
};

EditorPointerHit ResolveEditorPointerHit(const ProjectWorkspaceState& state,
                                         const editor::TextViewport& viewport,
                                         const editor::EditorViewMetrics& metrics,
                                         const render::TextRenderer& text_renderer, float x,
                                         float y, const SettingValueGetter& get_setting_value);

}
