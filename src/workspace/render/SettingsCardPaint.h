#pragma once

#include <SDL3/SDL.h>

#include <functional>

#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "workspace/render/PreparedFrameState.h"
#include "workspace/render/RenderViewModelBuilder.h"

// The Settings / Help & About card as a derived-damage surface. Its view model
// is built once per prepared frame (it was built by every paint of the card),
// and each thing the card shows is a unit keyed on what it paints: the backdrop
// it dims, the card's own chrome (header, filter bar, section band, footer,
// scrollbars, the Help list), the scope chip's tooltip, the font picker, and one
// unit per category row of the rail and per value row.
//
// A row is lit under the pointer as paint lights it -- through `pointer_over`,
// which knows what covers the card -- so a pointer leaving a row for a toast
// over it un-lights the row with no request (soak 3f2fdebb5bd6: the toast took
// the motion, and nothing named the row). A change to the card owes a frame.
namespace microide::workspace {

struct SettingsCardInputs {
  SDL_Renderer* renderer = nullptr;  // for the device-pixel snap of unit rects
  const render::Theme* theme = nullptr;
  const render::TextRenderer* text_renderer = nullptr;
  SettingsOverlayViewModel vm;  // this frame's; `!vm.visible` when the card is closed
  SDL_FRect backdrop{};         // the region the card dims (the overlay anchor)
  std::function<bool(const SDL_FRect&)> pointer_over;
  bool scrollbar_dragged = false;
  bool category_scrollbar_dragged = false;
  bool picker_scrollbar_dragged = false;
  std::function<void(const SDL_FRect&)> damage;
};

// Settles the card into `frame.settings_card` and derives its damage.
void PrepareSettingsCardFrame(PreparedFrameState& frame, SettingsCardInputs inputs);

}  // namespace microide::workspace
