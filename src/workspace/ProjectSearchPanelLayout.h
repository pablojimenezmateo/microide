#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <string_view>

#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/state/WorkspaceTextInputState.h"

namespace microide::workspace::project_search_panel {

inline constexpr std::string_view kModeButtonLabel = ".*";
inline constexpr std::string_view kHiddenButtonLabel = "Hidden";

inline constexpr float kInset = 10.0f;
inline constexpr float kButtonTop = 34.0f;
inline constexpr float kButtonHeight = 18.0f;
inline constexpr float kFieldHeight = 20.0f;
inline constexpr float kFieldStride = 24.0f;
inline constexpr float kQueryTop = 58.0f;
inline constexpr float kReplaceTop = kQueryTop + kFieldStride;
inline constexpr float kIncludeTop = kReplaceTop + kFieldStride;
inline constexpr float kExcludeTop = kIncludeTop + kFieldStride;
inline constexpr float kScopeExtraHeight = kFieldStride * 2.0f;
inline constexpr float kCollapsedStatusTop = 106.0f;
inline constexpr float kCollapsedResultsTop = 124.0f;

SDL_FRect QueryRect(const SDL_FRect& sidebar_rect);
SDL_FRect ReplaceRect(const SDL_FRect& sidebar_rect);
SDL_FRect IncludeRect(const SDL_FRect& sidebar_rect, bool scope_expanded);
SDL_FRect ExcludeRect(const SDL_FRect& sidebar_rect, bool scope_expanded);

inline constexpr float kReplaceAllButtonWidth = 34.0f;
SDL_FRect ReplaceAllButtonRect(const SDL_FRect& sidebar_rect);

SDL_FRect ModeButtonRect(const SDL_FRect& sidebar_rect);
SDL_FRect CaseButtonRect(const SDL_FRect& sidebar_rect);
SDL_FRect HiddenButtonRect(const SDL_FRect& sidebar_rect);
SDL_FRect ScopeButtonRect(const SDL_FRect& sidebar_rect);

struct FieldSlot {
  SDL_FRect rect;
  TextInputSurface surface;
  ProjectSearchEditField field;
};

inline std::array<FieldSlot, 4> SidebarSearchFieldRects(const SDL_FRect& sidebar_rect,
                                                        bool scope_expanded) {
  return {
      FieldSlot{QueryRect(sidebar_rect), TextInputSurface::SidebarSearchQuery,
                ProjectSearchEditField::Query},
      FieldSlot{ReplaceRect(sidebar_rect), TextInputSurface::SidebarSearchReplace,
                ProjectSearchEditField::Replace},
      FieldSlot{IncludeRect(sidebar_rect, scope_expanded), TextInputSurface::SidebarSearchInclude,
                ProjectSearchEditField::Include},
      FieldSlot{ExcludeRect(sidebar_rect, scope_expanded), TextInputSurface::SidebarSearchExclude,
                ProjectSearchEditField::Exclude},
  };
}

inline float StatusTop(bool scope_expanded) {
  return kCollapsedStatusTop + (scope_expanded ? kScopeExtraHeight : 0.0f);
}
inline float ResultsTop(bool scope_expanded) {
  return kCollapsedResultsTop + (scope_expanded ? kScopeExtraHeight : 0.0f);
}

}
