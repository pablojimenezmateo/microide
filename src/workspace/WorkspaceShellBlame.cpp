#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>

#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

constexpr std::size_t kCaretBlameRadius = 1;
constexpr std::size_t kInlineBlameGapColumns = 8;
constexpr std::size_t kMinimumCodeColumnsWithBlame = 24;
constexpr float kMinimumEditorBlamePaneWidth = 520.0f;

std::string FormatBlameDate(std::int64_t author_time) {
  if (author_time <= 0) {
    return "Unknown date";
  }

  const std::time_t timestamp = static_cast<std::time_t>(author_time);
  std::tm utc_time{};
#if defined(_WIN32)
  gmtime_s(&utc_time, &timestamp);
#else
  gmtime_r(&timestamp, &utc_time);
#endif

  std::ostringstream output;
  output << std::put_time(&utc_time, "%Y-%m-%d %H:%M UTC");
  return output.str();
}

}  // namespace

bool WorkspaceShell::EditorBlameFitsPane(const editor::TextViewport& viewport,
                                         const SDL_FRect& rect,
                                         float minimum_pane_width) const {
  if (rect.w <= 0.0f || rect.h <= 0.0f || rect.w < minimum_pane_width) {
    return false;
  }

  const editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(text_renderer_, viewport, rect);
  return metrics.visible_columns >= kMinimumCodeColumnsWithBlame;
}

