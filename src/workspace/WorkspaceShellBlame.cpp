#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>

#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

constexpr std::size_t kCaretBlameRadius = 1;
constexpr std::size_t kInlineBlameGapColumns = 8;
constexpr std::size_t kMinimumCodeColumnsWithBlame = 24;
constexpr float kMinimumEditorBlamePaneWidth = 520.0f;
constexpr float kBlamePopupPadding = 12.0f;
constexpr float kBlamePopupGap = 6.0f;
constexpr float kBlamePopupMinWidth = 320.0f;
constexpr float kBlamePopupMaxWidth = 640.0f;
constexpr float kBlamePopupHoverMargin = 4.0f;
constexpr float kBlamePopupLineGap = 2.0f;
constexpr float kBlamePopupSectionGap = 8.0f;
constexpr std::size_t kBlamePopupMaxSummaryLines = 4;
constexpr float kBlamePopupCopyShaHitPaddingX = 12.0f;
constexpr float kBlamePopupCopyShaHitPaddingY = 6.0f;

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

SDL_FRect BlamePopupHoverZoneRect(const SDL_FRect& blame_rect, const SDL_FRect& popup_rect) {
  const float left = std::min(blame_rect.x, popup_rect.x) - kBlamePopupHoverMargin;
  const float top = std::min(blame_rect.y, popup_rect.y) - kBlamePopupHoverMargin;
  const float right = std::max(blame_rect.x + blame_rect.w, popup_rect.x + popup_rect.w) +
                      kBlamePopupHoverMargin;
  const float bottom =
      std::max(blame_rect.y + blame_rect.h, popup_rect.y + popup_rect.h) +
      kBlamePopupHoverMargin;
  return MakeRect(left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top));
}

std::string NormalizePopupText(std::string_view text) {
  std::string normalized;
  normalized.reserve(text.size());
  bool last_was_space = true;
  for (char c : text) {
    const bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
    if (space) {
      if (!last_was_space) {
        normalized.push_back(' ');
      }
      last_was_space = true;
      continue;
    }
    normalized.push_back(c);
    last_was_space = false;
  }
  while (!normalized.empty() && normalized.back() == ' ') {
    normalized.pop_back();
  }
  return normalized;
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
  const float available_width = std::max(220.0f, layout.editor_surface.w - 16.0f);
  const float max_card_width = std::min(kBlamePopupMaxWidth, available_width);
  const float base_content_width = std::max({text_renderer_.MeasureWidth(blame_line->author),
                                             text_renderer_.MeasureWidth(blame_line->date),
                                             copy_width});
  const float summary_content_width =
      std::min(max_card_width - kBlamePopupPadding * 2.0f,
               text_renderer_.MeasureWidth(blame_line->summary));
  const float card_width = std::clamp(
      std::max(kBlamePopupMinWidth,
               std::max(base_content_width, summary_content_width) + kBlamePopupPadding * 2.0f),
      base_content_width + kBlamePopupPadding * 2.0f, max_card_width);
  const auto summary_lines =
      WrapEditorBlamePopupText(blame_line->summary,
                               std::max(0.0f, card_width - kBlamePopupPadding * 2.0f),
                               kBlamePopupMaxSummaryLines);
  const float button_height = text_renderer_.LineHeight() + 8.0f;
  const float line_height = text_renderer_.LineHeight();
  const float summary_height =
      static_cast<float>(summary_lines.size()) * line_height +
      (summary_lines.empty() ? 0.0f : static_cast<float>(summary_lines.size() - 1) * kBlamePopupLineGap);
  const float card_height =
      kBlamePopupPadding * 2.0f + line_height * 2.0f + kBlamePopupLineGap +
      (summary_lines.empty() ? 0.0f : kBlamePopupSectionGap + summary_height) +
      kBlamePopupSectionGap + button_height;

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

SDL_FRect WorkspaceShell::EditorBlamePopupCopyShaHitRect(const EditorBlamePopupLayout& popup) const {
  return MakeRect(popup.copy_sha_rect.x - kBlamePopupCopyShaHitPaddingX,
                  popup.copy_sha_rect.y - kBlamePopupCopyShaHitPaddingY,
                  popup.copy_sha_rect.w + kBlamePopupCopyShaHitPaddingX * 2.0f,
                  popup.copy_sha_rect.h + kBlamePopupCopyShaHitPaddingY * 2.0f);
}

bool WorkspaceShell::EditorBlamePopupCopyShaHovered(float x, float y) const {
  const auto popup = ActiveEditorBlamePopupLayout();
  return popup.has_value() && Contains(EditorBlamePopupCopyShaHitRect(*popup), x, y);
}

std::vector<std::string> WorkspaceShell::WrapEditorBlamePopupText(std::string_view text,
                                                                  float max_width,
                                                                  std::size_t max_lines) const {
  std::vector<std::string> lines;
  if (max_lines == 0 || max_width <= 0.0f) {
    return lines;
  }

  const std::string normalized = NormalizePopupText(text);
  if (normalized.empty()) {
    return lines;
  }
  if (text_renderer_.MeasureWidth(normalized) <= max_width) {
    lines.push_back(normalized);
    return lines;
  }

  std::istringstream stream(normalized);
  std::vector<std::string> words;
  for (std::string word; stream >> word;) {
    words.push_back(std::move(word));
  }
  if (words.empty()) {
    lines.push_back(text_renderer_.TruncateToWidth(normalized, max_width));
    return lines;
  }

  std::size_t index = 0;
  while (index < words.size() && lines.size() + 1 < max_lines) {
    std::string line = words[index];
    ++index;
    while (index < words.size()) {
      const std::string candidate = line + " " + words[index];
      if (text_renderer_.MeasureWidth(candidate) > max_width) {
        break;
      }
      line = candidate;
      ++index;
    }
    lines.push_back(std::move(line));
  }

  std::string remaining;
  while (index < words.size()) {
    if (!remaining.empty()) {
      remaining += ' ';
    }
    remaining += words[index];
    ++index;
  }
  if (!remaining.empty() && lines.size() < max_lines) {
    lines.push_back(text_renderer_.TruncateToWidth(remaining, max_width));
  }
  if (lines.empty()) {
    lines.push_back(text_renderer_.TruncateToWidth(normalized, max_width));
  }
  return lines;
}

void WorkspaceShell::UpdateEditorBlameHover(float x, float y) {
  if (const editor::EditorBlameLine* blame_line = EditorBlameLineAtPosition(x, y);
      blame_line != nullptr && blame_line->interactive) {
    active_editor_blame_popup_line_ = blame_line->line_index;
    return;
  }

  if (const auto popup = ActiveEditorBlamePopupLayout(); popup.has_value()) {
    if (Contains(popup->rect, x, y)) {
      return;
    }
    if (const editor::EditorBlameLine* popup_line = VisibleEditorBlameLine(popup->line_index);
        popup_line != nullptr &&
        Contains(BlamePopupHoverZoneRect(popup_line->rect, popup->rect), x, y)) {
      return;
    }
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
