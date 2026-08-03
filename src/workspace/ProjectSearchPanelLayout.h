#pragma once

#include <SDL3/SDL.h>

#include <array>

#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/state/WorkspaceTextInputState.h"

// Geometry for the search sidebar's header: the toggle-button row, the query and
// replace fields, the optional include/exclude scope fields, and the vertical
// offsets the status line and result list start at.
//
// These are pure functions of the sidebar rect plus one "are the scope boxes
// expanded?" flag. They live outside WorkspaceShell so the shell stays an
// orchestrator and so render, hit-testing, cursor shaping, and the sidebar mouse
// coordinator can all agree on the same numbers without routing five
// `std::function<SDL_FRect(const SDL_FRect&)>` callbacks through the shell.
namespace microide::workspace::project_search_panel {

inline constexpr float kInset = 10.0f;
inline constexpr float kButtonTop = 34.0f;
inline constexpr float kButtonHeight = 18.0f;
inline constexpr float kFieldHeight = 20.0f;
// Vertical distance between consecutive text fields (field height + gap).
inline constexpr float kFieldStride = 24.0f;
inline constexpr float kQueryTop = 58.0f;
inline constexpr float kReplaceTop = kQueryTop + kFieldStride;
inline constexpr float kIncludeTop = kReplaceTop + kFieldStride;
inline constexpr float kExcludeTop = kIncludeTop + kFieldStride;
// Extra vertical space the two scope fields occupy when expanded.
inline constexpr float kScopeExtraHeight = kFieldStride * 2.0f;
inline constexpr float kCollapsedStatusTop = 106.0f;
inline constexpr float kCollapsedResultsTop = 124.0f;

SDL_FRect QueryRect(const SDL_FRect& sidebar_rect);
SDL_FRect ReplaceRect(const SDL_FRect& sidebar_rect);
// The scope fields exist only while expanded. When collapsed these return a
// zero-sized rect, so every `Contains(...)` hit test misses without the caller
// needing its own visibility branch.
SDL_FRect IncludeRect(const SDL_FRect& sidebar_rect, bool scope_expanded);
SDL_FRect ExcludeRect(const SDL_FRect& sidebar_rect, bool scope_expanded);

SDL_FRect ModeButtonRect(const SDL_FRect& sidebar_rect);
SDL_FRect CaseButtonRect(const SDL_FRect& sidebar_rect);
SDL_FRect HiddenButtonRect(const SDL_FRect& sidebar_rect);
// The "..." toggle that shows/hides the include/exclude fields, mirroring the
// same affordance in VS Code's search view.
SDL_FRect ScopeButtonRect(const SDL_FRect& sidebar_rect);

// One search-panel text field: where it is, which text-input surface owns it, and
// which committed editor it edits. Returned in draw/tab order so render,
// hit-testing, drag re-derivation, and Tab cycling all walk the same list instead
// of each re-spelling the field set (which is how query/replace drifted from
// having any scope fields at all).
struct FieldSlot {
  SDL_FRect rect;
  TextInputSurface surface;
  ProjectSearchEditField field;
};

// Collapsed scope fields are present in the array with a zero-sized rect; callers
// skip them with a single `rect.w <= 0.0f` check.
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

}  // namespace microide::workspace::project_search_panel
