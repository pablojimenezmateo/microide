#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "editor/DiagnosticsRender.h"
#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

constexpr float kEditorHoverPopupPadding = 12.0f;
constexpr float kEditorHoverPopupGap = 6.0f;
constexpr float kEditorHoverPopupMinWidth = 280.0f;
constexpr float kEditorHoverPopupMaxWidth = 640.0f;
constexpr float kEditorHoverPopupHoverMargin = 4.0f;
constexpr float kEditorHoverPopupLineGap = 2.0f;
constexpr float kEditorHoverPopupSectionGap = 8.0f;
constexpr std::size_t kEditorHoverPopupMaxSummaryLines = 4;
constexpr std::size_t kEditorHoverPopupMaxDiagnosticLines = 6;
constexpr float kEditorHoverPopupPrimaryActionHitPaddingX = 12.0f;
constexpr float kEditorHoverPopupPrimaryActionHitPaddingY = 6.0f;

SDL_FRect HoverPopupHoverZoneRect(const SDL_FRect& anchor_rect, const SDL_FRect& popup_rect) {
  const float left = std::min(anchor_rect.x, popup_rect.x) - kEditorHoverPopupHoverMargin;
  const float top = std::min(anchor_rect.y, popup_rect.y) - kEditorHoverPopupHoverMargin;
  const float right =
      std::max(anchor_rect.x + anchor_rect.w, popup_rect.x + popup_rect.w) +
      kEditorHoverPopupHoverMargin;
  const float bottom =
      std::max(anchor_rect.y + anchor_rect.h, popup_rect.y + popup_rect.h) +
      kEditorHoverPopupHoverMargin;
  return MakeRect(left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top));
}

std::string NormalizeHoverPopupText(std::string_view text) {
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

std::string DiagnosticSeverityLabel(editor::DiagnosticSeverity severity) {
  switch (severity) {
    case editor::DiagnosticSeverity::Error:
      return "Error";
    case editor::DiagnosticSeverity::Warning:
      return "Warning";
    case editor::DiagnosticSeverity::Info:
      return "Info";
    case editor::DiagnosticSeverity::Hint:
      return "Hint";
  }
  return "Diagnostic";
}

float ClampPopupWidth(float preferred_width, float minimum_width, float max_width) {
  const float clamped_max = std::max(1.0f, max_width);
  const float clamped_min = std::min(minimum_width, clamped_max);
  return std::max(clamped_min, std::min(preferred_width, clamped_max));
}

SDL_FRect PositionHoverPopup(const SDL_FRect& anchor_rect,
                             float card_width,
                             float card_height,
                             const SDL_FRect& editor_surface) {
  float x = anchor_rect.x;
  float y = anchor_rect.y + anchor_rect.h + kEditorHoverPopupGap;
  if (x + card_width > editor_surface.x + editor_surface.w - 8.0f) {
    x = editor_surface.x + editor_surface.w - card_width - 8.0f;
  }
  if (y + card_height > editor_surface.y + editor_surface.h - 8.0f) {
    y = anchor_rect.y - card_height - kEditorHoverPopupGap;
  }
  x = std::max(editor_surface.x + 8.0f, x);
  y = std::max(editor_surface.y + 8.0f, y);
  return MakeRect(x, y, card_width, card_height);
}

TextGridInteractionLayout BuildEditorInteractionLayout(
    const render::TextRenderer& text_renderer,
    const editor::TextViewport& viewport,
    const SDL_FRect& rect) {
  const editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(text_renderer, viewport, rect);
  return ComputeTextGridInteractionLayout(
      rect, metrics.text_x, metrics.first_line_y, metrics.line_height, text_renderer.CharWidth(),
      viewport.scroll_line(), viewport.line_count(), viewport.horizontal_scroll(),
      metrics.visible_rows, metrics.visible_columns);
}

}  // namespace

