#include "workspace/render/HoverCardPaint.h"
#include "workspace/render/WorkspaceShellRenderPrimitives.h"

#include "workspace/render/HoverCardLimits.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/DiagnosticsRender.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "workspace/FileUri.h"
#include "workspace/lsp/LspFeatureFlags.h"
#include "workspace/lsp/LspProtocol.h"
#include "workspace/lsp/LspViewportPositions.h"
#include "workspace/HoverTextWrap.h"
#include "workspace/render/EditorInteractionGrid.h"
#include "workspace/render/RenderViewModelBuilder.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

using namespace detail;

namespace {

constexpr std::size_t kMaxHoverTextBytes = 8192;

void TruncateHoverText(std::string& text) {
  if (text.size() > kMaxHoverTextBytes) {
    text.resize(util::PreviousUtf8Boundary(text, kMaxHoverTextBytes));
  }
}

constexpr float kEditorHoverPopupPadding = 12.0f;
constexpr float kEditorHoverPopupGap = 6.0f;
constexpr float kEditorHoverPopupMinWidth = 280.0f;
constexpr float kEditorHoverPopupMaxWidth = 640.0f;
constexpr float kEditorHoverPopupHoverMargin = 4.0f;
constexpr float kEditorHoverPopupLineGap = 2.0f;
constexpr float kEditorHoverPopupSectionGap = 8.0f;
constexpr std::size_t kEditorHoverPopupMaxSummaryLines = 4;
constexpr std::size_t kEditorHoverPopupMaxDiagnosticLines = 10;
constexpr float kEditorHoverPopupPrimaryActionHitPaddingX = 12.0f;
constexpr float kEditorHoverPopupPrimaryActionHitPaddingY = 6.0f;
constexpr std::string_view kQuickFixShortcutHint = "Ctrl+.";

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
  if (x + card_width > editor_surface.x + editor_surface.w - 8.0f) {
    x = editor_surface.x + editor_surface.w - card_width - 8.0f;
  }
  float y = anchor_rect.y - card_height - kEditorHoverPopupGap;
  if (y < editor_surface.y + 8.0f) {
    y = anchor_rect.y + anchor_rect.h + kEditorHoverPopupGap;
    if (y + card_height > editor_surface.y + editor_surface.h - 8.0f) {
      y = editor_surface.y + editor_surface.h - 8.0f - card_height;
    }
  }
  x = std::max(editor_surface.x + 8.0f, x);
  y = std::max(editor_surface.y + 8.0f, y);
  return MakeRect(x, y, card_width, card_height);
}

}

