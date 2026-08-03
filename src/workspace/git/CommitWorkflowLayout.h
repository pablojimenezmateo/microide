#pragma once

#include <cstddef>

#include <SDL3/SDL.h>

namespace microide::workspace {

// Visible body-field rows of the inline commit draft. Shared by the sidebar
// render path, GitSidebarCommitWorkflowHeight, and the frame-prep viewport
// sizing (PrepareCommitBodyViewportForFrame) so they can never drift apart.
inline constexpr int kCommitWorkflowBodyRows = 4;

// Horizontal inset of the commit draft fields inside the sidebar (matches the
// sidebar's kSidebarInset). Used by frame prep to derive the body field width
// without the full panel layout.
inline constexpr float kCommitWorkflowFieldInset = 10.0f;

// Pure geometry of the inline commit draft panel: the staged-summary header, the framed
// subject/body fields, the pre-check/status lines, and the confirm button. Shared by the
// sidebar render path (which draws and caches the rects) and GitSidebarCommitWorkflowHeight
// (which reserves exactly this much vertical space). Computing both from one helper keeps the
// file list flush below the panel instead of leaving the old fixed-height dead gap before the
// Conflicts section.
struct CommitWorkflowLayout {
  float staged_summary_y = 0.0f;
  float subject_label_y = 0.0f;
  SDL_FRect subject_field{};
  float body_label_y = 0.0f;
  SDL_FRect body_field{};
  float checks_y = 0.0f;  // baseline of the first pre-check line
  float status_y = 0.0f;  // baseline of the status line (valid when has_status)
  SDL_FRect commit_button{};
  float total_height = 0.0f;  // top..bottom extent of the panel content
};

inline CommitWorkflowLayout ComputeCommitWorkflowLayout(float top, float field_x, float field_w,
                                                        float line_height, int body_rows,
                                                        std::size_t check_count, bool has_status) {
  // Tuned spacing: the staged-summary header gets clear separation from the subject field,
  // each label sits just above its own field, and the body field has breathing room before
  // the pre-checks/confirm button without the previous oversized reserved gap.
  constexpr float kStagedToLabelGap = 12.0f;    // staged summary -> Subject label
  constexpr float kLabelToFieldGap = 5.0f;      // label -> framed field top
  constexpr float kFieldToLabelGap = 12.0f;     // framed field bottom -> next label
  constexpr float kFieldVerticalPadding = 8.0f; // extra height inside framed fields
  constexpr float kCheckLineHeight = 14.0f;
  constexpr float kButtonGap = 12.0f;  // last content line -> confirm button
  constexpr float kButtonHeight = 26.0f;

  CommitWorkflowLayout layout;
  float y = top;

  layout.staged_summary_y = y;
  y += line_height + kStagedToLabelGap;

  layout.subject_label_y = y;
  y += line_height + kLabelToFieldGap;
  layout.subject_field = SDL_FRect{field_x, y, field_w, line_height + kFieldVerticalPadding};
  y += layout.subject_field.h + kFieldToLabelGap;

  layout.body_label_y = y;
  y += line_height + kLabelToFieldGap;
  layout.body_field = SDL_FRect{field_x, y, field_w,
                                line_height * static_cast<float>(body_rows) + kFieldVerticalPadding};
  y += layout.body_field.h + kFieldToLabelGap;

  layout.checks_y = y;
  y += static_cast<float>(check_count) * kCheckLineHeight;
  layout.status_y = y;
  if (has_status) {
    y += kCheckLineHeight;
  }

  y += kButtonGap;
  layout.commit_button = SDL_FRect{field_x, y, field_w, kButtonHeight};
  y += kButtonHeight;

  layout.total_height = y - top;
  return layout;
}

}  // namespace microide::workspace
