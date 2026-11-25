#include "workspace/render/HoverCardPaint.h"

#include "workspace/render/DerivedDamage.h"
#include "workspace/render/WorkspaceShellRenderPrimitives.h"

namespace microide::workspace {

std::uint64_t FingerprintOf(const CardPaint& card) {
  const auto& [shown, framed, style, rect, fills, texts, button] = card;
  PaintFingerprint key;
  key.Add(shown);
  if (!shown) {
    return key.value();
  }
  key.Add(framed).Add(style).Add(rect).Add(fills.size()).Add(texts.size());
  for (const CardFillPaint& fill : fills) {
    const auto& [fill_rect, color] = fill;
    key.Add(fill_rect).Add(color);
  }
  for (const CardTextPaint& text : texts) {
    const auto& [x, y, color, string, on_card] = text;
    key.Add(x).Add(y).Add(color).Add(std::string_view(string)).Add(on_card);
  }
  key.Add(button.has_value());
  if (button.has_value()) {
    const auto& [button_rect, label, hovered] = *button;
    key.Add(button_rect).Add(label).Add(hovered);
  }
  return key.value();
}

void PaintCard(SDL_Renderer* renderer, const CardPaint& card, const render::Theme& theme,
               const render::TextRenderer& text_renderer) {
  if (!card.shown) {
    return;
  }
  if (card.framed) {
    render::DrawCardFrame(renderer, theme, card.rect, card.style);
  }
  for (const CardFillPaint& fill : card.fills) {
    render::FillRect(renderer, fill.rect, fill.color);
  }
  const SDL_Color background = render::CardBackground(theme, card.style);
  for (const CardTextPaint& text : card.texts) {
    if (text.on_card) {
      text_renderer.DrawStringOn(renderer, text.x, text.y, text.color, background, text.text);
    } else {
      text_renderer.DrawString(renderer, text.x, text.y, text.color, text.text);
    }
  }
  if (card.button.has_value()) {
    detail::DrawButtonCentered(text_renderer, renderer, theme, card.button->rect, card.button->label,
                               detail::ButtonTone::Accent,
                               detail::ButtonVisualState{.enabled = true, .hovered = card.button->hovered, .active = false});
  }
}

}