std::optional<WorkspaceShell::EditorHoverTarget> WorkspaceShell::DiagnosticHoverTargetForViewport(
    const editor::TextViewport& viewport,
    const TextGridInteractionLayout& interaction,
    std::span<const editor::PublishedDiagnostic> diagnostics,
    float x,
    float y) const {
  if (viewport.path().empty() || viewport.dirty() || diagnostics.empty() ||
      !Contains(interaction.rect, x, y)) {
    return std::nullopt;
  }

  const std::optional<std::size_t> line_index = VisibleTextGridLineAtY(interaction, y);
  if (!line_index.has_value() || *line_index >= viewport.lines().size()) {
    return std::nullopt;
  }

  const float line_y =
      interaction.first_line_y +
      static_cast<float>(*line_index - interaction.scroll_line) * interaction.line_height;
  const std::string& line = viewport.lines()[*line_index];
  for (const editor::PublishedDiagnostic& diagnostic : diagnostics) {
    const auto rect = editor::DiagnosticUnderlineRect(
        text_renderer_, interaction.text_x, line_y, interaction.line_height, line, *line_index,
        interaction.horizontal_scroll, interaction.visible_columns, viewport.tab_size(),
        diagnostic);
    if (rect.has_value() && Contains(*rect, x, y)) {
      return EditorHoverTarget{
          .kind = EditorHoverTarget::Kind::Diagnostic,
          .anchor_rect = *rect,
          .blame_line_index = 0,
          .diagnostic = diagnostic,
      };
    }
  }

  return std::nullopt;
}

std::optional<WorkspaceShell::EditorHoverTarget> WorkspaceShell::DiagnosticHoverTargetAtPosition(
    float x,
    float y) const {
  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return std::nullopt;
  }
  const WorkspaceLayout layout = *layout_state;

  if (ActiveTabIsCompare()) {
    const CompareTabState* compare_tab = ActiveCompareTab();
    if (compare_tab == nullptr || !compare_tab->right_editable ||
        compare_tab->right_viewport.path().empty() || compare_tab->right_viewport.dirty()) {
      return std::nullopt;
    }

    const auto* diagnostics = diagnostics_store_.FindByPath(compare_tab->right_viewport.path());
    if (diagnostics == nullptr || diagnostics->empty()) {
      return std::nullopt;
    }

    const CompareSurfaceLayout surface =
        ComputeCompareSurfaceLayout(layout.editor_surface, *compare_tab);
    const TextGridInteractionLayout interaction = ComputeTextGridInteractionLayout(
        MakeRect(surface.right_x, surface.rows_y, surface.gutter_width + surface.right_width,
                 static_cast<float>(surface.visible_rows) * surface.line_height),
        surface.right_x + surface.gutter_width, surface.rows_y, surface.line_height,
        text_renderer_.CharWidth(), static_cast<std::size_t>(std::max(0, compare_tab->scroll_row)),
        compare_tab->model.rows.size(), compare_tab->horizontal_scroll,
        static_cast<std::size_t>(surface.visible_rows), surface.right_visible_columns);
    if (!Contains(interaction.rect, x, y)) {
      return std::nullopt;
    }

    const std::optional<std::size_t> model_row = VisibleTextGridLineAtY(interaction, y);
    if (!model_row.has_value() || *model_row >= compare_tab->model.rows.size()) {
      return std::nullopt;
    }

    const auto& row = compare_tab->model.rows[*model_row];
    if (row.right_line <= 0 || static_cast<std::size_t>(row.right_line) > compare_tab->right_viewport.lines().size()) {
      return std::nullopt;
    }

    const std::size_t line_index = static_cast<std::size_t>(row.right_line - 1);
    const float line_y =
        surface.rows_y +
        static_cast<float>(*model_row - static_cast<std::size_t>(std::max(0, compare_tab->scroll_row))) *
            surface.line_height;
    for (const editor::PublishedDiagnostic& diagnostic : *diagnostics) {
      const auto rect = editor::DiagnosticUnderlineRect(
          text_renderer_, interaction.text_x, line_y, surface.line_height, row.right_text,
          line_index, compare_tab->horizontal_scroll, surface.right_visible_columns,
          compare_tab->right_viewport.tab_size(), diagnostic);
      if (rect.has_value() && Contains(*rect, x, y)) {
        return EditorHoverTarget{
            .kind = EditorHoverTarget::Kind::Diagnostic,
            .anchor_rect = *rect,
            .blame_line_index = 0,
            .diagnostic = diagnostic,
        };
      }
    }
    return std::nullopt;
  }

  if (ActiveTabIsMerge()) {
    const MergeTabState* merge_tab = ActiveMergeTab();
    if (merge_tab == nullptr || merge_tab->result_viewport.path().empty() ||
        merge_tab->result_viewport.dirty()) {
      return std::nullopt;
    }

    const auto* diagnostics = diagnostics_store_.FindByPath(merge_tab->result_viewport.path());
    if (diagnostics == nullptr || diagnostics->empty()) {
      return std::nullopt;
    }

    const MergeSurfaceLayout surface = ComputeMergeSurfaceLayout(layout.editor_surface, *merge_tab);
    const SDL_FRect result_rect = ComputeMergeResultViewportRect(
        layout.editor_surface, surface.center_x, surface.rows_y, surface.gutter_width,
        surface.center_width, surface.show_horizontal);
    return DiagnosticHoverTargetForViewport(
        merge_tab->result_viewport,
        BuildEditorInteractionLayout(text_renderer_, merge_tab->result_viewport, result_rect),
        std::span<const editor::PublishedDiagnostic>(*diagnostics), x, y);
  }

  if (!ActiveTabIsEditor()) {
    return std::nullopt;
  }

  const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
  const TabEntry::EditorTabState* editor_tab = ActiveEditorTab();
  if (panes.empty() && !text_viewport_.is_placeholder()) {
    const auto* diagnostics = diagnostics_store_.FindByPath(text_viewport_.path());
    return diagnostics != nullptr
               ? DiagnosticHoverTargetForViewport(
                     text_viewport_,
                     BuildEditorInteractionLayout(text_renderer_, text_viewport_,
                                                  layout.editor_surface),
                     std::span<const editor::PublishedDiagnostic>(*diagnostics), x, y)
               : std::nullopt;
  }

  for (const EditorPaneLayout& pane : panes) {
    const editor::TextViewport* viewport =
        pane.active ? &text_viewport_
                    : (editor_tab != nullptr ? FindEditorView(*editor_tab, pane.leaf_id) : nullptr);
    if (viewport == nullptr || viewport->path().empty() || viewport->dirty()) {
      continue;
    }

    const auto* diagnostics = diagnostics_store_.FindByPath(viewport->path());
    if (diagnostics == nullptr || diagnostics->empty()) {
      continue;
    }

    if (const auto target = DiagnosticHoverTargetForViewport(
            *viewport, BuildEditorInteractionLayout(text_renderer_, *viewport, pane.rect),
            std::span<const editor::PublishedDiagnostic>(*diagnostics), x, y);
        target.has_value()) {
      return target;
    }
  }

  return std::nullopt;
}

