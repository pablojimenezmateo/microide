#include "workspace/render/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string_view>

#include "render/DevicePixelSnap.h"
#include "render/PaintElement.h"
#include "workspace/git/CommitWorkflowLayout.h"
#include "workspace/git/GitSidebarHeaderLayout.h"
#include "workspace/render/GitSidebarPaint.h"
#include "workspace/render/SingleLineViewMetrics.h"

// The Source Control panel's head. Prep lays it out into the frame's snapshot
// (PrepareGitPanelHeadFrame) and derives its damage from it (openspec
// derived-damage 4.1); paint draws the snapshot only, each element in its own
// PaintElement (clip-discipline 4.1). The commit draft's control rects and caret
// are laid out here, in prep, and published to its hit-testing by the caller:
// paint used to write them, so a frame that skipped the sidebar left them stale.

namespace microide::workspace {

using namespace detail;

namespace {

constexpr float kInset = kCommitWorkflowFieldInset;

// A unit's rect: the element's, snapped as its PaintElement clips (clip-discipline D2).
SDL_FRect UnitRect(SDL_Renderer* renderer, const SDL_FRect& rect) {
  const SDL_Rect snapped = render::SnapToDevicePixels(renderer, rect);
  return SDL_FRect{static_cast<float>(snapped.x), static_cast<float>(snapped.y), static_cast<float>(snapped.w),
                   static_cast<float>(snapped.h)};
}

SDL_Color InkColor(const render::Theme& theme, GitPanelInk ink) {
  switch (ink) {
    case GitPanelInk::Secondary:
      return theme.text_secondary;
    case GitPanelInk::Primary:
      return theme.text_primary;
    case GitPanelInk::Error:
      return theme.diff_deleted;
    case GitPanelInk::Muted:
      break;
  }
  return theme.text_muted;
}

// Lays the commit draft out under `top`: its staged summary, the Subject and Body
// fields, the confirm button and the pre-check and status lines the sidebar has
// room for. A control with no room is left out, its rect empty, so no click,
// cursor or control-channel query finds it (soak c8c7389e178d).
void LayOutCommitDraft(PreparedFrameState::GitPanelHeadPaint& head, const GitPanelHeadInputs& inputs, float top) {
  const CommitWorkflowState& workflow = *inputs.workflow;
  const render::TextRenderer& tr = *inputs.text_renderer;
  const float field_x = inputs.sidebar.x + kInset;
  const float field_w = inputs.sidebar.w - kInset * 2.0f;
  const float line_height = tr.LineHeight();
  const float bottom = inputs.sidebar.y + inputs.sidebar.h;
  const bool subject_focused = inputs.panel_focused && workflow.focus_field == CommitWorkflowFocusField::Subject;
  const bool body_focused = inputs.panel_focused && workflow.focus_field == CommitWorkflowFocusField::Body;
  // As many rows as keep the Commit button inside the sidebar (the same fit
  // GitSidebarCommitWorkflowHeight and PrepareCommitBodyViewportForFrame make).
  const int body_rows =
      FitCommitWorkflowBodyRows(top, bottom, line_height, workflow.checks.size(), !workflow.status_message.empty());
  const CommitWorkflowLayout panel = ComputeCommitWorkflowLayout(
      top, field_x, field_w, line_height, body_rows, workflow.checks.size(), !workflow.status_message.empty());
  head.body_visible_rows = body_rows;
  const auto fits = [bottom](const SDL_FRect& rect) { return rect.y + rect.h <= bottom; };
  // A text line is its line box: one the sidebar cuts is left out, not drawn past it.
  const auto line = [&](float x, float y, float width, std::string_view text, GitPanelInk ink) {
    const SDL_FRect rect = MakeRect(x, y, std::max(0.0f, width), line_height);
    if (fits(rect)) {
      head.lines.push_back(GitPanelTextPaint{.rect = rect, .text = text, .ink = ink});
      return true;
    }
    return false;
  };

  line(field_x, panel.staged_summary_y, field_w, workflow.staged_summary_line, GitPanelInk::Primary);

  // Subject: a framed single-line input, scrolled so its caret shows.
  if (fits(panel.subject_field)) {
    line(field_x, panel.subject_label_y, field_w, "Subject:", GitPanelInk::Muted);
    GitPanelFieldPaint& field = head.subject;
    field.frame = panel.subject_field;
    field.focused = subject_focused;
    field.text_x = field.frame.x + 6.0f;
    field.text_width = std::max(1.0f, field.frame.w - 12.0f);
    const float text_y = field.frame.y + std::floor((field.frame.h - line_height) * 0.5f);
    if (workflow.subject.text().empty() && !subject_focused) {
      field.placeholder = "<required>";
      field.placeholder_y = text_y;
    } else {
      SingleLineViewMetrics metrics = ComputeSingleLineViewMetrics(tr, workflow.subject, "", field.text_width);
      head.subject_text.swap(metrics.displayed_text);
      const std::string_view shown = head.subject_text;
      SDL_FRect selection{};
      if (metrics.selection_bytes.has_value()) {
        const auto [start, end] = *metrics.selection_bytes;
        if (end <= shown.size() && start < end) {
          selection = MakeRect(field.text_x + tr.MeasureWidth(shown.substr(0, start)), field.frame.y + 2.0f,
                               tr.MeasureWidth(shown.substr(start, end - start)), field.frame.h - 4.0f);
        }
      }
      field.lines.push_back(GitPanelFieldLine{.y = text_y, .text = shown, .selection = selection});
      if (subject_focused) {
        head.caret = MakeRect(field.text_x + metrics.cursor_x, field.frame.y + 2.0f, 1.0f, field.frame.h - 4.0f);
      }
    }
  }

  // Body: a framed multi-line edit area. Its viewport was sized and scrolled to
  // the caret earlier in prep (PrepareCommitBodyViewportForFrame); only the
  // visible rows are read, through zero-copy line views (TD-2026-07-17-083).
  if (fits(panel.body_field)) {
    line(field_x, panel.body_label_y, field_w, "Body:", GitPanelInk::Muted);
    GitPanelFieldPaint& field = head.body;
    field.frame = panel.body_field;
    field.focused = body_focused;
    field.text_x = field.frame.x + 6.0f;
    field.text_width = std::max(1.0f, field.frame.w - 12.0f);
    const float top_y = field.frame.y + 3.0f;
    const editor::TextViewport& body = workflow.body;
    const editor::TextBuffer& body_lines = body.lines();
    const std::size_t line_count = body_lines.size();
    if (line_count == 1 && body_lines.LineView(0).empty() && !body_focused) {
      field.placeholder = "<optional>";
      field.placeholder_y = top_y;
    } else {
      const std::size_t scroll = body.scroll_line();
      const std::optional<editor::SelectionRange> selection = body.selection_range();
      for (int row = 0; row < body_rows; ++row) {
        const std::size_t index = scroll + static_cast<std::size_t>(row);
        if (index >= line_count) {
          break;
        }
        const std::string_view text = body_lines.LineView(index);
        const float row_y = top_y + static_cast<float>(row) * line_height;
        SDL_FRect selected{};
        if (selection.has_value() && index >= selection->start.line && index <= selection->end.line) {
          const std::size_t from = index == selection->start.line ? selection->start.column : 0;
          const std::size_t to =
              index == selection->end.line ? std::min(selection->end.column, text.size()) : text.size();
          if (from < to && from <= text.size()) {
            const float width = tr.MeasureWidth(text.substr(from, to - from));
            if (width > 0.0f) {
              selected = MakeRect(field.text_x + tr.MeasureWidth(text.substr(0, from)), row_y - 1.0f, width,
                                  line_height);
            }
          }
        }
        field.lines.push_back(GitPanelFieldLine{.y = row_y, .text = text, .selection = selected});
        if (body_focused && index == body.cursor_line()) {
          const std::size_t column = std::min(body.cursor_column(), text.size());
          const float caret_x =
              std::min(field.text_x + tr.MeasureWidth(text.substr(0, column)), field.frame.x + field.frame.w - 2.0f);
          head.caret = MakeRect(caret_x, row_y - 1.0f, 1.0f, line_height);
        }
      }
    }
  }

  // The confirm button: the discoverable way to commit the staged changes (the
  // Ctrl+Enter shortcut's twin), enabled only when the draft can be committed.
  if (fits(panel.commit_button)) {
    head.commit_button = panel.commit_button;
    head.buttons.push_back(GitPanelButtonPaint{.rect = panel.commit_button,
                                               .label = "Commit",
                                               .tone = GitPanelTone::Accent,
                                               .enabled = inputs.commit_can_execute,
                                               .hovered = inputs.commit_can_execute &&
                                                          inputs.pointer_over(panel.commit_button)});
  }

  // The pre-check and status lines below the button: those the sidebar has room for.
  float check_y = panel.checks_y;
  for (const project::CommitPreCheck& check : workflow.checks) {
    const GitPanelInk ink = check.severity == project::CommitPreCheckSeverity::Blocking ? GitPanelInk::Primary
                                                                                         : GitPanelInk::Muted;
    if (!line(field_x, check_y, field_w, check.message, ink)) {
      break;
    }
    check_y += kCommitWorkflowCheckLineHeight;
  }
  if (!workflow.status_message.empty()) {
    line(field_x, panel.status_y, field_w, workflow.status_message, GitPanelInk::Muted);
  }
}

void PaintField(SDL_Renderer* renderer, const GitPanelFieldPaint& field, const render::Theme& theme,
                const render::TextRenderer& tr, const char* name) {
  if (field.frame.w <= 0.0f || field.frame.h <= 0.0f) {
    return;
  }
  const render::PaintElement element(renderer, field.frame, name);
  DrawTextFieldFrame(renderer, theme, field.frame, field.focused);
  if (!field.placeholder.empty()) {
    DrawTextOn(tr, renderer, field.text_x, field.placeholder_y, theme.text_muted, theme.surface_background,
               tr.TruncateToWidthEphemeralView(field.placeholder, field.text_width));
  }
  for (const GitPanelFieldLine& line : field.lines) {
    if (line.selection.w > 0.0f) {
      FillRect(renderer, line.selection, theme.selection_fill);
    }
    DrawTextOn(tr, renderer, field.text_x, line.y, theme.text_primary, theme.surface_background,
               tr.TruncateToWidthEphemeralView(line.text, field.text_width));
  }
}

}  // namespace

void PrepareGitPanelHeadFrame(PreparedFrameState& frame, const GitPanelHeadInputs& inputs) {
  PreparedFrameState::GitPanelHeadPaint& head = frame.git_panel;
  head.visible = false;
  head.presentation.reset();
  head.buttons.clear();
  head.lines.clear();
  head.subject = GitPanelFieldPaint{};
  head.body = GitPanelFieldPaint{};
  head.caret = SDL_FRect{};
  head.caret_on = false;
  head.commit_button = SDL_FRect{};
  head.body_visible_rows = 0;
  std::vector<DerivedDamage::Unit>& units = frame.derived_damage.Begin(DerivedSurface::GitPanelHead);
  if (inputs.visible && inputs.presentation != nullptr) {
    head.visible = true;
    head.presentation = inputs.presentation;
    const GitSidebarViewModel& git = head.presentation->view_model;
    const render::TextRenderer& tr = *inputs.text_renderer;
    const float line_height = tr.LineHeight();
    const SDL_FRect& sidebar = inputs.sidebar;
    const float text_width = sidebar.w - kInset * 2.0f;

    // The action row: Stage All, Discard All, Refresh.
    const auto action = [&](const SDL_FRect& rect, std::string_view label, bool enabled, GitPanelTone tone) {
      head.buttons.push_back(GitPanelButtonPaint{.rect = rect,
                                                 .label = label,
                                                 .tone = tone,
                                                 .enabled = enabled,
                                                 .hovered = enabled && inputs.pointer_over(rect)});
    };
    action(git_sidebar_header::StageAllButtonRect(sidebar), "Stage All", inputs.stage_all_enabled,
           GitPanelTone::Neutral);
    action(git_sidebar_header::DiscardAllButtonRect(sidebar), "Discard All", inputs.discard_all_enabled,
           GitPanelTone::Destructive);
    action(git_sidebar_header::RefreshButtonRect(sidebar), "Refresh", true, GitPanelTone::Neutral);

    // The branch row: the current branch (opens the switch picker) and Sync, which
    // carries the ahead/behind counts, both labels precomposed by the presentation.
    const SDL_FRect branch_rect = git_sidebar_header::BranchButtonRect(sidebar);
    const SDL_FRect sync_rect = git_sidebar_header::SyncButtonRect(sidebar);
    head.buttons.push_back(GitPanelButtonPaint{.rect = branch_rect,
                                               .label = git.branch_button_label,
                                               .hovered = inputs.pointer_over(branch_rect),
                                               .ellipsize = true});
    head.buttons.push_back(GitPanelButtonPaint{.rect = sync_rect,
                                               .label = git.sync_button_label,
                                               .hovered = inputs.pointer_over(sync_rect),
                                               .ellipsize = true});

    // Below the action row: the Commit button (or, while the draft is open, the
    // commit line), then the banners -- a stale snapshot, a refresh error.
    const SDL_FRect action_row = git_sidebar_header::ActionRowRect(sidebar, 1);
    float y = action_row.y + action_row.h + 10.0f;
    // Must match the constants in WorkspaceShellSidebar.cpp so the file list
    // starts exactly below this block.
    constexpr float kSummaryLineHeight = 17.0f;
    constexpr float kCommitButtonHeight = 22.0f;
    constexpr float kCommitButtonGap = 8.0f;
    if (git.show_commit_button) {
      const SDL_FRect commit_rect = git_sidebar_header::CommitButtonRect(sidebar);
      head.buttons.push_back(GitPanelButtonPaint{.rect = commit_rect,
                                                 .label = "Commit",
                                                 .tone = GitPanelTone::Accent,
                                                 .enabled = git.commit_ready,
                                                 .hovered = git.commit_ready && inputs.pointer_over(commit_rect)});
      if (!git.commit_ready && !git.commit_blocked_reason.empty()) {
        // To the right of the button, in the room left there: across the whole
        // sidebar a narrow one painted "nothing staged" past its own edge (QA
        // monkey, seed 4).
        const float reason_x = commit_rect.x + commit_rect.w + 8.0f;
        const float reason_width = std::max(0.0f, sidebar.x + sidebar.w - kInset - reason_x);
        const float reason_y = commit_rect.y + std::floor(std::max(0.0f, commit_rect.h - line_height) * 0.5f);
        head.lines.push_back(GitPanelTextPaint{.rect = MakeRect(reason_x, reason_y, reason_width, line_height),
                                               .text = git.commit_blocked_reason,
                                               .ink = GitPanelInk::Muted});
      }
      y += kCommitButtonHeight + kCommitButtonGap;
    } else if (!git.commit_summary_line.empty()) {
      head.lines.push_back(GitPanelTextPaint{.rect = MakeRect(sidebar.x + kInset, y, text_width, line_height),
                                             .text = git.commit_summary_line,
                                             .ink = GitPanelInk::Muted});
      y += kSummaryLineHeight;
    }
    for (const std::string& banner : git.summary_lines) {
      head.lines.push_back(GitPanelTextPaint{
          .rect = MakeRect(sidebar.x + kInset, y, text_width, line_height),
          .text = banner,
          .ink = banner == git.error_banner ? GitPanelInk::Error : GitPanelInk::Secondary});
      y += kSummaryLineHeight;
    }
    if (inputs.workflow != nullptr) {
      LayOutCommitDraft(head, inputs, y + 4.0f);
      head.caret_on = inputs.caret_on && head.caret.w > 0.0f;
    }

    const render::Theme& theme = *inputs.theme;
    const std::uint64_t style = PaintFingerprint()
                                    .Add(theme.surface_background)
                                    .Add(theme.text_muted)
                                    .Add(theme.text_secondary)
                                    .Add(theme.text_primary)
                                    .Add(theme.diff_deleted)
                                    .Add(theme.selection_fill)
                                    .Add(theme.accent)
                                    .Add(theme.border)
                                    .Add(theme.row_highlight)
                                    .Add(tr.MetricsGeneration())
                                    .value();
    const auto unit = [&](const SDL_FRect& rect, std::uint64_t fingerprint) {
      units.push_back(DerivedDamage::Unit{.rect = UnitRect(inputs.renderer, rect), .fingerprint = fingerprint});
    };
    for (const GitPanelButtonPaint& button : head.buttons) {
      unit(button.rect, FingerprintOf(button, style));
    }
    for (const GitPanelTextPaint& text : head.lines) {
      unit(text.rect, FingerprintOf(text, style));
    }
    unit(head.subject.frame, FingerprintOf(head.subject, style));
    unit(head.body.frame, FingerprintOf(head.body, style));
    unit(head.caret, PaintFingerprint().Add(head.caret).Add(head.caret_on).Add(style).value());
    unit(inputs.list_rect, PaintFingerprint()
                               .Add(inputs.list_rect)
                               .Add(inputs.list_scroll_row)
                               .Add(inputs.list_line_count)
                               .value());
  }
  frame.derived_damage.Commit(DerivedSurface::GitPanelHead, inputs.damage);
}

void PaintGitPanelHead(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                       const render::TextRenderer& text_renderer) {
  const PreparedFrameState::GitPanelHeadPaint& head = frame.git_panel;
  if (!head.visible) {
    return;
  }
  for (const GitPanelButtonPaint& button : head.buttons) {
    if (button.rect.w <= 0.0f || button.rect.h <= 0.0f) {
      continue;
    }
    const render::PaintElement element(renderer, button.rect, "git-button");
    DrawButtonCentered(
        text_renderer, renderer, theme, button.rect,
        button.ellipsize ? text_renderer.TruncateToWidthEphemeralView(button.label, std::max(0.0f, button.rect.w - 8.0f))
                         : button.label,
        button.tone == GitPanelTone::Accent        ? ButtonTone::Accent
        : button.tone == GitPanelTone::Destructive ? ButtonTone::Destructive
                                                   : ButtonTone::Neutral,
        ButtonVisualState{.enabled = button.enabled, .hovered = button.hovered, .active = false});
  }
  for (const GitPanelTextPaint& line : head.lines) {
    if (line.rect.w <= 0.0f || line.text.empty()) {
      continue;
    }
    const render::PaintElement element(renderer, line.rect, "git-line");
    DrawTextOn(text_renderer, renderer, line.rect.x, line.rect.y, InkColor(theme, line.ink), theme.surface_background,
               text_renderer.TruncateToWidthEphemeralView(line.text, line.rect.w));
  }
  PaintField(renderer, head.subject, theme, text_renderer, "git-subject");
  PaintField(renderer, head.body, theme, text_renderer, "git-body");
  if (head.caret_on) {
    const render::PaintElement element(renderer, head.caret, "git-caret");
    FillRect(renderer, head.caret, theme.text_primary);
  }
}

}  // namespace microide::workspace
