#pragma once

#include <SDL3/SDL.h>

#include <cstddef>

namespace microide::workspace::git_sidebar_header {

inline constexpr float kInset = 10.0f;
inline constexpr float kActionRowTop = 34.0f;
inline constexpr float kActionButtonHeight = 18.0f;
inline constexpr float kActionGap = 6.0f;
inline constexpr float kActionRowStride = kActionButtonHeight + kActionGap;
inline constexpr float kCommitButtonHeight = 22.0f;
inline constexpr float kCommitButtonGap = 8.0f;
inline constexpr float kSummaryLineHeight = 17.0f;
inline constexpr float kActionRowsHeight = kActionRowStride + kActionButtonHeight;

SDL_FRect ActionRowRect(const SDL_FRect& sidebar_rect, std::size_t row_index);

SDL_FRect ActionSlotRect(const SDL_FRect& row_rect, std::size_t index, std::size_t count);

SDL_FRect StageAllButtonRect(const SDL_FRect& sidebar_rect);
SDL_FRect DiscardAllButtonRect(const SDL_FRect& sidebar_rect);
SDL_FRect RefreshButtonRect(const SDL_FRect& sidebar_rect);
SDL_FRect BranchButtonRect(const SDL_FRect& sidebar_rect);
SDL_FRect SyncButtonRect(const SDL_FRect& sidebar_rect);

SDL_FRect CommitButtonRect(const SDL_FRect& sidebar_rect);

}
