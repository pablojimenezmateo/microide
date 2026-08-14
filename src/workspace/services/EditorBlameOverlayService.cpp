#include "workspace/services/EditorBlameOverlayService.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <string>

#include "project/GitCommandUtil.h"

namespace microide::workspace {
namespace {

// Inline blame annotates the caret line only, the way VSCode/GitLens does. A
// wider radius painted the same "author, date" string on the rows above and
// below the caret too, which read as a rendering bug and cost 3x the snapshot
// copies, truncation, and text measurement every painted frame.
constexpr std::size_t kCaretBlameRadius = 0;
constexpr std::size_t kInlineBlameGapColumns = 8;
constexpr std::size_t kMinimumCodeColumnsWithBlame = 24;

// A `std::filesystem::path` assignment reallocates unconditionally, and both
// paths in a blame request are the same values frame after frame, so compare
// first. The comparison is a string compare on the native pathname.
void AssignPathIfChanged(std::filesystem::path& target, const std::filesystem::path& source) {
  if (target != source) {
    target = source;
  }
}

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

  // strftime into a stack buffer rather than an ostringstream: the stream is
  // several allocations plus a locale imbue for a fixed 21-byte result, and this
  // runs once per annotated line per painted frame while inline blame is on.
  char formatted[32] = {};
  const std::size_t written =
      std::strftime(formatted, sizeof(formatted), "%Y-%m-%d %H:%M UTC", &utc_time);
  if (written == 0) {
    return "Unknown date";
  }
  return std::string(formatted, written);
}

}  // namespace

bool EditorBlameOverlayService::FitsPane(render::TextRenderer& text_renderer,
                                         const editor::TextViewport& viewport,
                                         const SDL_FRect& rect,
                                         float minimum_pane_width,
                                         bool show_line_numbers) const {
  if (rect.w <= 0.0f || rect.h <= 0.0f || rect.w < minimum_pane_width) {
    return false;
  }

  const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
      text_renderer, viewport, rect, 0, show_line_numbers);
  return metrics.visible_columns >= kMinimumCodeColumnsWithBlame;
}

