#pragma once

// The floating cards frame prep lays out whole -- the editor's hover card, the
// signature-help card and a control's tooltip -- as the list of what paint draws:
// the card's frame, its fills, its text lines placed and already wrapped and
// truncated, its button. Each card's derived-damage unit is keyed on this list
// (FingerprintOf), and PaintCard draws nothing else, so a card cannot show a value
// its key did not fold: the blame card's text, read live by its paint, once could.

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "render/SurfacePrimitives.h"
#include "render/TextRenderer.h"
#include "render/Theme.h"

namespace microide::workspace {

struct CardTextPaint {
  float x = 0.0f;
  float y = 0.0f;
  SDL_Color color{};
  std::string text;
  bool on_card = true;  // drawn on the card's background; false: over what is under it
};

struct CardFillPaint {
  SDL_FRect rect{};
  SDL_Color color{};
};

// The card's one action, an accent button.
struct CardButtonPaint {
  SDL_FRect rect{};
  std::string_view label;  // a static label ("Quick Fix", "Copy SHA")
  bool hovered = false;
};

// A card (a frame and what is on it), or an unframed overlay -- a field's
// selection, its caret, an IME preedit -- whose `rect` is everything it draws.
struct CardPaint {
  bool shown = false;
  bool framed = true;
  render::CardStyle style = render::CardStyle::Overlay;
  SDL_FRect rect{};
  std::vector<CardFillPaint> fills;
  std::vector<CardTextPaint> texts;
  std::optional<CardButtonPaint> button;

  // Nothing shows. Keeps the lists' capacity (and the strings') across frames.
  void Clear() {
    shown = false;
    fills.clear();
    texts.clear();
    button.reset();
  }
  // Shows the card at `card` in `card_style`, empty.
  void Open(const SDL_FRect& card, render::CardStyle card_style) {
    Clear();
    shown = true;
    framed = true;
    style = card_style;
    rect = card;
  }
  // An overlay with no frame; `covers` holds everything the builder will add.
  void OpenOverlay(const SDL_FRect& covers) {
    Clear();
    shown = true;
    framed = false;
    rect = covers;
  }
  void Fill(const SDL_FRect& fill, SDL_Color color) { fills.push_back(CardFillPaint{fill, color}); }
  void Text(float x, float y, SDL_Color color, std::string_view text) {
    texts.push_back(CardTextPaint{.x = x, .y = y, .color = color, .text = std::string(text), .on_card = true});
  }
  // Text over whatever is under it (an overlay's), no background of its own.
  void TextOver(float x, float y, SDL_Color color, std::string_view text) {
    texts.push_back(CardTextPaint{.x = x, .y = y, .color = color, .text = std::string(text), .on_card = false});
  }
  // A tooltip: one line of `text` on a tooltip card at `card`, its text centred
  // vertically, 8 px in.
  void Tooltip(const render::TextRenderer& text_renderer, const render::Theme& theme, const SDL_FRect& card,
               std::string_view text) {
    Open(card, render::CardStyle::Tooltip);
    Text(card.x + 8.0f, card.y + std::floor(std::max(0.0f, card.h - text_renderer.LineHeight()) * 0.5f),
         theme.text_primary, text);
  }
  // An overlay's rect: everything it draws, each text as wide as it measures.
  void FitOverlayToContents(const render::TextRenderer& text_renderer) {
    bool any = false;
    const auto join = [&](const SDL_FRect& r) {
      if (r.w <= 0.0f || r.h <= 0.0f) {
        return;
      }
      if (!any) {
        rect = r;
        any = true;
        return;
      }
      const float left = std::min(rect.x, r.x);
      const float top = std::min(rect.y, r.y);
      const float right = std::max(rect.x + rect.w, r.x + r.w);
      const float bottom = std::max(rect.y + rect.h, r.y + r.h);
      rect = SDL_FRect{left, top, right - left, bottom - top};
    };
    for (const CardFillPaint& fill : fills) {
      join(fill.rect);
    }
    for (const CardTextPaint& text : texts) {
      join(SDL_FRect{text.x, text.y, text_renderer.MeasureWidth(text.text), text_renderer.LineHeight()});
    }
    if (!any) {
      Clear();
    }
  }
  // Stacks `lines` from `*y` down, a line gap between them; `*y` ends below the last.
  void Lines(const render::TextRenderer& text_renderer, float x, float* y, SDL_Color color,
             const std::vector<std::string>& lines, float line_gap) {
    for (std::size_t i = 0; i < lines.size(); ++i) {
      Text(x, *y, color, lines[i]);
      *y += text_renderer.LineHeight();
      if (i + 1 < lines.size()) {
        *y += line_gap;
      }
    }
  }
};

// Every field of the card. The structured bindings name them all: a field added
// to a paint struct stops this compiling until it is folded in too.
std::uint64_t FingerprintOf(const CardPaint& card);

// Draws the card as prep laid it out, and nothing when it is not shown.
void PaintCard(SDL_Renderer* renderer, const CardPaint& card, const render::Theme& theme,
               const render::TextRenderer& text_renderer);

}  // namespace microide::workspace
