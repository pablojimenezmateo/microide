#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace microide::workspace {

namespace {

constexpr std::size_t kEditorBlameReservedColumns = 28;
constexpr std::size_t kMinimumCodeColumnsWithBlame = 20;

}  // namespace

bool WorkspaceShell::EditorBlameFitsPane(const editor::TextViewport& viewport,
                                         const SDL_FRect& rect) const {
  if (rect.w <= 0.0f || rect.h <= 0.0f) {
    return false;
  }

  const editor::EditorViewMetrics base_metrics =
      editor::EditorViewRenderer::ComputeMetrics(text_renderer_, viewport, rect);
  const float char_width = std::max(1.0f, text_renderer_.CharWidth());
  const float blame_width = static_cast<float>(kEditorBlameReservedColumns) * char_width;
  const float remaining_width =
      rect.w - base_metrics.gutter_width - blame_width - 40.0f;
  return remaining_width / char_width >= static_cast<float>(kMinimumCodeColumnsWithBlame);
}

std::optional<editor::EditorBlameOverlay> WorkspaceShell::BuildEditorBlameOverlay(
    const editor::TextViewport& viewport,
    const SDL_FRect& rect) {
  if (project_root_.empty() || viewport.is_placeholder() || viewport.path().empty() ||
      viewport.dirty() || viewport.large_file_mode() ||
      !EditorBlameFitsPane(viewport, rect)) {
    return std::nullopt;
  }

  std::error_code error;
  const auto relative_path = std::filesystem::relative(
      viewport.path().lexically_normal(), project_root_.lexically_normal(), error);
  if (error || relative_path.empty() || relative_path.native().rfind("..", 0) == 0) {
    return std::nullopt;
  }

  const editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(text_renderer_, viewport, rect);
  const project::GitBlameRequest request{
      .root = project_root_,
      .absolute_path = viewport.path(),
      .visible_start_line = viewport.scroll_line(),
      .visible_line_count = metrics.visible_rows,
      .total_line_count = viewport.line_count(),
      .dirty = viewport.dirty(),
      .large_file_mode = viewport.large_file_mode(),
  };

  git_blame_service_.Request(request);
  const project::GitBlameSnapshot snapshot = git_blame_service_.Snapshot(request);
  if (!snapshot.eligible && !snapshot.loading) {
    return std::nullopt;
  }

  editor::EditorBlameOverlay overlay;
  overlay.visible = true;
  overlay.reserved_columns = kEditorBlameReservedColumns;
  overlay.lines.reserve(snapshot.lines.size());
  for (const auto& line : snapshot.lines) {
    overlay.lines.push_back(
        editor::EditorBlameLine{.line_index = line.line, .text = line.text});
  }
  return overlay;
}

void WorkspaceShell::InvalidateEditorBlamePath(const std::filesystem::path& path) {
  if (project_root_.empty() || path.empty()) {
    return;
  }
  git_blame_service_.InvalidatePath(project_root_, path.lexically_normal());
}

void WorkspaceShell::ClearEditorBlame() {
  git_blame_service_.Clear();
}

}  // namespace microide::workspace