std::optional<WorkspaceShell::EditorHoverTarget> WorkspaceShell::EditorHoverTargetAtPosition(
    float x,
    float y) const {
  if (const editor::EditorBlameLine* blame_line = EditorBlameLineAtPosition(x, y);
      blame_line != nullptr && blame_line->interactive) {
    return EditorHoverTarget{
        .kind = EditorHoverTarget::Kind::Blame,
        .anchor_rect = blame_line->rect,
        .blame_line_index = blame_line->line_index,
        .diagnostic = std::nullopt,
    };
  }

  return DiagnosticHoverTargetAtPosition(x, y);
}

std::optional<WorkspaceShell::EditorHoverPopupLayout> WorkspaceShell::ActiveEditorHoverPopupLayout()
    const {
  if (!active_editor_hover_target_.has_value()) {
    return std::nullopt;
  }

  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return std::nullopt;
  }
  const WorkspaceLayout layout = *layout_state;
  const float available_width = std::max(220.0f, layout.editor_surface.w - 16.0f);
  const float max_card_width = std::min(kEditorHoverPopupMaxWidth, available_width);
  const float line_height = text_renderer_.LineHeight();

  if (active_editor_hover_target_->kind == EditorHoverTarget::Kind::Blame) {
    const editor::EditorBlameLine* blame_line =
        VisibleEditorBlameLine(active_editor_hover_target_->blame_line_index);
    if (blame_line == nullptr || !blame_line->interactive) {
      return std::nullopt;
    }

    const float copy_width = std::max(84.0f, text_renderer_.MeasureWidth("Copy SHA") + 18.0f);
    const float base_content_width = std::max(
        {text_renderer_.MeasureWidth(blame_line->author), text_renderer_.MeasureWidth(blame_line->date),
         copy_width});
    const float summary_content_width =
        std::min(max_card_width - kEditorHoverPopupPadding * 2.0f,
                 text_renderer_.MeasureWidth(blame_line->summary));
    const float preferred_width = std::max(
        kEditorHoverPopupMinWidth,
        std::max(base_content_width, summary_content_width) + kEditorHoverPopupPadding * 2.0f);
    const float card_width = ClampPopupWidth(
        preferred_width, base_content_width + kEditorHoverPopupPadding * 2.0f, max_card_width);
    const auto summary_lines = WrapEditorHoverPopupText(
        blame_line->summary, std::max(0.0f, card_width - kEditorHoverPopupPadding * 2.0f),
        kEditorHoverPopupMaxSummaryLines);
    const float button_height = line_height + 8.0f;
    const float summary_height =
        static_cast<float>(summary_lines.size()) * line_height +
        (summary_lines.empty() ? 0.0f
                               : static_cast<float>(summary_lines.size() - 1) *
                                     kEditorHoverPopupLineGap);
    const float card_height =
        kEditorHoverPopupPadding * 2.0f + line_height * 2.0f + kEditorHoverPopupLineGap +
        (summary_lines.empty() ? 0.0f : kEditorHoverPopupSectionGap + summary_height) +
        kEditorHoverPopupSectionGap + button_height;
    const SDL_FRect card_rect = PositionHoverPopup(active_editor_hover_target_->anchor_rect,
                                                   card_width, card_height, layout.editor_surface);
    const SDL_FRect action_rect = MakeRect(
        card_rect.x + card_rect.w - copy_width - kEditorHoverPopupPadding,
        card_rect.y + card_rect.h - button_height - kEditorHoverPopupPadding, copy_width,
        button_height);
    return EditorHoverPopupLayout{
        .kind = EditorHoverTarget::Kind::Blame,
        .anchor_rect = active_editor_hover_target_->anchor_rect,
        .rect = card_rect,
        .blame_line_index = blame_line->line_index,
        .diagnostic = std::nullopt,
        .primary_action_rect = action_rect,
    };
  }

  if (!active_editor_hover_target_->diagnostic.has_value()) {
    return std::nullopt;
  }
  const editor::PublishedDiagnostic& diagnostic = *active_editor_hover_target_->diagnostic;
  const std::string severity = DiagnosticSeverityLabel(diagnostic.severity);
  const float severity_width = text_renderer_.MeasureWidth(severity);
  const float message_width =
      std::min(max_card_width - kEditorHoverPopupPadding * 2.0f,
               text_renderer_.MeasureWidth(diagnostic.message));
  const float preferred_width = std::max(
      kEditorHoverPopupMinWidth,
      std::max(severity_width, message_width) + kEditorHoverPopupPadding * 2.0f);
  const float card_width = ClampPopupWidth(
      preferred_width, severity_width + kEditorHoverPopupPadding * 2.0f, max_card_width);
  const auto message_lines = WrapEditorHoverPopupText(
      diagnostic.message, std::max(0.0f, card_width - kEditorHoverPopupPadding * 2.0f),
      kEditorHoverPopupMaxDiagnosticLines);
  const float message_height =
      static_cast<float>(message_lines.size()) * line_height +
      (message_lines.empty() ? 0.0f
                             : static_cast<float>(message_lines.size() - 1) *
                                   kEditorHoverPopupLineGap);
  const float card_height = kEditorHoverPopupPadding * 2.0f + line_height +
                            (message_lines.empty() ? 0.0f
                                                   : kEditorHoverPopupSectionGap + message_height);
  const SDL_FRect card_rect = PositionHoverPopup(active_editor_hover_target_->anchor_rect,
                                                 card_width, card_height, layout.editor_surface);
  return EditorHoverPopupLayout{
      .kind = EditorHoverTarget::Kind::Diagnostic,
      .anchor_rect = active_editor_hover_target_->anchor_rect,
      .rect = card_rect,
      .blame_line_index = 0,
      .diagnostic = diagnostic,
      .primary_action_rect = std::nullopt,
  };
}

