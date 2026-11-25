#include "workspace/services/EditorBlameOverlayService.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>

#include "project/GitCommandUtil.h"

namespace microide::workspace {
namespace {

constexpr std::size_t kCaretBlameRadius = 0;
constexpr std::size_t kInlineBlameGapColumns = 8;
constexpr std::size_t kMinimumCodeColumnsWithBlame = 24;

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
  gmtime_r(&timestamp, &utc_time);

  char formatted[32] = {};
  const std::size_t written =
      std::strftime(formatted, sizeof(formatted), "%Y-%m-%d %H:%M UTC", &utc_time);
  if (written == 0) {
    return "Unknown date";
  }
  return std::string(formatted, written);
}

struct CaretLines {
  std::size_t start = 0;
  std::size_t end = 0;
};

CaretLines CaretBlameLines(const editor::TextViewport& viewport) {
  const std::size_t cursor = viewport.cursor_line();
  const std::size_t line_count = viewport.line_count();
  return CaretLines{
      .start = cursor > kCaretBlameRadius ? cursor - kCaretBlameRadius : 0,
      .end = line_count == 0 ? 0 : std::min(line_count - 1, cursor + kCaretBlameRadius),
  };
}

bool BlameableBuffer(const std::filesystem::path& project_root,
                     const editor::TextViewport& viewport) {
  return !project_root.empty() && !viewport.is_placeholder() && !viewport.path().empty() &&
         !viewport.dirty() &&
         project::internal::AbsoluteToRelativePathRef(project_root, viewport.path()).has_value();
}

void FillBlameRequest(project::GitBlameRequest& request,
                      const platform::ProcessLauncher* launcher,
                      const std::filesystem::path& project_root,
                      const editor::TextViewport& viewport,
                      std::size_t visible_start_line,
                      std::size_t visible_line_count,
                      CaretLines caret) {
  request.launcher = launcher;
  AssignPathIfChanged(request.root, project_root);
  AssignPathIfChanged(request.absolute_path, viewport.path());
  request.visible_start_line = visible_start_line;
  request.visible_line_count = visible_line_count;
  request.total_line_count = viewport.line_count();
  request.dirty = viewport.dirty();
  request.result_start_line = caret.start;
  request.result_line_count = caret.end - caret.start + 1;
}

void AppendBlameLine(editor::EditorBlameOverlay& overlay,
                     render::TextRenderer& text_renderer,
                     const project::GitBlameLine& line,
                     float x,
                     float y,
                     float line_height,
                     float right_limit) {
  const float char_width = std::max(1.0f, text_renderer.CharWidth());
  const float max_width = std::max(0.0f, right_limit - x);
  if (max_width < char_width * 4.0f) {
    return;
  }
  std::string display_text = text_renderer.TruncateToWidth(line.text, max_width);
  if (display_text.empty()) {
    return;
  }
  const float width = text_renderer.MeasureWidth(display_text);
  overlay.lines.push_back(editor::EditorBlameLine{
      .line_index = line.line,
      .rect = MakeRect(x, y, width, line_height),
      .text = std::move(display_text),
      .commit_id = line.commit_id,
      .author = line.author,
      .summary = line.summary,
      .date = FormatBlameDate(line.author_time),
      .interactive = !line.commit_id.empty() && !line.synthetic,
  });
}

}

const platform::ProcessLauncher* EditorBlameOverlayService::ProjectLauncher() const {
  const platform::ProcessLauncher* launcher = project_launcher_ ? project_launcher_() : nullptr;
  return launcher != nullptr ? launcher : &platform::LocalProcessLauncher();
}

