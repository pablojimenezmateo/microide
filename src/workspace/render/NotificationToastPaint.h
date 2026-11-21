#pragma once

#include <SDL3/SDL.h>

#include <functional>

#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "workspace/render/PreparedFrameState.h"
#include "workspace/render/RenderViewModelBuilder.h"

// The notification toasts over the status bar's right end, as a derived-damage
// surface (openspec derived-damage 3.1) painted element by element
// (clip-discipline stage 3). Free functions, as the bottom panel's header
// (BottomPanelHeaderPaint.h).
namespace microide::workspace {

// Settles the toasts this frame paints -- the builder's view model, composed once
// here instead of in paint -- into `frame.notification_toasts`, and derives their
// damage: each toast is a unit whose fingerprint is everything it shows.
void PrepareNotificationToastsFrame(PreparedFrameState& frame, SDL_Renderer* renderer,
                                    const NotificationsViewModel& toasts, const render::Theme& theme,
                                    const std::function<void(const SDL_FRect&)>& damage);

// Draws `frame.notification_toasts` and nothing else, each toast inside its own
// render::PaintElement.
void PaintNotificationToasts(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                             const render::TextRenderer& text_renderer);

// The rect the toasts cover together (empty without any): what a change the
// units cannot see yet -- a hover moving between them -- asks to repaint.
SDL_FRect NotificationToastsBounds(const NotificationsViewModel& toasts);

}  // namespace microide::workspace
