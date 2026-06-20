#include "workspace/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "editor/DiagnosticsRender.h"
#include "util/PerformanceTrace.h"
#include "workspace/RenderViewModelBuilder.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

using namespace detail;

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
constexpr std::size_t kEditorHoverPopupMaxPluginTitleLines = 2;
constexpr std::size_t kEditorHoverPopupMaxPluginContentLines = 6;
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

void DrawHoverPopupLines(const render::TextRenderer& text_renderer,
                        SDL_Renderer* renderer,
                        float x,
                        float* y,
                        SDL_Color foreground,
                        SDL_Color background,
                        const std::vector<std::string>& lines,
                        float line_gap = kEditorHoverPopupLineGap) {
  if (y == nullptr) {
    return;
  }
  for (std::size_t i = 0; i < lines.size(); ++i) {
    text_renderer.DrawStringOn(renderer, x, *y, foreground, background, lines[i]);
    *y += text_renderer.LineHeight();
    if (i + 1 < lines.size()) {
      *y += line_gap;
    }
  }
}

}  // namespace

std::optional<WorkspaceShell::EditorHoverPopupLayout> WorkspaceShell::ActiveEditorHoverPopupLayout()
    const {
  if (MenuSurfaceCapturingMouse()) {
    return std::nullopt;
  }
  const HoverPopupViewModel hover_popup_vm =
      RenderViewModelBuilder(context_).BuildHoverPopup(active_editor_hover_target_.has_value());
  if (!hover_popup_vm.visible || !hover_popup_vm.has_active_target ||
      !active_editor_hover_target_.has_value()) {
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
        editor_blame_overlay_service_.VisibleLine(active_editor_hover_target_->blame_line_index);
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
        .plugin_hover = std::nullopt,
        .primary_action_rect = action_rect,
    };
  }

  if (active_editor_hover_target_->kind == EditorHoverTarget::Kind::Plugin) {
    if (!active_editor_hover_target_->plugin_hover.has_value()) {
      return std::nullopt;
    }
    const plugin::PluginHost::HoverResult& hover = *active_editor_hover_target_->plugin_hover;
    return EditorHoverPopupLayout{
        .kind = EditorHoverTarget::Kind::Plugin,
        .anchor_rect = active_editor_hover_target_->anchor_rect,
        .rect = ComputeTwoBlockHoverCardRect(
            hover.title, hover.content, kEditorHoverPopupMaxPluginTitleLines,
            kEditorHoverPopupMaxPluginContentLines, active_editor_hover_target_->anchor_rect,
            layout.editor_surface),
        .blame_line_index = 0,
        .diagnostic = std::nullopt,
        .plugin_hover = hover,
        .debug_value = std::nullopt,
        .primary_action_rect = std::nullopt,
    };
  }

  if (active_editor_hover_target_->kind == EditorHoverTarget::Kind::DebugValue) {
    if (!active_editor_hover_target_->debug_value.has_value()) {
      return std::nullopt;
    }
    const DebugHoverValue& debug_value = *active_editor_hover_target_->debug_value;
    if (debug_value.value.empty() && debug_value.type.empty()) {
      return std::nullopt;
    }
    // The value is the primary content; the type (when present) is the muted title.
    return EditorHoverPopupLayout{
        .kind = EditorHoverTarget::Kind::DebugValue,
        .anchor_rect = active_editor_hover_target_->anchor_rect,
        .rect = ComputeTwoBlockHoverCardRect(
            debug_value.type, debug_value.value, kEditorHoverPopupMaxPluginTitleLines,
            kEditorHoverPopupMaxPluginContentLines, active_editor_hover_target_->anchor_rect,
            layout.editor_surface),
        .blame_line_index = 0,
        .diagnostic = std::nullopt,
        .plugin_hover = std::nullopt,
        .debug_value = debug_value,
        .primary_action_rect = std::nullopt,
    };
  }

  if (!active_editor_hover_target_->diagnostic.has_value()) {
    return std::nullopt;
  }
  const editor::PublishedDiagnostic& diagnostic = *active_editor_hover_target_->diagnostic;
  const std::string_view severity = DiagnosticSeverityLabel(diagnostic.severity);
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
      .plugin_hover = std::nullopt,
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

