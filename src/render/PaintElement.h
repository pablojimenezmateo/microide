#pragma once

#include <SDL3/SDL.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "render/ScopedRenderClip.h"

// openspec clip-discipline D1: an element (a tab, a row, a cell, a divider) is
// painted inside a PaintElement scope on its layout rect. The scope clips to the
// rect -- so a full frame draws exactly what a partial frame clipped to the rect
// can -- fills it first when the element is opaque, and, while overrun recording
// is on (the QA redraw verifier, redraw tests), notes every draw that falls
// outside the rect: the defect a stale `clip-edge` frame only showed afterwards,
// named where it happens.
namespace microide::render {

struct PaintOverrun {
  std::string element;
  SDL_Rect element_rect{};  // device-snapped
  SDL_Rect draw_rect{};     // device-snapped
  // The same two as the paint code gave them, and the device scale they were
  // snapped at: an overrun no replay reproduces is diagnosed from its log line.
  SDL_FRect element_logical{};
  SDL_FRect draw_logical{};
  SDL_FPoint scale{1.0f, 1.0f};
};

// Whether draws are checked against their element (MICROIDE_QA_VERIFY_REDRAW, or a
// test's SetPaintOverrunRecording). Off: a scope costs its clip and fill only.
bool PaintOverrunRecordingEnabled();
void SetPaintOverrunRecording(bool enabled);

// A drawing primitive reports the logical rect it is about to touch. Free unless
// recording is on and a scope is open.
void NotePaint(SDL_Renderer* renderer, const SDL_FRect& logical);

// The overruns noted since the last call, once per element name.
std::vector<PaintOverrun> TakePaintOverruns();

// A paint pass that a partial frame may skip (the workspace's RenderClip runs it
// only for clips that touch what it declared it paints). While one is open and
// recording is on, every draw it makes -- as far as the renderer's clip and the
// innermost element let it show -- must fall inside a rect `covers` accepts; one
// that does not is an overrun named "pass:<name>": a pixel a partial frame skipping
// the pass would leave stale.
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

}  // namespace microide::render
