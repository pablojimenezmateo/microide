#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "project/DirectoryTree.h"
#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "workspace/registries/WorkspaceFileIconRegistry.h"
#include "workspace/render/PreparedFrameState.h"

// The sidebar's file tree -- its Collapse and Refresh buttons, each visible row
// and its scrollbar -- as a derived-damage surface (openspec derived-damage 4.1)
// painted element by element (clip-discipline). Free functions, as the git
// panel's (GitSidebarPaint.h). The mode tab row above it is shared by every
// sidebar view and stays with the sidebar surface.
namespace microide::workspace {

struct TreeSidebarInputs {
  SDL_Renderer* renderer = nullptr;  // for the device-pixel snap of unit rects
  bool visible = false;              // the sidebar is on screen showing the tree
  std::span<const project::TreeEntry> entries;
  std::size_t selected_index = 0;
  // The rows' list, laid out for this frame from the stored scroll.
  ScrollableListLayout list;
  SDL_FRect collapse_button{};
  SDL_FRect refresh_button{};
  bool can_collapse_all = false;
  // The resolved file icons, one per entry (a directory's is none), or null when
  // file icons are off.
  const std::vector<std::optional<WorkspaceFileIconRegistry::Icon>>* icons = nullptr;
  bool scrollbar_active = false;  // the thumb is being dragged
  const render::Theme* theme = nullptr;
  const render::TextRenderer* text_renderer = nullptr;
  std::function<bool(const SDL_FRect&)> pointer_over;
  std::function<void(const SDL_FRect&)> damage;
};

// Lays the tree out into `frame.tree_sidebar` and derives its damage: each button,
// each visible row and the scrollbar is a unit keyed on what it draws. A row's
// label views its tree entry, which outlives the frame's paint.
void PrepareTreeSidebarFrame(PreparedFrameState& frame, const TreeSidebarInputs& inputs);

// Draws `frame.tree_sidebar` and nothing else, each element inside its own
// render::PaintElement.
void PaintTreeSidebar(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                      const render::TextRenderer& text_renderer);

}  // namespace microide::workspace
