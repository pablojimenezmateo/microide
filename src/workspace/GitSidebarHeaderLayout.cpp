#include "workspace/GitSidebarHeaderLayout.h"

#include <algorithm>
#include <cmath>

namespace microide::workspace::git_sidebar_header {

namespace {

constexpr SDL_FRect kEmpty{0.0f, 0.0f, 0.0f, 0.0f};

SDL_FRect MakeRect(float x, float y, float w, float h) { return SDL_FRect{x, y, w, h}; }

}  // namespace

SDL_FRect ActionRowRect(const SDL_FRect& sidebar_rect, const std::size_t row_index) {
  if (sidebar_rect.w <= 0.0f || sidebar_rect.h <= 0.0f) {
    return kEmpty;
  }
  return MakeRect(sidebar_rect.x + kInset,
                  sidebar_rect.y + kActionRowTop +
                      static_cast<float>(row_index) * kActionRowStride,
                  std::max(0.0f, sidebar_rect.w - kInset * 2.0f), kActionButtonHeight);
}

SDL_FRect ActionSlotRect(const SDL_FRect& row_rect,
                         const std::size_t index,
                         const std::size_t count) {
  if (row_rect.w <= 0.0f || row_rect.h <= 0.0f || count == 0 || index >= count) {
    return kEmpty;
  }
  const float gaps = kActionGap * static_cast<float>(count - 1);
  const float button_width = std::max(0.0f, (row_rect.w - gaps) / static_cast<float>(count));
  return MakeRect(row_rect.x + (button_width + kActionGap) * static_cast<float>(index), row_rect.y,
                  button_width, row_rect.h);
}

SDL_FRect StageAllButtonRect(const SDL_FRect& sidebar_rect) {
  return ActionSlotRect(ActionRowRect(sidebar_rect, 0), 0, 3);
}

SDL_FRect DiscardAllButtonRect(const SDL_FRect& sidebar_rect) {
  return ActionSlotRect(ActionRowRect(sidebar_rect, 0), 1, 3);
}

SDL_FRect RefreshButtonRect(const SDL_FRect& sidebar_rect) {
  return ActionSlotRect(ActionRowRect(sidebar_rect, 0), 2, 3);
}

SDL_FRect BranchButtonRect(const SDL_FRect& sidebar_rect) {
  // The branch name needs the room, so it takes two thirds of the row and Sync
  // takes the remaining third.
  const SDL_FRect row = ActionRowRect(sidebar_rect, 1);
  const SDL_FRect third = ActionSlotRect(row, 0, 3);
  if (third.w <= 0.0f) {
    return kEmpty;
  }
  return MakeRect(row.x, row.y, third.w * 2.0f + kActionGap, row.h);
}

SDL_FRect SyncButtonRect(const SDL_FRect& sidebar_rect) {
  return ActionSlotRect(ActionRowRect(sidebar_rect, 1), 2, 3);
}

SDL_FRect CommitButtonRect(const SDL_FRect& sidebar_rect) {
  const SDL_FRect last_row = ActionRowRect(sidebar_rect, 1);
  if (last_row.w <= 0.0f || last_row.h <= 0.0f) {
    return kEmpty;
  }
  // First element of the summary block; the render path starts that block at
  // action-rows-bottom + 10, and this must stay in sync with it.
  return MakeRect(last_row.x, last_row.y + last_row.h + 10.0f, std::min(last_row.w, 96.0f),
                  kCommitButtonHeight);
}

}  // namespace microide::workspace::git_sidebar_header