SDL_FRect WorkspaceShell::ComputeTwoBlockHoverCardRect(std::string_view title,
                                                       std::string_view content,
                                                       std::size_t max_title_lines,
                                                       std::size_t max_content_lines,
                                                       const SDL_FRect& anchor_rect,
                                                       const SDL_FRect& editor_surface) const {
  const float available_width = std::max(220.0f, editor_surface.w - 16.0f);
  const float max_card_width = std::min(kEditorHoverPopupMaxWidth, available_width);
  const float line_height = text_renderer_.LineHeight();
  const float title_width =
      title.empty() ? 0.0f
                    : std::min(max_card_width - kEditorHoverPopupPadding * 2.0f,
                               text_renderer_.MeasureWidth(title));
  const float content_width =
      content.empty() ? 0.0f
                      : std::min(max_card_width - kEditorHoverPopupPadding * 2.0f,
                                 text_renderer_.MeasureWidth(content));
  const float minimum_width =
      std::max(title_width, content_width) + kEditorHoverPopupPadding * 2.0f;
  const float preferred_width = std::max(kEditorHoverPopupMinWidth, minimum_width);
  const float card_width = ClampPopupWidth(preferred_width, minimum_width, max_card_width);
  const float wrap_width = std::max(0.0f, card_width - kEditorHoverPopupPadding * 2.0f);
  const auto title_lines = title.empty()
                               ? std::vector<std::string>{}
                               : WrapEditorHoverPopupText(title, wrap_width, max_title_lines);
  const auto content_lines = content.empty()
                                 ? std::vector<std::string>{}
                                 : WrapEditorHoverPopupText(content, wrap_width, max_content_lines);
  const auto block_height = [line_height](const std::vector<std::string>& lines) {
    return static_cast<float>(lines.size()) * line_height +
           (lines.empty() ? 0.0f
                          : static_cast<float>(lines.size() - 1) * kEditorHoverPopupLineGap);
  };
  const float card_height =
      kEditorHoverPopupPadding * 2.0f + block_height(title_lines) +
      (title_lines.empty() || content_lines.empty() ? 0.0f : kEditorHoverPopupSectionGap) +
      block_height(content_lines);
  return PositionHoverPopup(anchor_rect, card_width, card_height, editor_surface);
}

void WorkspaceShell::DrawTwoBlockHoverCard(SDL_Renderer* renderer,
                                           const SDL_FRect& card_rect,
                                           std::string_view title,
                                           std::string_view content,
                                           std::size_t max_title_lines,
                                           std::size_t max_content_lines) const {
  const float text_x = card_rect.x + kEditorHoverPopupPadding;
  const float text_width = std::max(0.0f, card_rect.w - kEditorHoverPopupPadding * 2.0f);
  float text_y = card_rect.y + kEditorHoverPopupPadding;
  const auto title_lines = title.empty()
                               ? std::vector<std::string>{}
                               : WrapEditorHoverPopupText(title, text_width, max_title_lines);
  const auto content_lines = content.empty()
                                 ? std::vector<std::string>{}
                                 : WrapEditorHoverPopupText(content, text_width, max_content_lines);
  DrawHoverPopupLines(text_renderer_, renderer, text_x, &text_y, theme_.text_secondary,
                      theme_.overlay_background, title_lines);
  if (!title_lines.empty() && !content_lines.empty()) {
    text_y += kEditorHoverPopupSectionGap;
  }
  DrawHoverPopupLines(text_renderer_, renderer, text_x, &text_y, theme_.text_primary,
                      theme_.overlay_background, content_lines);
}

