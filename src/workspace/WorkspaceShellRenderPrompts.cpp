#include "workspace/WorkspaceShellRenderPrimitives.h"

namespace microide::workspace {

using namespace detail;

void WorkspaceShell::RenderPromptSurface(
    SDL_Renderer* renderer,
    const WorkspaceLayout& layout,
    const std::optional<TextInputVisual>& active_text_input_visual) const {
  if (!context_.prompts.surface_visible) {
    return;
  }

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  DrawFilledRect(renderer, layout.full, SDL_Color{0x05, 0x07, 0x0b, 0xcc});

  const SDL_FRect dialog = ComputePromptSurfaceRect(layout.full);
  const SDL_FRect header = MakeRect(dialog.x, dialog.y, dialog.w, 32.0f);
  const SDL_FRect message_rect =
      MakeRect(dialog.x + 16.0f, dialog.y + 50.0f, dialog.w - 32.0f, 36.0f);
  DrawFilledRect(renderer, dialog, theme_.overlay_background);
  DrawRect(renderer, dialog, theme_.border);
  DrawFilledRect(renderer, header, theme_.chrome_background);
  DrawFilledRect(renderer, MakeRect(header.x, header.y + header.h - 1.0f, header.w, 1.0f),
                 theme_.border);
  DrawVCenteredTextOn(text_renderer_, renderer, header, 16.0f, theme_.chrome_text,
                      theme_.chrome_background, PromptSurfaceTitle());
  DrawTextOn(text_renderer_, renderer, message_rect.x, message_rect.y, theme_.text_muted,
             theme_.overlay_background, TruncateLabel(PromptSurfaceMessage(), message_rect.w));

  if (context_.prompts.surface.kind == PromptSurfaceState::Kind::TextInput) {
    const SDL_FRect input_rect = ComputePromptSurfaceInputRect(dialog);
    DrawFilledRect(renderer, input_rect, theme_.surface_background);
    DrawRect(renderer, input_rect, theme_.border);
    DrawVCenteredTextOn(text_renderer_, renderer, input_rect, 6.0f, theme_.surface_text,
                        theme_.surface_background,
                        TruncateLabel(context_.prompts.surface.input, input_rect.w - 12.0f));
    if (context_.interaction_state.window_has_input_focus && context_.text_input.composition.text.empty() &&
        active_text_input_visual.has_value() &&
        active_text_input_visual->surface == TextInputSurface::PromptInput) {
      DrawFilledRect(renderer,
                     MakeRect(active_text_input_visual->cursor_x,
                              active_text_input_visual->text_y - 1.0f, 1.5f,
                              text_renderer_.LineHeight()),
                     theme_.cursor);
    }
  }

  const auto buttons = ComputePromptSurfaceButtonRects(dialog);
  const auto labels = PromptSurfaceActionLabels();
  for (std::size_t i = 0; i < buttons.size(); ++i) {
    const bool selected = context_.prompts.surface.selected_button == static_cast<int>(i);
    const SDL_Color background = selected ? theme_.chrome_active : theme_.surface_raised;
    DrawFilledRect(renderer, buttons[i], background);
    DrawRect(renderer, buttons[i], selected ? theme_.accent : theme_.border);
    DrawCenteredTextOn(text_renderer_, renderer, buttons[i],
                       selected ? theme_.chrome_active_text : theme_.surface_text, background,
                       labels[i]);
  }
}

void WorkspaceShell::RenderDirtyPromptSurface(SDL_Renderer* renderer,
                                              const WorkspaceLayout& layout) const {
  if (!context_.prompts.dirty_visible) {
    return;
  }

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  DrawFilledRect(renderer, layout.full, SDL_Color{0x05, 0x07, 0x0b, 0xcc});

  const SDL_FRect dialog = ComputeDirtyPromptRect(layout.full);
  const SDL_FRect header = MakeRect(dialog.x, dialog.y, dialog.w, 32.0f);
  const SDL_FRect message_rect =
      MakeRect(dialog.x + 16.0f, dialog.y + 50.0f, dialog.w - 32.0f, 54.0f);
  DrawFilledRect(renderer, dialog, theme_.overlay_background);
  DrawRect(renderer, dialog, theme_.border);
  DrawFilledRect(renderer, header, theme_.chrome_background);
  DrawFilledRect(renderer, MakeRect(header.x, header.y + header.h - 1.0f, header.w, 1.0f),
                 theme_.border);

  DrawVCenteredTextOn(text_renderer_, renderer, header, 12.0f, theme_.chrome_text,
                      theme_.chrome_background, DirtyPromptTitle());
  DrawTextOn(text_renderer_, renderer, message_rect.x, message_rect.y, theme_.text_secondary,
             theme_.overlay_background, TruncateLabel(DirtyPromptMessage(), message_rect.w));
  DrawTextOn(text_renderer_, renderer, message_rect.x, message_rect.y + 22.0f, theme_.text_muted,
             theme_.overlay_background, "Enter confirm  Left/Right choose  Esc cancel");

  const auto buttons = ComputeDirtyPromptButtonRects(dialog);
  const auto labels = DirtyPromptActionLabels();
  for (std::size_t i = 0; i < buttons.size(); ++i) {
    const bool selected = context_.prompts.dirty.selected_action == static_cast<int>(i);
    DrawFilledRect(renderer, buttons[i],
                   selected ? theme_.chrome_active : theme_.surface_raised);
    DrawRect(renderer, buttons[i], selected ? theme_.accent : theme_.border);
    DrawCenteredTextOn(text_renderer_, renderer, buttons[i],
                       selected ? theme_.chrome_active_text : theme_.surface_text,
                       selected ? theme_.chrome_active : theme_.surface_raised, labels[i]);
  }

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

}  // namespace microide::workspace
