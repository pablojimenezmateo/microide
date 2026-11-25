#include "workspace/render/WorkspaceShellRenderPrimitives.h"

namespace microide::workspace {

using namespace detail;

// The prompt dialog as frame prep resolved it (PreparedFrameState::prompt_dialog):
// what it says, its field's displayed text, its buttons' hover and selection.
void WorkspaceShell::RenderPromptSurface(SDL_Renderer* renderer, const WorkspaceLayout& layout) const {
  const PreparedFrameState::PromptDialogPaint& prompt = frame_state_.prompt_dialog;
  if (!prompt.shown) {
    return;
  }

  DrawFilledRect(renderer, layout.full, theme_.overlay_backdrop);

  const SDL_FRect dialog = ComputePromptSurfaceRect(layout.full);
  const SDL_FRect header = DrawTitledCardFrame(renderer, theme_, dialog, 32.0f, CardStyle::Overlay);
  const SDL_FRect message_rect =
      MakeRect(dialog.x + 16.0f, dialog.y + 50.0f, dialog.w - 32.0f, 36.0f);
  const auto buttons = ComputePromptSurfaceButtonRects(dialog, prompt.laid_out_buttons);
  DrawVCenteredTextOn(text_renderer_, renderer, header, 16.0f, theme_.chrome_text,
                      theme_.chrome_background, prompt.title);
  // The message wraps over the room down to the input field or the buttons (less
  // a line for the detail): drawn as one truncated line, "Renaming to 'x' changes
  // 2 files (1 not open). Apply…" lost its question over an empty card (QA
  // lsp-workspace-edits).
  const std::string_view detail = prompt.detail;
  const float line_height = text_renderer_.LineHeight();
  const float content_bottom =
      (prompt.text_input ? ComputePromptSurfaceInputRect(dialog).y : buttons.front().y) -
      6.0f;
  const float room = std::max(line_height, content_bottom - message_rect.y -
                                               (detail.empty() ? 0.0f : line_height + 4.0f));
  const std::size_t message_lines = DrawWrappedPlaceholderIn(
      text_renderer_, renderer, MakeRect(message_rect.x, message_rect.y, message_rect.w, room),
      theme_.text_muted, theme_.overlay_background, prompt.message, kWrapAsTheBoxHolds);
  if (!detail.empty()) {
    const float detail_y = message_rect.y +
                           static_cast<float>(std::max<std::size_t>(1, message_lines)) * line_height + 4.0f;
    DrawTextOn(text_renderer_, renderer, message_rect.x, detail_y, theme_.text_secondary,
               theme_.overlay_background, TruncateLabelView(detail, message_rect.w));
  }

  if (prompt.text_input) {
    const SDL_FRect input_rect = ComputePromptSurfaceInputRect(dialog);
    DrawTextFieldFrame(renderer, theme_, input_rect, true);
    DrawSingleLineTextTail(renderer, input_rect.x + 6.0f, input_rect.y + 4.0f,
                           std::max(1.0f, input_rect.w - 12.0f), theme_.surface_text,
                           theme_.surface_background, prompt.input_text);
  }

  // As many buttons as prep resolved labels and rects for (it bounded them by both).
  for (std::size_t i = 0; i < std::min(buttons.size(), prompt.button_count); ++i) {
    DrawButtonCentered(text_renderer_, renderer, theme_, buttons[i], prompt.labels[i], ButtonTone::Neutral,
                       ButtonVisualState{
                           .enabled = true,
                           .hovered = prompt.hovered[i],
                           .active = prompt.selected == static_cast<int>(i),
                       });
  }
}

// The unsaved-changes prompt as frame prep resolved it (dirty_prompt_dialog).
void WorkspaceShell::RenderDirtyPromptSurface(SDL_Renderer* renderer,
                                              const WorkspaceLayout& layout) const {
  const PreparedFrameState::PromptDialogPaint& dirty = frame_state_.dirty_prompt_dialog;
  if (!dirty.shown) {
    return;
  }

  DrawFilledRect(renderer, layout.full, theme_.overlay_backdrop);

  const SDL_FRect dialog = ComputeDirtyPromptRect(layout.full);
  const SDL_FRect header = DrawTitledCardFrame(renderer, theme_, dialog, 32.0f, CardStyle::Overlay);
  const SDL_FRect message_rect =
      MakeRect(dialog.x + 16.0f, dialog.y + 50.0f, dialog.w - 32.0f, 54.0f);

  DrawVCenteredTextOn(text_renderer_, renderer, header, 12.0f, theme_.chrome_text,
                      theme_.chrome_background, dirty.title);
  // Wrapped like the plain prompt's, over the room above the hint line and the
  // buttons ("Save the 2 affected dirty editors before renaming re…").
  const auto buttons = ComputeDirtyPromptButtonRects(dialog);
  const float line_height = text_renderer_.LineHeight();
  const float room =
      std::max(line_height, buttons.front().y - 6.0f - message_rect.y - (line_height + 4.0f));
  const std::size_t message_lines = DrawWrappedPlaceholderIn(
      text_renderer_, renderer, MakeRect(message_rect.x, message_rect.y, message_rect.w, room),
      theme_.text_secondary, theme_.overlay_background, dirty.message, kWrapAsTheBoxHolds);
  const float hint_y =
      message_rect.y + static_cast<float>(std::max<std::size_t>(1, message_lines)) * line_height + 4.0f;
  // Constant hint line: spelled out rather than re-joined per frame. The joiner
  // lives in WorkspaceUiText.h for text assembled from live state, which belongs
  // in RenderViewModelBuilder, not in a render TU -- but the SEPARATOR is still
  // kHintSeparator. Spelled "  |  " here until 2026-09-19, which made the dirty
  // prompt the third hint line in the app punctuated differently from every
  // other one. The lint targets a redefined joiner, so a divergent spelling
  // baked into one literal is invisible to it.
  DrawTextOn(text_renderer_, renderer, message_rect.x, hint_y, theme_.text_muted,
             theme_.overlay_background,
             std::string_view("Enter confirm · Left/Right choose · Esc cancel"));

  for (std::size_t i = 0; i < std::min(buttons.size(), dirty.button_count); ++i) {
    DrawButtonCentered(text_renderer_, renderer, theme_, buttons[i], dirty.labels[i],
                       i == 1 ? ButtonTone::Destructive : ButtonTone::Neutral,
                       ButtonVisualState{
                           .enabled = true,
                           .hovered = dirty.hovered[i],
                           .active = dirty.selected == static_cast<int>(i),
                       });
  }
}

}  // namespace microide::workspace
