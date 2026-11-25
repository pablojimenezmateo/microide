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

namespace microide::workspace {

struct GitPanelInputs {
  SDL_Renderer* renderer = nullptr;
  bool visible = false;
  SDL_FRect sidebar{};
  std::shared_ptr<const GitSidebarPresentation> presentation;
  const CommitWorkflowState* workflow = nullptr;
  bool stage_all_enabled = false;
  bool discard_all_enabled = false;
  bool commit_can_execute = false;
  bool panel_focused = false;
  bool caret_on = false;
  ScrollableListLayout list;
  std::size_t selected_entry = 0;
  std::optional<SDL_FRect> outgoing_base_button;
  bool outgoing_base_menu_open = false;
  bool scrollbar_active = false;
  const render::Theme* theme = nullptr;
  const render::TextRenderer* text_renderer = nullptr;
  std::function<bool(const SDL_FRect&)> pointer_over;
  std::function<void(const SDL_FRect&)> damage;
};

void PrepareGitPanelFrame(PreparedFrameState& frame, const GitPanelInputs& inputs);

void PaintGitPanel(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                   const render::TextRenderer& text_renderer);

struct PaintedGitSidebarList {
  const std::vector<GitSidebarLine>* lines = nullptr;
  const ScrollableListLayout* layout = nullptr;
};
PaintedGitSidebarList PaintedGitSidebarListOf(const PreparedFrameState& frame);

std::optional<std::size_t> ResolvePaintedGitEntry(const PreparedFrameState& frame, const GitSidebarLine& line,
                                                  const GitSidebarState& git);

}