std::optional<editor::EditorBlameOverlay> EditorBlameOverlayService::BuildEditorOverlay(
    const std::filesystem::path& project_root,
    render::TextRenderer& text_renderer,
    project::GitBlameService& git_blame_service,
    editor::TextViewport& viewport,
    const SDL_FRect& rect,
    float minimum_pane_width,
    std::size_t sticky_scroll_rows,
    bool show_line_numbers) const {
  if (project_root.empty() || viewport.is_placeholder() || viewport.path().empty() || viewport.dirty() ||
      !FitsPane(text_renderer, viewport, rect, minimum_pane_width, show_line_numbers)) {
    return std::nullopt;
  }

  if (!project::internal::AbsoluteToRelativePathRef(project_root, viewport.path()).has_value()) {
    return std::nullopt;
  }

  const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
      text_renderer, viewport, rect, sticky_scroll_rows, show_line_numbers);
  viewport.SetViewportSize(metrics.visible_rows, metrics.visible_columns);
  const std::size_t visible_start_line = viewport.VisualRowLineIndex(viewport.scroll_line());
  // Inline blame only renders the caret +/- kCaretBlameRadius, so request just
  // that window of snapshot lines (the visible window still drives prefetch).
  const std::size_t caret_start =
      viewport.cursor_line() > kCaretBlameRadius ? viewport.cursor_line() - kCaretBlameRadius : 0;
  const std::size_t caret_end =
      viewport.line_count() == 0 ? 0
                                 : std::min(viewport.line_count() - 1, viewport.cursor_line() + kCaretBlameRadius);
  // Reuse the descriptor rather than build one: see editor_request_'s comment.
  project::GitBlameRequest& request = editor_request_;
  AssignPathIfChanged(request.root, project_root);
  AssignPathIfChanged(request.absolute_path, viewport.path());
  request.visible_start_line = visible_start_line;
  request.visible_line_count = metrics.visible_rows;
  request.total_line_count = viewport.line_count();
  request.dirty = viewport.dirty();
  request.result_start_line = caret_start;
  request.result_line_count = caret_end - caret_start + 1;

  git_blame_service.Request(request);
  const project::GitBlameSnapshot snapshot = git_blame_service.Snapshot(request);
  if (!snapshot.eligible && !snapshot.loading) {
    return std::nullopt;
  }

  const float char_width = std::max(1.0f, text_renderer.CharWidth());
  const float inline_gap = static_cast<float>(kInlineBlameGapColumns) * char_width;
  const float right_limit = rect.x + rect.w - 12.0f;

  editor::EditorBlameOverlay overlay;
  overlay.visible = true;
  overlay.lines.reserve(snapshot.lines.size());
  for (const auto& line : snapshot.lines) {
    if (line.line < caret_start || line.line > caret_end || line.line < visible_start_line) {
      continue;
    }

    const std::size_t visual_row = viewport.VisualRowForLine(line.line);
    if (visual_row < viewport.scroll_line()) {
      continue;
    }
    const std::size_t row = visual_row - viewport.scroll_line();
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

    const std::string display_text = text_renderer.TruncateToWidth(line.text, max_width);
    if (display_text.empty()) {
      continue;
    }

    overlay.lines.push_back(editor::EditorBlameLine{
        .line_index = line.line,
        .rect = MakeRect(x, y, text_renderer.MeasureWidth(display_text), metrics.line_height),
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

std::optional<editor::EditorBlameOverlay> EditorBlameOverlayService::BuildCompareOverlay(
    const std::filesystem::path& project_root,
    render::TextRenderer& text_renderer,
    project::GitBlameService& git_blame_service,
    CompareTabState& compare_tab,
    const CompareOverlayLayout& layout,
    const std::function<std::optional<CompareBlameAnchor>(std::size_t, std::size_t)>&
        compare_blame_anchor) const {
  if (!compare_tab.right_editable || !compare_tab.right_view_active) {
    return std::nullopt;
  }
  if (project_root.empty() || compare_tab.right_viewport.is_placeholder() ||
      compare_tab.right_viewport.path().empty() || compare_tab.right_viewport.dirty() ||
      // The compare surface renders its own gutter (WorkspaceShellRenderCompare),
      // independent of the editor.line_numbers toggle, so this blame-fits heuristic
      // keeps the line-number-reserved width regardless.
      !FitsPane(text_renderer, compare_tab.right_viewport, layout.pane_rect, 320.0f,
                /*show_line_numbers=*/true)) {
    return std::nullopt;
  }

  // …Ref, not the copying form: this only asks has_value(), and the copy is an
  // owned path per painted frame (the editor overlay above was converted by
  // TD-2026-08-14-223; the compare one was missed).
  if (!project::internal::AbsoluteToRelativePathRef(project_root,
                                                    compare_tab.right_viewport.path())
           .has_value()) {
    return std::nullopt;
  }

  compare_tab.right_viewport.SetViewportSize(static_cast<std::size_t>(layout.visible_rows),
                                             layout.visible_columns);
  const std::size_t caret_start =
      compare_tab.right_viewport.cursor_line() > kCaretBlameRadius
          ? compare_tab.right_viewport.cursor_line() - kCaretBlameRadius
          : 0;
  const std::size_t caret_end = compare_tab.right_viewport.line_count() == 0
                                    ? 0
                                    : std::min(compare_tab.right_viewport.line_count() - 1,
                                               compare_tab.right_viewport.cursor_line() + kCaretBlameRadius);
  project::GitBlameRequest& request = compare_request_;
  AssignPathIfChanged(request.root, project_root);
  AssignPathIfChanged(request.absolute_path, compare_tab.right_viewport.path());
  request.visible_start_line = compare_tab.right_viewport.scroll_line();
  request.visible_line_count = static_cast<std::size_t>(layout.visible_rows);
  request.total_line_count = compare_tab.right_viewport.line_count();
  request.dirty = compare_tab.right_viewport.dirty();
  request.result_start_line = caret_start;
  request.result_line_count = caret_end - caret_start + 1;

  git_blame_service.Request(request);
  const project::GitBlameSnapshot snapshot = git_blame_service.Snapshot(request);
  if (!snapshot.eligible && !snapshot.loading) {
    return std::nullopt;
  }

  const float char_width = std::max(1.0f, text_renderer.CharWidth());
  const float inline_gap = static_cast<float>(kInlineBlameGapColumns) * char_width;
  const float right_limit = layout.right_x + layout.gutter_width + layout.right_width - 12.0f;

  editor::EditorBlameOverlay overlay;
  overlay.visible = true;
  overlay.lines.reserve(snapshot.lines.size());
  for (const auto& line : snapshot.lines) {
    if (line.line < caret_start || line.line > caret_end) {
      continue;
    }

    const editor::LayoutLine layout_line = compare_tab.right_viewport.VisibleLineLayout(line.line);
    const std::optional<CompareBlameAnchor> anchor =
        compare_blame_anchor(line.line, layout_line.visual_columns);
    if (!anchor.has_value()) {
      continue;
    }
    const std::size_t scroll_row = static_cast<std::size_t>(std::max(0, compare_tab.scroll_row));
    if (anchor->visual_row < scroll_row ||
        anchor->visual_row >= scroll_row + static_cast<std::size_t>(layout.visible_rows)) {
      continue;
    }

    const float y = layout.rows_y +
                    static_cast<float>(anchor->visual_row - scroll_row) * layout.line_height;
    const float x = layout.right_x + layout.gutter_width +
                    static_cast<float>(anchor->end_column) * char_width + inline_gap;
    const float max_width = std::max(0.0f, right_limit - x);
    if (max_width < char_width * 4.0f) {
      continue;
    }

    const std::string display_text = text_renderer.TruncateToWidth(line.text, max_width);
    if (display_text.empty()) {
      continue;
    }

    overlay.lines.push_back(editor::EditorBlameLine{
        .line_index = line.line,
        .rect = MakeRect(x, y, text_renderer.MeasureWidth(display_text), layout.line_height),
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

void EditorBlameOverlayService::SetVisibleOverlay(std::optional<editor::EditorBlameOverlay> overlay) {
  visible_overlay_ = std::move(overlay);
}

void EditorBlameOverlayService::ClearVisibleOverlay() {
  visible_overlay_.reset();
}

const editor::EditorBlameLine* EditorBlameOverlayService::VisibleLine(std::size_t line_index) const {
  if (!visible_overlay_.has_value() || !visible_overlay_->visible) {
    return nullptr;
  }

  const auto it = std::find_if(visible_overlay_->lines.begin(),
                               visible_overlay_->lines.end(),
                               [&](const editor::EditorBlameLine& line) {
                                 return line.line_index == line_index;
                               });
  return it == visible_overlay_->lines.end() ? nullptr : &*it;
}

const editor::EditorBlameLine* EditorBlameOverlayService::LineAtPosition(float x, float y) const {
  if (!visible_overlay_.has_value() || !visible_overlay_->visible) {
    return nullptr;
  }

  const auto it = std::find_if(visible_overlay_->lines.begin(),
                               visible_overlay_->lines.end(),
                               [&](const editor::EditorBlameLine& line) {
                                 return Contains(line.rect, x, y);
                               });
  return it == visible_overlay_->lines.end() ? nullptr : &*it;
}

}  // namespace microide::workspace
