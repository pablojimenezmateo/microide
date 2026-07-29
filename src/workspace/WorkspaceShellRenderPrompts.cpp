#include "workspace/WorkspaceShellRenderPrimitives.h"

namespace microide::workspace {

using namespace detail;

void WorkspaceShell::RenderPromptSurface(
    SDL_Renderer* renderer,
    const WorkspaceLayout& layout,
    const std::optional<TextInputVisual>& active_text_input_visual) const {
  (void)active_text_input_visual;
  if (!context_.prompts.surface_visible) {
    return;
  }

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  DrawFilledRect(renderer, layout.full, theme_.overlay_backdrop);

  const SDL_FRect dialog = ComputePromptSurfaceRect(layout.full);
  const SDL_FRect header = DrawTitledCardFrame(renderer, theme_, dialog, 32.0f, CardStyle::Overlay);
  const SDL_FRect message_rect =
      MakeRect(dialog.x + 16.0f, dialog.y + 50.0f, dialog.w - 32.0f, 36.0f);
  const SDL_FRect detail_rect =
      MakeRect(dialog.x + 16.0f, dialog.y + 72.0f, dialog.w - 32.0f, 40.0f);
  DrawVCenteredTextOn(text_renderer_, renderer, header, 16.0f, theme_.chrome_text,
                      theme_.chrome_background, PromptSurfaceTitle());
  DrawTextOn(text_renderer_, renderer, message_rect.x, message_rect.y, theme_.text_muted,
             theme_.overlay_background, TruncateLabelView(PromptSurfaceMessage(), message_rect.w));
  if (const std::string detail = PromptSurfaceDetail(); !detail.empty()) {
    DrawTextOn(text_renderer_, renderer, detail_rect.x, detail_rect.y, theme_.text_secondary,
               theme_.overlay_background, TruncateLabelView(detail, detail_rect.w));
  }

  if (context_.prompts.surface.kind == PromptSurfaceState::Kind::TextInput) {
    const SDL_FRect input_rect = ComputePromptSurfaceInputRect(dialog);
    DrawTextFieldFrame(renderer, theme_, input_rect, true);
    const std::string_view prompt_text =
        (active_text_input_visual.has_value() &&
         active_text_input_visual->surface == TextInputSurface::PromptInput &&
         !active_text_input_visual->displayed_text.empty())
            ? std::string_view(active_text_input_visual->displayed_text)
            : std::string_view(context_.prompts.surface.input.text());
    DrawSingleLineTextTail(renderer, input_rect.x + 6.0f, input_rect.y + 4.0f,
                           std::max(1.0f, input_rect.w - 12.0f), theme_.surface_text,
                           theme_.surface_background, prompt_text);
  }

  const auto buttons =
      ComputePromptSurfaceButtonRects(dialog, context_.prompts.surface.button_count);
  const auto labels = PromptSurfaceActionLabels();
  for (std::size_t i = 0; i < buttons.size(); ++i) {
    DrawButtonCentered(
        text_renderer_, renderer, theme_, buttons[i], labels[i], ButtonTone::Neutral,
        ButtonVisualState{
            .enabled = true,
            .hovered = last_mouse_position_valid_ &&
                       Contains(buttons[i], last_mouse_x_, last_mouse_y_),
            .active = context_.prompts.surface.selected_button == static_cast<int>(i),
        });
  }

  // Restore the default blend mode so BLEND does not leak into later draws in the
  // frame, matching RenderDirtyPromptSurface below.
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

void WorkspaceShell::RenderDirtyPromptSurface(SDL_Renderer* renderer,
                                              const WorkspaceLayout& layout) const {
  if (!context_.prompts.dirty_visible) {
    return;
  }

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  DrawFilledRect(renderer, layout.full, theme_.overlay_backdrop);

  const SDL_FRect dialog = ComputeDirtyPromptRect(layout.full);
  const SDL_FRect header = DrawTitledCardFrame(renderer, theme_, dialog, 32.0f, CardStyle::Overlay);
  const SDL_FRect message_rect =
      MakeRect(dialog.x + 16.0f, dialog.y + 50.0f, dialog.w - 32.0f, 54.0f);

  DrawVCenteredTextOn(text_renderer_, renderer, header, 12.0f, theme_.chrome_text,
                      theme_.chrome_background, DirtyPromptTitle());
  DrawTextOn(text_renderer_, renderer, message_rect.x, message_rect.y, theme_.text_secondary,
             theme_.overlay_background, TruncateLabelView(DirtyPromptMessage(), message_rect.w));
  // Constant hint line: spelled out rather than re-joined per frame. The joiner
  // lives in WorkspaceUiText.h for text assembled from live state, which belongs
  // in RenderViewModelBuilder, not in a render TU.
  DrawTextOn(text_renderer_, renderer, message_rect.x, message_rect.y + 22.0f, theme_.text_muted,
             theme_.overlay_background,
             std::string_view("Enter confirm  |  Left/Right choose  |  Esc cancel"));

  const auto buttons = ComputeDirtyPromptButtonRects(dialog);
  const auto labels = DirtyPromptActionLabels();
  for (std::size_t i = 0; i < buttons.size(); ++i) {
    DrawButtonCentered(
        text_renderer_, renderer, theme_, buttons[i], labels[i],
        i == 1 ? ButtonTone::Destructive : ButtonTone::Neutral,
        ButtonVisualState{
            .enabled = true,
            .hovered = last_mouse_position_valid_ &&
                       Contains(buttons[i], last_mouse_x_, last_mouse_y_),
            .active = context_.prompts.dirty.selected_action == static_cast<int>(i),
        });
  }

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

}  // namespace microide::workspace
