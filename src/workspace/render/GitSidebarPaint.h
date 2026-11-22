#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <functional>
#include <memory>

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

struct GitPanelHeadInputs {
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
  // The file list under the head, as laid out for this frame. Until its rows are
  // units of their own, the list is one: the head growing a banner moves it.
  SDL_FRect list_rect{};
  int list_scroll_row = 0;
  std::size_t list_line_count = 0;
  const render::Theme* theme = nullptr;
  const render::TextRenderer* text_renderer = nullptr;  // the draft's fields are measured here
  std::function<bool(const SDL_FRect&)> pointer_over;
  std::function<void(const SDL_FRect&)> damage;
};

// Lays the panel's head out into `frame.git_panel` and derives its damage: each
// button, each text line, each field and the caret is a unit. The draft's control
// rects and caret land in `frame.git_panel` for the caller to publish to the
// commit workflow's hit-testing.
void PrepareGitPanelHeadFrame(PreparedFrameState& frame, const GitPanelHeadInputs& inputs);

// Draws `frame.git_panel` and nothing else, each element inside its own
// render::PaintElement.
void PaintGitPanelHead(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                       const render::TextRenderer& text_renderer);

}  // namespace microide::workspace
