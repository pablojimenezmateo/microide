#pragma once

#include <SDL3/SDL.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "render/ScopedRenderClip.h"

namespace microide::render {

struct PaintOverrun {
  std::string element;
  SDL_Rect element_rect{};
  SDL_Rect draw_rect{};
  SDL_FRect element_logical{};
  SDL_FRect draw_logical{};
  SDL_FPoint scale{1.0f, 1.0f};
};

bool PaintOverrunRecordingEnabled();
void SetPaintOverrunRecording(bool enabled);

void NotePaint(SDL_Renderer* renderer, const SDL_FRect& logical);

std::vector<PaintOverrun> TakePaintOverruns();

class PaintPassScope {
 public:
  using Covers = bool (*)(const void* context, const SDL_FRect& visible);
  PaintPassScope(SDL_Renderer* renderer, std::string_view name, Covers covers, const void* context);
  ~PaintPassScope();
  PaintPassScope(const PaintPassScope&) = delete;
  PaintPassScope& operator=(const PaintPassScope&) = delete;
  void Check(SDL_Renderer* renderer, const SDL_FRect& logical) const;

 private:
  std::string name_;
  Covers covers_ = nullptr;
  const void* context_ = nullptr;
  PaintPassScope* parent_ = nullptr;
};

class PaintElement {
 public:
  PaintElement(SDL_Renderer* renderer, const SDL_FRect& rect, std::string_view name,
               std::optional<SDL_Color> opaque_background = std::nullopt);
  ~PaintElement();
  PaintElement(const PaintElement&) = delete;
  PaintElement& operator=(const PaintElement&) = delete;

  const SDL_Rect& snapped() const { return snapped_; }
  const SDL_FRect& logical() const { return logical_; }
  std::string_view name() const { return name_; }

 private:
  SDL_FRect logical_{};
  SDL_Rect snapped_{};
  std::string_view name_;
  PaintElement* parent_ = nullptr;
  std::optional<ScopedRenderClip> clip_;
};

}
