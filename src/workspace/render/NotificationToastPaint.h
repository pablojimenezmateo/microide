#pragma once

#include <SDL3/SDL.h>

#include <functional>

#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "workspace/render/PreparedFrameState.h"
#include "workspace/render/RenderViewModelBuilder.h"

namespace microide::workspace {

void PrepareNotificationToastsFrame(PreparedFrameState& frame, SDL_Renderer* renderer,
                                    const NotificationsViewModel& toasts, const render::Theme& theme,
                                    const std::function<void(const SDL_FRect&)>& damage);

void PaintNotificationToasts(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                             const render::TextRenderer& text_renderer);

}