std::optional<EditorHoverPopupLayout> WorkspaceShell::ActiveEditorHoverPopupLayout()
    const {
  if (MenuSurfaceCapturingMouse()) {
    return std::nullopt;
  }
  const HoverPopupViewModel hover_popup_vm =
      RenderViewModelBuilder(context_).BuildHoverPopup(active_editor_hover_target_.has_value(),
                                                       settings_overlay_service_.Visible());
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
    if (blame_line == nullptr || !blame_line->interactive ||
        !RectsEqual(blame_line->rect, active_editor_hover_target_->anchor_rect)) {
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
        card_rect.x + kEditorHoverPopupPadding,
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
  const bool offer_quick_fix = diagnostic.owner == "lsp";
  const float button_height = line_height + 8.0f;
  const float quick_fix_width =
      offer_quick_fix ? std::max(96.0f, text_renderer_.MeasureWidth("Quick Fix") + 18.0f) : 0.0f;
  const float quick_fix_hint_width =
      offer_quick_fix ? text_renderer_.MeasureWidth(kQuickFixShortcutHint) : 0.0f;
  const float button_row_width =
      offer_quick_fix ? quick_fix_width + kEditorHoverPopupGap + quick_fix_hint_width : 0.0f;
  const std::string_view severity = DiagnosticSeverityLabel(diagnostic.severity);
  const float severity_width = text_renderer_.MeasureWidth(severity);
  const float max_text_width = max_card_width - kEditorHoverPopupPadding * 2.0f;
  const float message_width =
      std::min(max_text_width, text_renderer_.MeasureWidth(diagnostic.message));
  const std::optional<plugin::PluginHost::HoverResult>& symbol =
      active_editor_hover_target_->plugin_hover;
  const float symbol_width =
      symbol.has_value()
          ? std::min(max_text_width, std::max(text_renderer_.MeasureWidth(symbol->title),
                                              text_renderer_.MeasureWidth(symbol->content)))
          : 0.0f;
  const float preferred_width = std::max(
      kEditorHoverPopupMinWidth,
      std::max({severity_width, message_width, symbol_width, button_row_width}) +
          kEditorHoverPopupPadding * 2.0f);
  const float card_width = ClampPopupWidth(
      preferred_width,
      std::max(severity_width, button_row_width) + kEditorHoverPopupPadding * 2.0f, max_card_width);
  const auto message_lines = WrapEditorHoverPopupText(
      diagnostic.message, std::max(0.0f, card_width - kEditorHoverPopupPadding * 2.0f),
      kEditorHoverPopupMaxDiagnosticLines);
  const float message_height =
      static_cast<float>(message_lines.size()) * line_height +
      (message_lines.empty() ? 0.0f
                             : static_cast<float>(message_lines.size() - 1) *
                                   kEditorHoverPopupLineGap);
  float symbol_height = 0.0f;
  if (symbol.has_value()) {
    const float wrap_width = std::max(0.0f, card_width - kEditorHoverPopupPadding * 2.0f);
    const auto block_height = [&](std::string_view text, std::size_t max_lines) {
      if (text.empty()) {
        return 0.0f;
      }
      const std::size_t lines = WrapEditorHoverPopupText(text, wrap_width, max_lines).size();
      return static_cast<float>(lines) * line_height +
             (lines == 0 ? 0.0f : static_cast<float>(lines - 1) * kEditorHoverPopupLineGap);
    };
    const float title_height = block_height(symbol->title, kEditorHoverPopupMaxPluginTitleLines);
    const float content_height =
        block_height(symbol->content, kEditorHoverPopupMaxPluginContentLines);
    symbol_height = kEditorHoverPopupSectionGap + title_height +
                    (title_height > 0.0f && content_height > 0.0f ? kEditorHoverPopupSectionGap
                                                                   : 0.0f) +
                    content_height;
  }
  const float card_height = kEditorHoverPopupPadding * 2.0f + line_height +
                            (message_lines.empty() ? 0.0f
                                                   : kEditorHoverPopupSectionGap + message_height) +
                            symbol_height +
                            (offer_quick_fix ? kEditorHoverPopupSectionGap + button_height : 0.0f);
  const SDL_FRect card_rect = PositionHoverPopup(active_editor_hover_target_->anchor_rect,
                                                 card_width, card_height, layout.editor_surface);
  std::optional<SDL_FRect> action_rect;
  if (offer_quick_fix) {
    action_rect = MakeRect(card_rect.x + kEditorHoverPopupPadding,
                           card_rect.y + card_rect.h - button_height - kEditorHoverPopupPadding,
                           quick_fix_width, button_height);
  }
  return EditorHoverPopupLayout{
      .kind = EditorHoverTarget::Kind::Diagnostic,
      .anchor_rect = active_editor_hover_target_->anchor_rect,
      .rect = card_rect,
      .blame_line_index = 0,
      .diagnostic = diagnostic,
      .plugin_hover = symbol,
      .primary_action_rect = action_rect,
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
  struct WrapMemoEntry {
    std::string text;
    float max_width = 0.0f;
    std::size_t max_lines = 0;
    std::size_t metrics_generation = 0;
    std::vector<std::string> lines;
    bool valid = false;
  };
  static thread_local std::array<WrapMemoEntry, 4> memo;
  static thread_local std::size_t memo_next = 0;

  std::vector<std::string> lines;
  if (max_lines == 0 || max_width <= 0.0f) {
    return lines;
  }

  const std::string_view key_text = text;
  const std::size_t metrics_generation = text_renderer_.MetricsGeneration();
  for (const WrapMemoEntry& entry : memo) {
    if (entry.valid && entry.max_width == max_width && entry.max_lines == max_lines &&
        entry.metrics_generation == metrics_generation && entry.text == key_text) {
      return entry.lines;
    }
  }

  const auto remember = [&](std::vector<std::string> wrapped) {
    WrapMemoEntry& slot = memo[memo_next];
    memo_next = (memo_next + 1) % memo.size();
    slot.text.assign(key_text);
    slot.max_width = max_width;
    slot.max_lines = max_lines;
    slot.metrics_generation = metrics_generation;
    slot.lines = std::move(wrapped);
    slot.valid = true;
    return slot.lines;
  };

  if (text.size() > kMaxHoverTextBytes) {
    text = text.substr(0, util::PreviousUtf8Boundary(text, kMaxHoverTextBytes));
  }

  return remember(WrapHoverText(
      text, max_width, max_lines,
      HoverTextMeasure{
          .width = [this](std::string_view t) { return text_renderer_.MeasureWidth(t); },
          .truncate =
              [this](std::string_view t, float width) {
                return std::string(text_renderer_.TruncateToWidthEphemeralView(t, width));
              },
      }));
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

void WorkspaceShell::LayOutTwoBlockHoverCard(CardPaint& card,
                                             const SDL_FRect& area,
                                             std::string_view title,
                                             std::string_view content,
                                             std::size_t max_title_lines,
                                             std::size_t max_content_lines) const {
  float text_y = area.y;
  const float text_width = std::max(0.0f, area.w);
  const auto title_lines = title.empty()
                               ? std::vector<std::string>{}
                               : WrapEditorHoverPopupText(title, text_width, max_title_lines);
  const auto content_lines = content.empty()
                                 ? std::vector<std::string>{}
                                 : WrapEditorHoverPopupText(content, text_width, max_content_lines);
  card.Lines(text_renderer_, area.x, &text_y, theme_.text_secondary, title_lines, kEditorHoverPopupLineGap);
  if (!title_lines.empty() && !content_lines.empty()) {
    text_y += kEditorHoverPopupSectionGap;
  }
  card.Lines(text_renderer_, area.x, &text_y, theme_.text_primary, content_lines, kEditorHoverPopupLineGap);
}

void WorkspaceShell::LayOutEditorHoverCard(const EditorHoverPopupLayout& popup, CardPaint& card) const {
  card.Open(popup.rect, CardStyle::Overlay);
  const float text_x = popup.rect.x + kEditorHoverPopupPadding;
  const float text_width = std::max(0.0f, popup.rect.w - kEditorHoverPopupPadding * 2.0f);
  float text_y = popup.rect.y + kEditorHoverPopupPadding;

  if (popup.kind == EditorHoverTarget::Kind::Blame) {
    const editor::EditorBlameLine* blame_line = editor_blame_overlay_service_.VisibleLine(popup.blame_line_index);
    if (blame_line != nullptr) {
      const auto summary_lines =
          WrapEditorHoverPopupText(blame_line->summary, text_width, kEditorHoverPopupMaxSummaryLines);
      card.Text(text_x, text_y, theme_.text_primary,
                text_renderer_.TruncateToWidthEphemeralView(blame_line->author, text_width));
      text_y += text_renderer_.LineHeight() + kEditorHoverPopupLineGap;
      card.Text(text_x, text_y, theme_.text_secondary,
                text_renderer_.TruncateToWidthEphemeralView(blame_line->date, text_width));
      if (!summary_lines.empty()) {
        text_y += text_renderer_.LineHeight() + kEditorHoverPopupSectionGap;
        card.Lines(text_renderer_, text_x, &text_y, theme_.text_primary, summary_lines, kEditorHoverPopupLineGap);
      }
    }
  } else if (popup.kind == EditorHoverTarget::Kind::Plugin && popup.plugin_hover.has_value()) {
    LayOutTwoBlockHoverCard(card, MakeRect(text_x, text_y, text_width, 0.0f), popup.plugin_hover->title,
                            popup.plugin_hover->content, kEditorHoverPopupMaxPluginTitleLines,
                            kEditorHoverPopupMaxPluginContentLines);
  } else if (popup.kind == EditorHoverTarget::Kind::DebugValue && popup.debug_value.has_value()) {
    LayOutTwoBlockHoverCard(card, MakeRect(text_x, text_y, text_width, 0.0f), popup.debug_value->type,
                            popup.debug_value->value, kEditorHoverPopupMaxPluginTitleLines,
                            kEditorHoverPopupMaxPluginContentLines);
  } else if (popup.diagnostic.has_value()) {
    const SDL_Color severity_color = editor::DiagnosticSeverityColor(theme_, popup.diagnostic->severity);
    const auto message_lines =
        WrapEditorHoverPopupText(popup.diagnostic->message, text_width, kEditorHoverPopupMaxDiagnosticLines);
    card.Text(text_x, text_y, severity_color,
              text_renderer_.TruncateToWidthEphemeralView(DiagnosticSeverityLabel(popup.diagnostic->severity),
                                                          text_width));
    if (!message_lines.empty()) {
      text_y += text_renderer_.LineHeight() + kEditorHoverPopupSectionGap;
      card.Lines(text_renderer_, text_x, &text_y, theme_.text_primary, message_lines, kEditorHoverPopupLineGap);
    } else {
      text_y += text_renderer_.LineHeight();
    }
    if (popup.plugin_hover.has_value()) {
      card.Fill(MakeRect(popup.rect.x + 1.0f, std::floor(text_y + kEditorHoverPopupSectionGap * 0.5f),
                         popup.rect.w - 2.0f, 1.0f),
                theme_.border);
      text_y += kEditorHoverPopupSectionGap;
      LayOutTwoBlockHoverCard(card, MakeRect(text_x, text_y, text_width, 0.0f), popup.plugin_hover->title,
                              popup.plugin_hover->content, kEditorHoverPopupMaxPluginTitleLines,
                              kEditorHoverPopupMaxPluginContentLines);
    }
  }

  if (popup.primary_action_rect.has_value()) {
    card.button = CardButtonPaint{
        .rect = *popup.primary_action_rect,
        .label = popup.kind == EditorHoverTarget::Kind::Diagnostic ? "Quick Fix" : "Copy SHA",
        .hovered = last_mouse_position_valid_ && EditorHoverPopupPrimaryActionHovered(last_mouse_x_, last_mouse_y_),
    };
    if (popup.kind == EditorHoverTarget::Kind::Diagnostic) {
      const float hint_x = popup.primary_action_rect->x + popup.primary_action_rect->w + kEditorHoverPopupGap;
      const float hint_y =
          popup.primary_action_rect->y + (popup.primary_action_rect->h - text_renderer_.LineHeight()) * 0.5f;
      card.Text(hint_x, hint_y, theme_.text_secondary, kQuickFixShortcutHint);
    }
  }
}

void WorkspaceShell::UpdateEditorHover(float x, float y) {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::UpdateEditorHover");
  if (MenuSurfaceCapturingMouse()) {
    active_editor_hover_target_.reset();
    editor_hover_refresh_pending_ = false;
    return;
  }
  if (InteractionState::CaretHover& pin = context_.interaction_state.caret_hover; pin.active) {
    const editor::TextViewport* viewport = ActiveEditableViewport();
    const bool pointer_moved = last_mouse_position_valid_ &&
                               (!pin.pointer_valid || x != pin.pointer_x || y != pin.pointer_y);
    std::optional<SDL_FPoint> caret;
    if (viewport != nullptr && viewport == pin.viewport && viewport->cursor_line() == pin.line &&
        viewport->cursor_column() == pin.column && !pointer_moved) {
      if (const std::optional<WorkspaceLayout> layout = CurrentWorkspaceLayout(); layout.has_value()) {
        for (const EditorPaneLayout& pane : ComputeEditorPaneLayouts(*layout)) {
          if (!pane.active) {
            continue;
          }
          const TextGridInteractionLayout grid =
              BuildEditorInteractionGrid(editor_text_renderer_, *viewport, pane.rect, LineNumbersEnabled());
          const std::size_t row = viewport->VisualRowForLine(pin.line);
          const std::size_t column = viewport->VisualColumnAt(pin.line, pin.column);
          if (row >= grid.scroll_line && row < grid.scroll_line + grid.visible_rows &&
              column >= grid.horizontal_scroll) {
            caret = SDL_FPoint{
                grid.text_x + (static_cast<float>(column - grid.horizontal_scroll) + 0.5f) * grid.char_width,
                grid.first_line_y + (static_cast<float>(row - grid.scroll_line) + 0.5f) * grid.line_height};
          }
          break;
        }
      }
    }
    if (caret.has_value()) {
      x = caret->x;
      y = caret->y;
    } else {
      pin.active = false;
    }
  }
  const std::optional<EditorHoverTarget> previous_target = active_editor_hover_target_;
  pending_hover_eval_.valid = false;
  auto target = [&]() -> std::optional<EditorHoverTarget> {
    util::PerformanceTrace::Scope scope("WorkspaceShell::UpdateEditorHover::TargetAtPosition");
    return EditorHoverTargetAtPosition(x, y);
  }();
  if (pending_hover_eval_.valid) {
    debug_service_.EvaluateHover(pending_hover_eval_.frame_id, pending_hover_eval_.expression);
    pending_hover_eval_.valid = false;
  }
  if (plugin_hover_cache_.state == PluginHoverCache::State::Kickoff) {
    KickOffPluginHover(plugin_hover_cache_.path, plugin_hover_cache_.line,
                       plugin_hover_cache_.column);
    if (plugin_hover_cache_.state != PluginHoverCache::State::Pending) {
      target = EditorHoverTargetAtPosition(x, y);
    }
  }
  if (target.has_value()) {
    const bool same_card = previous_target.has_value() &&
                           previous_target->kind == target->kind &&
                           previous_target->diagnostic == target->diagnostic &&
                           RectsEqual(previous_target->anchor_rect, target->anchor_rect);
    if (previous_target.has_value() && *previous_target != *target && !same_card &&
        previous_target->kind == EditorHoverTarget::Kind::Diagnostic) {
      if (const auto sticky = ActiveEditorHoverPopupLayout();
          sticky.has_value() && sticky->primary_action_rect.has_value() &&
          (Contains(sticky->rect, x, y) ||
           Contains(HoverPopupHoverZoneRect(sticky->anchor_rect, sticky->rect), x, y))) {
        return;
      }
    }
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
    if (plugin_hover_cache_.state == PluginHoverCache::State::Resolved ||
        plugin_hover_cache_.state == PluginHoverCache::State::Failed) {
      plugin_hover_cache_.state = PluginHoverCache::State::Empty;
      ++plugin_hover_cache_.generation;
    }
  }
  active_editor_hover_target_.reset();
}

void WorkspaceShell::KickOffPluginHover(const std::filesystem::path& path, std::size_t line,
                                        std::size_t column) {
  plugin_hover_cache_.state = PluginHoverCache::State::Pending;
  const std::uint64_t generation = plugin_hover_cache_.generation;
  plugin_runtime_.Host().QueryHoverAsync(
      path, line, column,
      [this, generation](bool ok, plugin::PluginHost::HoverResult result) {
        if (plugin_hover_cache_.state != PluginHoverCache::State::Pending ||
            plugin_hover_cache_.generation != generation) {
          return;
        }
        if (ok) {
          TruncateHoverText(result.title);
          TruncateHoverText(result.content);
          plugin_hover_cache_.state = PluginHoverCache::State::Resolved;
          plugin_hover_cache_.result = std::move(result);
          QueueEditorHoverRefresh();
          RequestPreparedFrame();
          return;
        }
        KickOffLspHover(plugin_hover_cache_.path, plugin_hover_cache_.line,
                        plugin_hover_cache_.column);
      });
}

void WorkspaceShell::KickOffLspHover(const std::filesystem::path& path, std::size_t line,
                                     std::size_t column) {
  const auto fail = [&]() {
    plugin_hover_cache_.state = PluginHoverCache::State::Failed;
    plugin_hover_cache_.failed_at_ms = SDL_GetTicks();
    QueueEditorHoverRefresh();
    RequestPreparedFrame();
  };
  const auto get_setting = [this](std::string_view id) { return GetSettingValue(id); };
  if (!LspFeatureEnabled(get_setting, "lsp.hover.enabled")) {
    fail();
    return;
  }
  editor::TextViewport* viewport = ActiveEditorViewport();
  if (viewport == nullptr || viewport->path() != path) {
    fail();
    return;
  }
  std::string language_id;
  LspClient* client = LspClientForViewport(*viewport, &language_id);
  if (client == nullptr) {
    fail();
    return;
  }
  EnsureLspDocumentOpen(*viewport, *client, language_id);
  const std::size_t line0 = line > 0 ? line - 1 : 0;
  const std::size_t byte_column = column > 0 ? column - 1 : 0;
  const lsp_encoding::PositionEncoding encoding = LspEncodingForClient(*client);
  BeginTrackedLspRequest();
  const std::uint64_t generation = plugin_hover_cache_.generation;
  client->RequestHoverAsync(
      FileUriForPath(path), ByteColumnToLspPosition(*viewport, line0, byte_column, encoding),
      [this, generation](LspResult<util::JsonValue> result) {
        FinishTrackedLspRequest();
        if (plugin_hover_cache_.state != PluginHoverCache::State::Pending ||
            plugin_hover_cache_.generation != generation) {
          return;
        }
        std::string content =
            result.has_value() ? lsp_protocol::ParseHoverContents(*result) : std::string{};
        if (content.empty()) {
          plugin_hover_cache_.state = PluginHoverCache::State::Failed;
          plugin_hover_cache_.failed_at_ms = SDL_GetTicks();
        } else {
          plugin::PluginHost::HoverResult hover;
          hover.content = std::move(content);
          TruncateHoverText(hover.content);
          plugin_hover_cache_.result = std::move(hover);
          plugin_hover_cache_.state = PluginHoverCache::State::Resolved;
        }
        QueueEditorHoverRefresh();
        RequestPreparedFrame();
      });
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

void WorkspaceShell::ShowSignatureHelpPopup(std::string signature, std::string documentation) {
  const editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty() || signature.empty()) {
    if (active_signature_help_.has_value()) {
      active_signature_help_.reset();
      RequestPreparedFrame();
    }
    return;
  }
  active_signature_help_ = SignatureHelpPopup{
      .signature = std::move(signature),
      .documentation = std::move(documentation),
      .path = viewport->path(),
      .cursor_line = viewport->cursor_line(),
      .cursor_column = viewport->cursor_column(),
  };
  RequestPreparedFrame();
}

void WorkspaceShell::MaybeExpireSignatureHelp() {
  if (!active_signature_help_.has_value()) {
    return;
  }
  const editor::TextViewport* viewport = ActiveEditableViewport();
  const bool still_valid = viewport != nullptr &&
                           viewport->path() == active_signature_help_->path &&
                           viewport->cursor_line() == active_signature_help_->cursor_line &&
                           viewport->cursor_column() == active_signature_help_->cursor_column;
  if (!still_valid) {
    active_signature_help_.reset();
    RequestPreparedFrame();
  }
}

void WorkspaceShell::LayOutSignatureHelpCard(const SignatureHelpPopup& popup, const SDL_FRect& rect,
                                             CardPaint& card) const {
  card.Open(rect, CardStyle::Overlay);
  const float text_x = rect.x + kEditorHoverPopupPadding;
  const float text_width = std::max(0.0f, rect.w - kEditorHoverPopupPadding * 2.0f);
  float text_y = rect.y + kEditorHoverPopupPadding;
  const auto signature_lines =
      WrapEditorHoverPopupText(popup.signature, text_width, kEditorHoverPopupMaxPluginTitleLines);
  const auto doc_lines = popup.documentation.empty()
                             ? std::vector<std::string>{}
                             : WrapEditorHoverPopupText(popup.documentation, text_width,
                                                        kEditorHoverPopupMaxPluginContentLines);
  card.Lines(text_renderer_, text_x, &text_y, theme_.text_primary, signature_lines, kEditorHoverPopupLineGap);
  if (!signature_lines.empty() && !doc_lines.empty()) {
    text_y += kEditorHoverPopupSectionGap;
  }
  card.Lines(text_renderer_, text_x, &text_y, theme_.text_secondary, doc_lines, kEditorHoverPopupLineGap);
}

}
