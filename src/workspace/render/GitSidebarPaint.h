#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "workspace/git/CommitWorkflowState.h"
#include "workspace/git/WorkspaceGitSidebarPresentation.h"
#include "workspace/render/PreparedFrameState.h"

// The Source Control panel's head: its action row (Stage All, Discard All,
// Refresh), its branch and sync buttons, the Commit button
// or commit line, its banners, and the open commit draft (the staged summary, the
// Subject and Body fields, the confirm button, the pre-check and status lines), as
// a derived-damage surface (openspec derived-damage 4.1) painted element by
// element (clip-discipline 4.1). Free functions, as the debug pane's
// (DebugPanePaint.h).
namespace microide::workspace {

struct GitPanelInputs {
  SDL_Renderer* renderer = nullptr;  // for the device-pixel snap of unit rects
  bool visible = false;              // the sidebar is on screen in Source Control
  SDL_FRect sidebar{};
  // The presentation this frame's sidebar view model holds; the frame keeps it, so
  // every label laid out from it stays valid until the frame is replaced.
  std::shared_ptr<const GitSidebarPresentation> presentation;
  const CommitWorkflowState* workflow = nullptr;  // the commit draft, when it is open
  bool stage_all_enabled = false;
  bool discard_all_enabled = false;
  bool commit_can_execute = false;                // the draft's confirm button is enabled
  bool panel_focused = false;                     // keyboard focus is in this panel
  bool caret_on = false;                          // the blink shows the caret this frame
  // The list under the head, laid out for this frame with its scroll settled.
  ScrollableListLayout list;
  std::size_t selected_entry = 0;               // the git view's selected entry index
  std::optional<SDL_FRect> outgoing_base_button;  // on the Outgoing header, when it shows
  bool outgoing_base_menu_open = false;
  bool scrollbar_active = false;  // the list's thumb is being dragged
  const render::Theme* theme = nullptr;
  const render::TextRenderer* text_renderer = nullptr;  // the draft's fields are measured here
  std::function<bool(const SDL_FRect&)> pointer_over;
  std::function<void(const SDL_FRect&)> damage;
};

// Lays the panel out into `frame.git_panel` and derives its damage: each button,
// text line, field, the caret, each visible list row and the list's scrollbar is a
// unit. The draft's control rects and caret land in `frame.git_panel` for the
// caller to publish to the commit workflow's hit-testing.
void PrepareGitPanelFrame(PreparedFrameState& frame, const GitPanelInputs& inputs);

// Draws `frame.git_panel` and nothing else, each element inside its own
// render::PaintElement.
void PaintGitPanel(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                   const render::TextRenderer& text_renderer);

// The git list as the last prepared frame laid it out: its lines and layout, for
// mouse hit-testing, so a click lands on the row the user saw even when a refresh
// or a collapse has changed the live list since. Null when that frame showed no
// git view; the caller falls back to the live list.
struct PaintedGitSidebarList {
  const std::vector<GitSidebarLine>* lines = nullptr;
  const ScrollableListLayout* layout = nullptr;
};
PaintedGitSidebarList PaintedGitSidebarListOf(const PreparedFrameState& frame);

// The live entry a painted line shows: matched by its repository-relative path and
// section, since a refresh since that frame may have moved it or removed it.
// nullopt for a line that shows no entry, or whose entry is gone.
std::optional<std::size_t> ResolvePaintedGitEntry(const PreparedFrameState& frame, const GitSidebarLine& line,
                                                  const GitSidebarState& git);

}  // namespace microide::workspace
