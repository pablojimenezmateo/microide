#include "workspace/ProjectSearchPanelLayout.h"

#include <algorithm>
#include <cmath>

namespace microide::workspace::project_search_panel {

namespace {

constexpr float kButtonGap = 4.0f;
// Width shares of the toggle row. The scope "..." button takes whatever the three
// labelled buttons leave, so the row always fills the sidebar exactly.
constexpr float kModeShare = 0.24f;
constexpr float kCaseShare = 0.30f;
constexpr float kHiddenShare = 0.28f;

SDL_FRect MakeRect(float x, float y, float w, float h) { return SDL_FRect{x, y, w, h}; }

SDL_FRect FieldRect(const SDL_FRect& sidebar_rect, float top) {
  return MakeRect(sidebar_rect.x + kInset, sidebar_rect.y + top,
                  std::max(0.0f, sidebar_rect.w - kInset * 2.0f), kFieldHeight);
}

float ButtonRowWidth(const SDL_FRect& sidebar_rect) {
  return std::max(0.0f, sidebar_rect.w - kInset * 2.0f - kButtonGap * 3.0f);
}

}  // namespace

SDL_FRect QueryRect(const SDL_FRect& sidebar_rect) { return FieldRect(sidebar_rect, kQueryTop); }

SDL_FRect ReplaceRect(const SDL_FRect& sidebar_rect) {
  return FieldRect(sidebar_rect, kReplaceTop);
}

SDL_FRect IncludeRect(const SDL_FRect& sidebar_rect, bool scope_expanded) {
  return scope_expanded ? FieldRect(sidebar_rect, kIncludeTop) : SDL_FRect{0.0f, 0.0f, 0.0f, 0.0f};
}

SDL_FRect ExcludeRect(const SDL_FRect& sidebar_rect, bool scope_expanded) {
  return scope_expanded ? FieldRect(sidebar_rect, kExcludeTop) : SDL_FRect{0.0f, 0.0f, 0.0f, 0.0f};
}

SDL_FRect ModeButtonRect(const SDL_FRect& sidebar_rect) {
  const float available = ButtonRowWidth(sidebar_rect);
  return MakeRect(sidebar_rect.x + kInset, sidebar_rect.y + kButtonTop,
                  std::floor(available * kModeShare), kButtonHeight);
}

SDL_FRect CaseButtonRect(const SDL_FRect& sidebar_rect) {
  const float available = ButtonRowWidth(sidebar_rect);
  const SDL_FRect mode_rect = ModeButtonRect(sidebar_rect);
  return MakeRect(mode_rect.x + mode_rect.w + kButtonGap, mode_rect.y,
                  std::floor(available * kCaseShare), kButtonHeight);
}

SDL_FRect HiddenButtonRect(const SDL_FRect& sidebar_rect) {
  const float available = ButtonRowWidth(sidebar_rect);
  const SDL_FRect case_rect = CaseButtonRect(sidebar_rect);
  return MakeRect(case_rect.x + case_rect.w + kButtonGap, case_rect.y,
                  std::floor(available * kHiddenShare), kButtonHeight);
}

SDL_FRect ScopeButtonRect(const SDL_FRect& sidebar_rect) {
  const float available = ButtonRowWidth(sidebar_rect);
  const SDL_FRect hidden_rect = HiddenButtonRect(sidebar_rect);
  const float used = std::floor(available * kModeShare) + std::floor(available * kCaseShare) +
                     std::floor(available * kHiddenShare);
  return MakeRect(hidden_rect.x + hidden_rect.w + kButtonGap, hidden_rect.y,
                  std::max(0.0f, available - used), kButtonHeight);
}

}  // namespace microide::workspace::project_search_panel