SDL_FRect WorkspaceShell::EditorHoverPopupPrimaryActionHitRect(
    const EditorHoverPopupLayout& popup) const {
  if (!popup.primary_action_rect.has_value()) {
    return {};
  }

  return MakeRect(
      popup.primary_action_rect->x - kEditorHoverPopupPrimaryActionHitPaddingX,
      popup.primary_action_rect->y - kEditorHoverPopupPrimaryActionHitPaddingY,
      popup.primary_action_rect->w + kEditorHoverPopupPrimaryActionHitPaddingX * 2.0f,
      popup.primary_action_rect->h + kEditorHoverPopupPrimaryActionHitPaddingY * 2.0f);
}

bool WorkspaceShell::EditorHoverPopupPrimaryActionHovered(float x, float y) const {
  const auto popup = ActiveEditorHoverPopupLayout();
  return popup.has_value() && popup->primary_action_rect.has_value() &&
         Contains(EditorHoverPopupPrimaryActionHitRect(*popup), x, y);
}

std::vector<std::string> WorkspaceShell::WrapEditorHoverPopupText(std::string_view text,
                                                                  float max_width,
                                                                  std::size_t max_lines) const {
  std::vector<std::string> lines;
  if (max_lines == 0 || max_width <= 0.0f) {
    return lines;
  }

  const std::string normalized = NormalizeHoverPopupText(text);
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

void WorkspaceShell::UpdateEditorHover(float x, float y) {
  if (const auto target = EditorHoverTargetAtPosition(x, y); target.has_value()) {
    active_editor_hover_target_ = *target;
    return;
  }

  if (const auto popup = ActiveEditorHoverPopupLayout(); popup.has_value()) {
    if (Contains(popup->rect, x, y) ||
        Contains(HoverPopupHoverZoneRect(popup->anchor_rect, popup->rect), x, y)) {
      return;
    }
  }

  active_editor_hover_target_.reset();
}

std::optional<WorkspaceShell::EditorBlamePopupLayout> WorkspaceShell::ActiveEditorBlamePopupLayout()
    const {
  const auto popup = ActiveEditorHoverPopupLayout();
  if (!popup.has_value() || popup->kind != EditorHoverTarget::Kind::Blame ||
      !popup->primary_action_rect.has_value()) {
    return std::nullopt;
  }

  return EditorBlamePopupLayout{
      .line_index = popup->blame_line_index,
      .rect = popup->rect,
      .copy_sha_rect = *popup->primary_action_rect,
  };
}

SDL_FRect WorkspaceShell::EditorBlamePopupCopyShaHitRect(
    const EditorBlamePopupLayout& popup) const {
  return MakeRect(
      popup.copy_sha_rect.x - kEditorHoverPopupPrimaryActionHitPaddingX,
      popup.copy_sha_rect.y - kEditorHoverPopupPrimaryActionHitPaddingY,
      popup.copy_sha_rect.w + kEditorHoverPopupPrimaryActionHitPaddingX * 2.0f,
      popup.copy_sha_rect.h + kEditorHoverPopupPrimaryActionHitPaddingY * 2.0f);
}

bool WorkspaceShell::EditorBlamePopupCopyShaHovered(float x, float y) const {
  return EditorHoverPopupPrimaryActionHovered(x, y);
}

std::vector<std::string> WorkspaceShell::WrapEditorBlamePopupText(std::string_view text,
                                                                  float max_width,
                                                                  std::size_t max_lines) const {
  return WrapEditorHoverPopupText(text, max_width, max_lines);
}

void WorkspaceShell::UpdateEditorBlameHover(float x, float y) {
  UpdateEditorHover(x, y);
}

}  // namespace microide::workspace
