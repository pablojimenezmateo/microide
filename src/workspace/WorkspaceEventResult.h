#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <optional>

#include "util/SmallVector.h"

namespace microide::workspace {

// Damage rects queued for one event, inline up to `kInlineRedrawRects`.
//
// This is on every input event: a mouse move queues one rect, the shell copies
// the struct out and the app loop appends it to the frame's damage list, and
// with a `std::vector` that was two heap round-trips per event to carry 16
// bytes (TD-2026-08-06-145). Eight is measured, not guessed: with the counters
// below armed, `menu_hover_switch` queues 7 rects per event (1,120 over 160
// events) and every other interactive scenario stays at or under 4. Anything
// past it spills to the heap rather than being dropped, and each spill bumps
// `workspace.redraw_rect_spills` — so if a new surface starts queuing more, the
// wrong guess is visible in the gate rather than silently costing a malloc per
// event again.
//
// Sixteen since derived damage made a split divider drag precise (2026-11-25): a
// frame dragging a four-pane grid's divider damages each pane's rows and tail,
// strips, bands and dividers -- more than 8 rects, where the old whole-column rect
// swallowed them all (and painted 2.9x the pixels). `editor_split_grid_workout`
// spilled once per drag frame at 8 and not at 16.
inline constexpr std::size_t kInlineRedrawRects = 16;

struct RenderInvalidation {
  bool full = false;
  util::SmallVector<SDL_FRect, kInlineRedrawRects> rects;
  // The frame must be presented again although nothing in it changed: a cursor
  // shape only shows once the compositor recomposites (on Wayland it rides a
  // hardware plane re-latched on commit; dev-docs/platform/wayland-stale-cursor.md).
  // Deliberately NOT damage. It used to be a 1x1 rect at the pointer, and every
  // "repaint my surface unless something is already queued" fallback took that
  // pixel for the surface: a tree click that also changed the cursor expanded the
  // directory and repainted one pixel of it.
  bool present = false;
  // A frame is owed whose damage frame prep derives (openspec derived-damage): the
  // state changed on a surface that compares what it shows with what it painted
  // -- a toast posted or expired -- and the caller names no rect, because the
  // surface knows its own. The app prepares the frame and paints exactly what prep
  // damaged, or nothing; it is NOT a full repaint.
  bool frame = false;

  // Something on screen changed: a render pass is owed.
  [[nodiscard]] bool HasDamage() const { return full || !rects.empty(); }
  // A render pass, a prepared frame or a bare re-present is owed.
  [[nodiscard]] bool HasAnyRedraw() const { return HasDamage() || frame || present; }

  // Queue `rect`, unless a queued rect already covers it; queued rects it covers
  // go. Several handlers damage the same surface for one event (a menu switch
  // queued the same popup rect four times), and the duplicates spilled the inline
  // storage to the heap on every hover.
  void Add(const SDL_FRect& rect) {
    const auto covers = [](const SDL_FRect& outer, const SDL_FRect& inner) {
      return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w &&
             inner.y + inner.h <= outer.y + outer.h;
    };
    for (const SDL_FRect& queued : rects) {
      if (covers(queued, rect)) {
        return;
      }
    }
    std::size_t kept = 0;
    for (std::size_t i = 0; i < rects.size(); ++i) {
      if (!covers(rect, rects[i])) {
        rects[kept++] = rects[i];
      }
    }
    rects.resize_uninitialized(kept);  // shrinks: every kept slot is written
    rects.push_back(rect);
  }

  // Fold `other` in: full absorbs every rect, and a present is owed if either owes one.
  void Merge(const RenderInvalidation& other) {
    present = present || other.present;
    frame = frame || other.frame;
    if (full) {
      return;
    }
    if (other.full) {
      full = true;
      rects.clear();
      return;
    }
    for (const SDL_FRect& rect : other.rects) {
      Add(rect);
    }
  }

  [[nodiscard]] std::optional<SDL_FRect> SingleRectIfOnlyOne() const {
    if (rects.size() != 1) {
      return std::nullopt;
    }
    return rects.front();
  }
};

// A sample of the shell's damage generation; see WorkspaceShell::MarkDamage.
using DamageMark = std::uint64_t;

struct EventResult {
  bool handled = false;
  RenderInvalidation redraw{};
};

}  // namespace microide::workspace
