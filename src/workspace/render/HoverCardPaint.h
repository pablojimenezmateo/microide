#pragma once

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
  bool on_card = true;
};

struct CardFillPaint {
  SDL_FRect rect{};
  SDL_Color color{};
};

struct CardButtonPaint {
  SDL_FRect rect{};
  std::string_view label;
  bool hovered = false;
};

struct CardPaint {
  bool shown = false;
  bool framed = true;
  render::CardStyle style = render::CardStyle::Overlay;
  SDL_FRect rect{};
  std::vector<CardFillPaint> fills;
  std::vector<CardTextPaint> texts;
  std::optional<CardButtonPaint> button;

  void Clear() {
    shown = false;
    fills.clear();
    texts.clear();
    button.reset();
  }
  void Open(const SDL_FRect& card, render::CardStyle card_style) {
    Clear();
    shown = true;
    framed = true;
    style = card_style;
    rect = card;
  }
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
  void TextOver(float x, float y, SDL_Color color, std::string_view text) {
    texts.push_back(CardTextPaint{.x = x, .y = y, .color = color, .text = std::string(text), .on_card = false});
  }
  void Tooltip(const render::TextRenderer& text_renderer, const render::Theme& theme, const SDL_FRect& card,
               std::string_view text) {
    Open(card, render::CardStyle::Tooltip);
    Text(card.x + 8.0f, card.y + std::floor(std::max(0.0f, card.h - text_renderer.LineHeight()) * 0.5f),
         theme.text_primary, text);
  }
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

std::uint64_t FingerprintOf(const CardPaint& card);

void PaintCard(SDL_Renderer* renderer, const CardPaint& card, const render::Theme& theme,
               const render::TextRenderer& text_renderer);

}