bool EditorBlameOverlayService::FitsPane(render::TextRenderer& text_renderer,
                                         const editor::TextViewport& viewport,
                                         const SDL_FRect& rect,
                                         float minimum_pane_width,
                                         bool show_line_numbers) const {
  if (rect.w <= 0.0f || rect.h <= 0.0f || rect.w < minimum_pane_width) {
    return false;
  }

  const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
      text_renderer, viewport, rect, editor::kNoStickyBand, show_line_numbers);
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
  if (!BlameableBuffer(project_root, viewport) ||
      !FitsPane(text_renderer, viewport, rect, minimum_pane_width, show_line_numbers)) {
    return std::nullopt;
  }

  const editor::EditorViewMetrics metrics = editor::EditorViewRenderer::ComputeMetrics(
      text_renderer, viewport, rect, sticky_scroll_rows, show_line_numbers);
  viewport.SetViewportSize(metrics.visible_rows, metrics.visible_columns);
  const std::size_t visible_start_line = viewport.VisualRowLineIndex(viewport.scroll_line());
  const CaretLines caret = CaretBlameLines(viewport);
  FillBlameRequest(editor_request_, ProjectLauncher(), project_root, viewport, visible_start_line,
                   metrics.visible_rows, caret);

  git_blame_service.Request(editor_request_);
  const project::GitBlameSnapshot snapshot = git_blame_service.Snapshot(editor_request_);
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
    if (line.line < caret.start || line.line > caret.end || line.line < visible_start_line) {
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
    AppendBlameLine(overlay, text_renderer, line, x, y, metrics.line_height, right_limit);
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
  editor::TextViewport& viewport = compare_tab.right_viewport;
  if (!compare_tab.right_editable || !compare_tab.right_view_active ||
      !BlameableBuffer(project_root, viewport) ||
      !FitsPane(text_renderer, viewport, layout.pane_rect, 320.0f, true)) {
    return std::nullopt;
  }

  viewport.SetViewportSize(static_cast<std::size_t>(layout.visible_rows), layout.visible_columns);
  const CaretLines caret = CaretBlameLines(viewport);
  FillBlameRequest(compare_request_, ProjectLauncher(), project_root, viewport,
                   viewport.scroll_line(), static_cast<std::size_t>(layout.visible_rows), caret);

  git_blame_service.Request(compare_request_);
  const project::GitBlameSnapshot snapshot = git_blame_service.Snapshot(compare_request_);
  if (!snapshot.eligible && !snapshot.loading) {
    return std::nullopt;
  }

  const float char_width = std::max(1.0f, text_renderer.CharWidth());
  const float inline_gap = static_cast<float>(kInlineBlameGapColumns) * char_width;
  const float right_limit = layout.right_x + layout.gutter_width + layout.right_width - 12.0f;
  const std::size_t scroll_row = static_cast<std::size_t>(std::max(0, compare_tab.scroll_row));

  editor::EditorBlameOverlay overlay;
  overlay.visible = true;
  overlay.lines.reserve(snapshot.lines.size());
  for (const auto& line : snapshot.lines) {
    if (line.line < caret.start || line.line > caret.end) {
      continue;
    }
    const editor::LayoutLine layout_line = viewport.VisibleLineLayout(line.line);
    const std::optional<CompareBlameAnchor> anchor =
        compare_blame_anchor(line.line, layout_line.visual_columns);
    if (!anchor.has_value() || anchor->visual_row < scroll_row ||
        anchor->visual_row >= scroll_row + static_cast<std::size_t>(layout.visible_rows)) {
      continue;
    }
    const float y = layout.rows_y +
                    static_cast<float>(anchor->visual_row - scroll_row) * layout.line_height;
    const float x = layout.right_x + layout.gutter_width +
                    static_cast<float>(anchor->end_column) * char_width + inline_gap;
    AppendBlameLine(overlay, text_renderer, line, x, y, layout.line_height, right_limit);
  }
  return overlay;
}

const std::optional<editor::EditorBlameOverlay>& EditorBlameOverlayService::EditorOverlayForFrame(
    std::uint64_t frame_id,
    const std::filesystem::path& project_root,
    render::TextRenderer& text_renderer,
    project::GitBlameService& git_blame_service,
    editor::TextViewport& viewport,
    const SDL_FRect& rect,
    float minimum_pane_width,
    std::size_t sticky_scroll_rows,
    bool show_line_numbers) const {
  const bool same_frame = frame_id != 0 && frame_id == frame_overlay_frame_id_ &&
                          &viewport == frame_overlay_viewport_ && rect.x == frame_overlay_rect_.x &&
                          rect.y == frame_overlay_rect_.y && rect.w == frame_overlay_rect_.w &&
                          rect.h == frame_overlay_rect_.h;
  if (!same_frame) {
    frame_overlay_ = BuildEditorOverlay(project_root, text_renderer, git_blame_service, viewport, rect,
                                        minimum_pane_width, sticky_scroll_rows, show_line_numbers);
    frame_overlay_frame_id_ = frame_id;
    frame_overlay_viewport_ = &viewport;
    frame_overlay_rect_ = rect;
  }
  return frame_overlay_;
}

void EditorBlameOverlayService::SetVisibleOverlay(std::optional<editor::EditorBlameOverlay> overlay) {
  visible_overlay_ = std::move(overlay);
  visible_source_.reset();
}

EditorBlameOverlayService::OverlaySource EditorBlameOverlayService::SourceOf(
    const editor::TextViewport& viewport) {
  return OverlaySource{.viewport = &viewport,
                       .caret_line = viewport.cursor_line(),
                       .scroll_line = viewport.scroll_line(),
                       .horizontal_scroll = viewport.horizontal_scroll(),
                       .content_revision = viewport.content_revision()};
}

void EditorBlameOverlayService::SetVisibleOverlay(std::optional<editor::EditorBlameOverlay> overlay,
                                                  const editor::TextViewport& source) {
  visible_overlay_ = std::move(overlay);
  visible_source_ = SourceOf(source);
}

void EditorBlameOverlayService::ForgetVisibleOverlayIfStale(const editor::TextViewport* active) {
  if (!visible_source_.has_value()) {
    return;
  }
  if (active == nullptr || !(SourceOf(*active) == *visible_source_)) {
    visible_overlay_.reset();
    visible_source_.reset();
  }
}

void EditorBlameOverlayService::ClearVisibleOverlay() {
  visible_overlay_.reset();
  visible_source_.reset();
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

}
