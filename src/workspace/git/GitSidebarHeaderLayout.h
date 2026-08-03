#pragma once

#include <SDL3/SDL.h>

#include <cstddef>

// Geometry for the git sidebar's header: two rows of action buttons above the
// summary/commit block, and the commit button that starts that block.
//
// Pure functions of the sidebar rect, kept out of WorkspaceShell for the same
// reason the project-search panel's geometry is (see ProjectSearchPanelLayout.h):
// render, hit-testing, cursor shaping, and the sidebar mouse coordinator all need
// the same numbers, and routing each rect through the shell as its own method
// pulled layout arithmetic into an orchestrator that should not own it.
namespace microide::workspace::git_sidebar_header {

inline constexpr float kInset = 10.0f;
inline constexpr float kActionRowTop = 34.0f;
inline constexpr float kActionButtonHeight = 18.0f;
inline constexpr float kActionGap = 6.0f;
// Vertical distance between the two action rows.
inline constexpr float kActionRowStride = kActionButtonHeight + kActionGap;
inline constexpr float kCommitButtonHeight = 22.0f;
// Total height the header's action rows occupy, from kActionRowTop to the bottom
// of the second row. Consumed by GitSidebarListTop.
inline constexpr float kActionRowsHeight = kActionRowStride + kActionButtonHeight;

// Row 0 is the working-tree row (Stage All / Discard All / Refresh); row 1 is the
// branch row (branch name / Sync).
SDL_FRect ActionRowRect(const SDL_FRect& sidebar_rect, std::size_t row_index);

// Slot `index` of `count` equal-width buttons spanning `row_rect`. One definition
// instead of the per-button `(row.w - gap * (count - 1)) / count` each rect used to
// spell for itself, which is how a fourth button in a row hard-coded for three
// would have silently overlapped.
SDL_FRect ActionSlotRect(const SDL_FRect& row_rect, std::size_t index, std::size_t count);

SDL_FRect StageAllButtonRect(const SDL_FRect& sidebar_rect);
SDL_FRect DiscardAllButtonRect(const SDL_FRect& sidebar_rect);
SDL_FRect RefreshButtonRect(const SDL_FRect& sidebar_rect);
// Shows the current branch and opens the switch-branch picker.
SDL_FRect BranchButtonRect(const SDL_FRect& sidebar_rect);
// Pull-then-push, annotated with ahead/behind counts when they are known.
SDL_FRect SyncButtonRect(const SDL_FRect& sidebar_rect);

SDL_FRect CommitButtonRect(const SDL_FRect& sidebar_rect);

}  // namespace microide::workspace::git_sidebar_header