std::optional<editor::EditorBlameOverlay> WorkspaceShell::BuildEditorBlameOverlay(
    editor::TextViewport& viewport,
    const SDL_FRect& rect,
    float minimum_pane_width) {
  if (project_root_.empty() || viewport.is_placeholder() || viewport.path().empty() ||
      viewport.dirty() ||
      !EditorBlameFitsPane(viewport, rect, minimum_pane_width)) {
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
  viewport.SetViewportSize(metrics.visible_rows, metrics.visible_columns);
  const project::GitBlameRequest request{
      .root = project_root_,
      .absolute_path = viewport.path(),
      .visible_start_line = viewport.scroll_line(),
      .visible_line_count = metrics.visible_rows,
      .total_line_count = viewport.line_count(),
      .dirty = viewport.dirty(),
  };

  git_blame_service_.Request(request);
  const project::GitBlameSnapshot snapshot = git_blame_service_.Snapshot(request);
  if (!snapshot.eligible && !snapshot.loading) {
    return std::nullopt;
  }

  const float char_width = std::max(1.0f, text_renderer_.CharWidth());
  const float inline_gap = static_cast<float>(kInlineBlameGapColumns) * char_width;
  const float right_limit = rect.x + rect.w - 12.0f;
  const std::size_t caret_start =
      viewport.cursor_line() > kCaretBlameRadius ? viewport.cursor_line() - kCaretBlameRadius : 0;
  const std::size_t caret_end =
      std::min(viewport.line_count() - 1, viewport.cursor_line() + kCaretBlameRadius);

  editor::EditorBlameOverlay overlay;
  overlay.visible = true;
  overlay.lines.reserve(snapshot.lines.size());
  for (const auto& line : snapshot.lines) {
    if (line.line < caret_start || line.line > caret_end || line.line < viewport.scroll_line()) {
      continue;
    }

    const std::size_t row = line.line - viewport.scroll_line();
    if (row >= metrics.visible_rows) {
      continue;
    }

    const editor::LayoutLine layout = viewport.VisibleLineLayout(line.line);
    const float y = metrics.first_line_y + static_cast<float>(row) * metrics.line_height;
    const float x = metrics.text_x + static_cast<float>(layout.visual_columns) * char_width + inline_gap;
    const float max_width = std::max(0.0f, right_limit - x);
    if (max_width < char_width * 4.0f) {
      continue;
    }

    const std::string display_text = text_renderer_.TruncateToWidth(line.text, max_width);
    if (display_text.empty()) {
      continue;
    }

    overlay.lines.push_back(editor::EditorBlameLine{
        .line_index = line.line,
        .rect = MakeRect(x, y, text_renderer_.MeasureWidth(display_text), metrics.line_height),
        .text = display_text,
        .commit_id = line.commit_id,
        .author = line.author,
        .summary = line.summary,
        .date = FormatBlameDate(line.author_time),
        .interactive = !line.commit_id.empty() && !line.synthetic,
    });
  }
  return overlay;
}

std::optional<editor::EditorBlameOverlay> WorkspaceShell::BuildCompareBlameOverlay(
    CompareTabState& compare_tab,
    const CompareSurfaceLayout& surface,
    const SDL_FRect& rect) {
  if (!compare_tab.right_editable || !compare_tab.right_view_active) {
    return std::nullopt;
  }

  const float bottom_reserved = surface.show_horizontal ? 12.0f : 0.0f;
  const SDL_FRect pane_rect =
      MakeRect(surface.right_x, rect.y, surface.gutter_width + surface.right_width,
               std::max(0.0f, rect.h - bottom_reserved));
  if (project_root_.empty() || compare_tab.right_viewport.is_placeholder() ||
      compare_tab.right_viewport.path().empty() || compare_tab.right_viewport.dirty() ||
      !EditorBlameFitsPane(compare_tab.right_viewport, pane_rect, 320.0f)) {
    return std::nullopt;
  }

  std::error_code error;
  const auto relative_path = std::filesystem::relative(
      compare_tab.right_viewport.path().lexically_normal(), project_root_.lexically_normal(), error);
  if (error || relative_path.empty() || relative_path.native().rfind("..", 0) == 0) {
    return std::nullopt;
  }

  compare_tab.right_viewport.SetViewportSize(static_cast<std::size_t>(surface.visible_rows),
                                             surface.visible_columns);
  const project::GitBlameRequest request{
      .root = project_root_,
      .absolute_path = compare_tab.right_viewport.path(),
      .visible_start_line = compare_tab.right_viewport.scroll_line(),
      .visible_line_count = static_cast<std::size_t>(surface.visible_rows),
      .total_line_count = compare_tab.right_viewport.line_count(),
      .dirty = compare_tab.right_viewport.dirty(),
  };

  git_blame_service_.Request(request);
  const project::GitBlameSnapshot snapshot = git_blame_service_.Snapshot(request);
  if (!snapshot.eligible && !snapshot.loading) {
    return std::nullopt;
  }

  const float char_width = std::max(1.0f, text_renderer_.CharWidth());
  const float inline_gap = static_cast<float>(kInlineBlameGapColumns) * char_width;
  const float right_limit = surface.right_x + surface.gutter_width + surface.right_width - 12.0f;
  const std::size_t caret_start =
      compare_tab.right_viewport.cursor_line() > kCaretBlameRadius
          ? compare_tab.right_viewport.cursor_line() - kCaretBlameRadius
          : 0;
  const std::size_t caret_end = compare_tab.right_viewport.line_count() == 0
                                    ? 0
                                    : std::min(compare_tab.right_viewport.line_count() - 1,
                                               compare_tab.right_viewport.cursor_line() + kCaretBlameRadius);

  editor::EditorBlameOverlay overlay;
  overlay.visible = true;
  overlay.lines.reserve(snapshot.lines.size());
  for (const auto& line : snapshot.lines) {
    if (line.line < caret_start || line.line > caret_end) {
      continue;
    }

    const std::size_t model_row = CompareRowIndexForRightLine(compare_tab, line.line);
    if (model_row >= compare_tab.model.rows.size()) {
      continue;
    }
    const auto& compare_row = compare_tab.model.rows[model_row];
    if (compare_row.right_line != static_cast<int>(line.line + 1)) {
      continue;
    }
    if (model_row < static_cast<std::size_t>(std::max(0, compare_tab.scroll_row)) ||
        model_row >= static_cast<std::size_t>(std::max(0, compare_tab.scroll_row) + surface.visible_rows)) {
      continue;
    }

    const editor::LayoutLine layout_line = compare_tab.right_viewport.VisibleLineLayout(line.line);
    const float y = surface.rows_y +
                    static_cast<float>(model_row - static_cast<std::size_t>(std::max(0, compare_tab.scroll_row))) *
                        surface.line_height;
    const float x = surface.right_x + surface.gutter_width +
                    static_cast<float>(layout_line.visual_columns) * char_width + inline_gap;
    const float max_width = std::max(0.0f, right_limit - x);
    if (max_width < char_width * 4.0f) {
      continue;
    }

    const std::string display_text = text_renderer_.TruncateToWidth(line.text, max_width);
    if (display_text.empty()) {
      continue;
    }

    overlay.lines.push_back(editor::EditorBlameLine{
        .line_index = line.line,
        .rect = MakeRect(x, y, text_renderer_.MeasureWidth(display_text), surface.line_height),
        .text = display_text,
        .commit_id = line.commit_id,
        .author = line.author,
        .summary = line.summary,
        .date = FormatBlameDate(line.author_time),
        .interactive = !line.commit_id.empty() && !line.synthetic,
    });
  }
  return overlay;
}

const editor::EditorBlameLine* WorkspaceShell::VisibleEditorBlameLine(std::size_t line_index) const {
  if (!visible_editor_blame_overlay_.has_value() || !visible_editor_blame_overlay_->visible) {
    return nullptr;
  }

  const auto it = std::find_if(visible_editor_blame_overlay_->lines.begin(),
                               visible_editor_blame_overlay_->lines.end(),
                               [&](const editor::EditorBlameLine& line) {
                                 return line.line_index == line_index;
                               });
  return it == visible_editor_blame_overlay_->lines.end() ? nullptr : &*it;
}

const editor::EditorBlameLine* WorkspaceShell::EditorBlameLineAtPosition(float x, float y) const {
  if (!visible_editor_blame_overlay_.has_value() || !visible_editor_blame_overlay_->visible) {
    return nullptr;
  }

  const auto it = std::find_if(visible_editor_blame_overlay_->lines.begin(),
                               visible_editor_blame_overlay_->lines.end(),
                               [&](const editor::EditorBlameLine& line) {
                                 return Contains(line.rect, x, y);
                               });
  return it == visible_editor_blame_overlay_->lines.end() ? nullptr : &*it;
}

void WorkspaceShell::InvalidateEditorBlamePath(const std::filesystem::path& path) {
  if (project_root_.empty() || path.empty()) {
    return;
  }
  git_blame_service_.InvalidatePath(project_root_, path.lexically_normal());
}

void WorkspaceShell::ClearEditorBlame() {
  visible_editor_blame_overlay_.reset();
  active_editor_hover_target_.reset();
  git_blame_service_.Clear();
}

}  // namespace microide::workspace