void WorkspaceShell::RenderEditorHoverPopup(SDL_Renderer* renderer) const {
  const auto popup = ActiveEditorHoverPopupLayout();
  if (!popup.has_value()) {
    return;
  }

  DrawCardFrame(renderer, theme_, popup->rect, CardStyle::Overlay);
  const float text_x = popup->rect.x + kEditorHoverPopupPadding;
  const float text_width =
      std::max(0.0f, popup->rect.w - kEditorHoverPopupPadding * 2.0f);
  float text_y = popup->rect.y + kEditorHoverPopupPadding;

  if (popup->kind == EditorHoverTarget::Kind::Blame) {
    const editor::EditorBlameLine* blame_line =
        editor_blame_overlay_service_.VisibleLine(popup->blame_line_index);
    if (blame_line != nullptr) {
      const auto summary_lines =
          WrapEditorHoverPopupText(blame_line->summary, text_width, kEditorHoverPopupMaxSummaryLines);
      text_renderer_.DrawStringOn(
          renderer, text_x, text_y, theme_.text_primary, theme_.overlay_background,
          text_renderer_.TruncateToWidth(blame_line->author, text_width));
      text_y += text_renderer_.LineHeight() + kEditorHoverPopupLineGap;
      text_renderer_.DrawStringOn(
          renderer, text_x, text_y, theme_.text_secondary, theme_.overlay_background,
          text_renderer_.TruncateToWidth(blame_line->date, text_width));
      if (!summary_lines.empty()) {
        text_y += text_renderer_.LineHeight() + kEditorHoverPopupSectionGap;
        DrawHoverPopupLines(text_renderer_, renderer, text_x, &text_y, theme_.text_primary,
                            theme_.overlay_background, summary_lines);
      }
    }
  } else if (popup->kind == EditorHoverTarget::Kind::Plugin && popup->plugin_hover.has_value()) {
    DrawTwoBlockHoverCard(renderer, popup->rect, popup->plugin_hover->title,
                          popup->plugin_hover->content, kEditorHoverPopupMaxPluginTitleLines,
                          kEditorHoverPopupMaxPluginContentLines);
  } else if (popup->kind == EditorHoverTarget::Kind::DebugValue &&
             popup->debug_value.has_value()) {
    DrawTwoBlockHoverCard(renderer, popup->rect, popup->debug_value->type,
                          popup->debug_value->value, kEditorHoverPopupMaxPluginTitleLines,
                          kEditorHoverPopupMaxPluginContentLines);
  } else if (popup->diagnostic.has_value()) {
    const SDL_Color severity_color =
        editor::DiagnosticSeverityColor(theme_, popup->diagnostic->severity);
    const auto message_lines = WrapEditorHoverPopupText(
        popup->diagnostic->message, text_width, kEditorHoverPopupMaxDiagnosticLines);
    text_renderer_.DrawStringOn(
        renderer, text_x, text_y, severity_color, theme_.overlay_background,
        text_renderer_.TruncateToWidth(DiagnosticSeverityLabel(popup->diagnostic->severity),
                                       text_width));
    if (!message_lines.empty()) {
      text_y += text_renderer_.LineHeight() + kEditorHoverPopupSectionGap;
      DrawHoverPopupLines(text_renderer_, renderer, text_x, &text_y, theme_.text_primary,
                          theme_.overlay_background, message_lines);
    }
  }

  if (popup->primary_action_rect.has_value()) {
    const bool action_hovered = last_mouse_position_valid_ &&
                                EditorHoverPopupPrimaryActionHovered(last_mouse_x_, last_mouse_y_);
    DrawButtonCentered(text_renderer_, renderer, theme_, *popup->primary_action_rect, "Copy SHA",
                       ButtonTone::Accent,
                       ButtonVisualState{
                           .enabled = true,
                           .hovered = action_hovered,
                           .active = false,
                       });
  }
}

void WorkspaceShell::UpdateEditorHover(float x, float y) {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::UpdateEditorHover");
  if (MenuSurfaceCapturingMouse()) {
    active_editor_hover_target_.reset();
    editor_hover_refresh_pending_ = false;
    return;
  }
  const std::optional<EditorHoverTarget> previous_target = active_editor_hover_target_;
  // The (const) resolver records a debug-value cache miss here; clear it first so a
  // stale request from a previous position is not re-issued.
  pending_hover_eval_.valid = false;
  const auto target = [&]() -> std::optional<EditorHoverTarget> {
    util::PerformanceTrace::Scope scope("WorkspaceShell::UpdateEditorHover::TargetAtPosition");
    return EditorHoverTargetAtPosition(x, y);
  }();
  // Cache miss for a debug-value hover: kick off the async evaluate. Its completion
  // requests an editor redraw, re-running this resolution into a cache hit.
  if (pending_hover_eval_.valid) {
    debug_service_.EvaluateHover(pending_hover_eval_.frame_id, pending_hover_eval_.expression);
    pending_hover_eval_.valid = false;
  }
  if (target.has_value()) {
    if (!previous_target.has_value() || *previous_target != *target) {
      ++editor_hover_target_generation_;
    }
    active_editor_hover_target_ = *target;
    return;
  }

  if (const auto popup = ActiveEditorHoverPopupLayout(); popup.has_value()) {
    if (Contains(popup->rect, x, y) ||
        Contains(HoverPopupHoverZoneRect(popup->anchor_rect, popup->rect), x, y)) {
      return;
    }
  }

  if (previous_target.has_value()) {
    ++editor_hover_target_generation_;
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
