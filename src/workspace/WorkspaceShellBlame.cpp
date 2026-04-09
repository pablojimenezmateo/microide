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
constexpr float kBlamePopupPadding = 12.0f;
constexpr float kBlamePopupGap = 6.0f;
constexpr float kBlamePopupMaxWidth = 420.0f;

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
                                         const SDL_FRect& rect) const {
  if (rect.w <= 0.0f || rect.h <= 0.0f || rect.w < kMinimumEditorBlamePaneWidth) {
    return false;
  }

  const editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(text_renderer_, viewport, rect);
  return metrics.visible_columns >= kMinimumCodeColumnsWithBlame;
}

std::optional<editor::EditorBlameOverlay> WorkspaceShell::BuildEditorBlameOverlay(
    editor::TextViewport& viewport,
    const SDL_FRect& rect) {
  if (project_root_.empty() || viewport.is_placeholder() || viewport.path().empty() ||
      viewport.dirty() || viewport.large_file_mode() || !EditorBlameFitsPane(viewport, rect)) {
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
      .large_file_mode = viewport.large_file_mode(),
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

std::optional<WorkspaceShell::EditorBlamePopupLayout> WorkspaceShell::ActiveEditorBlamePopupLayout() const {
  if (!active_editor_blame_popup_line_.has_value() || last_window_width_ <= 0 ||
      last_window_height_ <= 0) {
    return std::nullopt;
  }

  const editor::EditorBlameLine* blame_line = VisibleEditorBlameLine(*active_editor_blame_popup_line_);
  if (blame_line == nullptr || !blame_line->interactive) {
    return std::nullopt;
  }

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(last_window_width_), static_cast<float>(last_window_height_),
                    sidebar_visible_, BottomPanelVisible(), sidebar_width_, bottom_panel_height_);
  const float copy_width = std::max(84.0f, text_renderer_.MeasureWidth("Copy SHA") + 18.0f);
  const float content_width = std::max({text_renderer_.MeasureWidth(blame_line->author),
                                        text_renderer_.MeasureWidth(blame_line->date),
                                        text_renderer_.MeasureWidth(blame_line->summary),
                                        copy_width});
  const float card_width =
      std::min(kBlamePopupMaxWidth, content_width + kBlamePopupPadding * 2.0f);
  const float button_height = text_renderer_.LineHeight() + 8.0f;
  const float card_height = kBlamePopupPadding * 2.0f + text_renderer_.LineHeight() * 3.0f +
                            button_height + 10.0f;

  float x = blame_line->rect.x;
  float y = blame_line->rect.y + blame_line->rect.h + kBlamePopupGap;
  if (x + card_width > layout.editor_surface.x + layout.editor_surface.w - 8.0f) {
    x = layout.editor_surface.x + layout.editor_surface.w - card_width - 8.0f;
  }
  if (y + card_height > layout.editor_surface.y + layout.editor_surface.h - 8.0f) {
    y = blame_line->rect.y - card_height - kBlamePopupGap;
  }
  x = std::max(layout.editor_surface.x + 8.0f, x);
  y = std::max(layout.editor_surface.y + 8.0f, y);

  const SDL_FRect card_rect = MakeRect(x, y, card_width, card_height);
  const SDL_FRect copy_sha_rect = MakeRect(card_rect.x + card_rect.w - copy_width - kBlamePopupPadding,
                                           card_rect.y + card_rect.h - button_height - kBlamePopupPadding,
                                           copy_width, button_height);
  return EditorBlamePopupLayout{
      .line_index = blame_line->line_index,
      .rect = card_rect,
      .copy_sha_rect = copy_sha_rect,
  };
}

void WorkspaceShell::UpdateEditorBlameHover(float x, float y) {
  if (const editor::EditorBlameLine* blame_line = EditorBlameLineAtPosition(x, y);
      blame_line != nullptr && blame_line->interactive) {
    active_editor_blame_popup_line_ = blame_line->line_index;
    return;
  }

  if (const auto popup = ActiveEditorBlamePopupLayout();
      popup.has_value() && Contains(popup->rect, x, y)) {
    return;
  }

  active_editor_blame_popup_line_.reset();
}

void WorkspaceShell::InvalidateEditorBlamePath(const std::filesystem::path& path) {
  if (project_root_.empty() || path.empty()) {
    return;
  }
  git_blame_service_.InvalidatePath(project_root_, path.lexically_normal());
}

void WorkspaceShell::ClearEditorBlame() {
  visible_editor_blame_overlay_.reset();
  active_editor_blame_popup_line_.reset();
  git_blame_service_.Clear();
}

}  // namespace microide::workspace
