#pragma once

#include <cstddef>

#include <SDL3/SDL.h>

namespace microide::workspace {

inline constexpr int kCommitWorkflowBodyRows = 4;

inline constexpr float kCommitWorkflowFieldInset = 10.0f;

inline constexpr float kCommitWorkflowCheckLineHeight = 14.0f;

struct CommitWorkflowLayout {
  float staged_summary_y = 0.0f;
  float subject_label_y = 0.0f;
  SDL_FRect subject_field{};
  float body_label_y = 0.0f;
  SDL_FRect body_field{};
  SDL_FRect commit_button{};
  float checks_y = 0.0f;
  float status_y = 0.0f;
  float total_height = 0.0f;
};

inline CommitWorkflowLayout ComputeCommitWorkflowLayout(float top, float field_x, float field_w,
                                                        float line_height, int body_rows,
                                                        std::size_t check_count, bool has_status) {
  constexpr float kStagedToLabelGap = 12.0f;
  constexpr float kLabelToFieldGap = 5.0f;
  constexpr float kFieldToLabelGap = 12.0f;
  constexpr float kFieldVerticalPadding = 8.0f;
  constexpr float kButtonGap = 12.0f;
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

  y += kButtonGap;
  layout.commit_button = SDL_FRect{field_x, y, field_w, kButtonHeight};
  y += kButtonHeight + kButtonGap;

  layout.checks_y = y;
  y += static_cast<float>(check_count) * kCommitWorkflowCheckLineHeight;
  layout.status_y = y;
  if (has_status) {
    y += kCommitWorkflowCheckLineHeight;
  }

  layout.total_height = y - top;
  return layout;
}

inline int FitCommitWorkflowBodyRows(float top, float bottom, float line_height,
                                     std::size_t check_count, bool has_status) {
  for (int rows = kCommitWorkflowBodyRows; rows > 1; --rows) {
    const SDL_FRect button =
        ComputeCommitWorkflowLayout(top, 0.0f, 0.0f, line_height, rows, check_count, has_status)
            .commit_button;
    if (button.y + button.h <= bottom) {
      return rows;
    }
  }
  return 1;
}

}
