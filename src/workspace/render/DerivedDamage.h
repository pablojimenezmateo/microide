#pragma once

// Damage derived from paint snapshots (openspec: derived-damage).
//
// A surface reports, at frame prep, one unit per thing it paints separately --
// a segment, a row, a tab -- as the rect it covers and a fingerprint of every
// value its paint reads for it. Commit compares that with what the surface
// reported for the frame it last painted and damages the old and new rect of
// every unit that changed, appeared or went away. Nothing else has to remember
// to ask for those rects: a change no code path announced (a background thread,
// a control command, a plugin) is repainted all the same.
//
// The fingerprint is only as complete as what paint reads, so a migrated
// surface's paint reads nothing but the snapshot the fingerprint was folded
// from (the render-surface lint enforces it).

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <vector>

#include "util/Fnv1a.h"
#include "util/PerformanceCounters.h"

namespace microide::workspace {

// The surfaces whose damage is derived; one unit table each.
enum class DerivedSurface : std::uint8_t {
  StatusBar,
  BottomPanelHeader,
  BottomPanelOutput,
  BottomPanelTerminalChrome,
  NotificationToasts,
  DebugPane,
  // One per editor group (kMaxEditorGroups): each pane's visible rows.
  EditorPane0,
  EditorPane7 = EditorPane0 + 7,
  Count,
};

// Folds the values a unit paints into one 64-bit fingerprint.
class PaintFingerprint {
 public:
  PaintFingerprint& Add(std::string_view text) {
    Mix(text.size());
    hash_ = util::Fnv1aBytes(hash_, text);
    return *this;
  }
  PaintFingerprint& Add(const SDL_FRect& rect) { return Add(rect.x).Add(rect.y).Add(rect.w).Add(rect.h); }
  PaintFingerprint& Add(const SDL_Color& color) {
    return Mix((static_cast<std::uint64_t>(color.r) << 24) | (static_cast<std::uint64_t>(color.g) << 16) |
               (static_cast<std::uint64_t>(color.b) << 8) | color.a);
  }
  PaintFingerprint& Add(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return Mix(bits);
  }
  PaintFingerprint& Add(bool value) { return Mix(value ? 1u : 2u); }
  template <typename T>
    requires(std::is_integral_v<T> || std::is_enum_v<T>)
  PaintFingerprint& Add(T value) {
    return Mix(static_cast<std::uint64_t>(value));
  }
  std::uint64_t value() const { return hash_; }

 private:
  PaintFingerprint& Mix(std::uint64_t word) {
    hash_ = util::Fnv1aValue(hash_, word);
    return *this;
  }
  std::uint64_t hash_ = util::kFnv1aOffsetBasis;
};

class DerivedDamage {
 public:
  struct Unit {
    SDL_FRect rect{};
    std::uint64_t fingerprint = 0;
  };

  // The surface's units for the frame being prepared, in a fixed order (a
  // unit's index is its identity: segment 3 is compared with segment 3). The
  // returned list is cleared and keeps its capacity across frames.
  std::vector<Unit>& Begin(DerivedSurface surface) {
    std::vector<Unit>& units = Table(surface).current;
    units.clear();
    return units;
  }

  // Damages (through `damage(const SDL_FRect&)`) the old and new rect of every
  // unit that differs from the frame that last painted, and records this frame's
  // units as painted. Before any frame was painted, nothing is damaged: the
  // first frame paints the window whole.
  //
  // A surface damages at most kSeparateRectsPerSurface rects; past that, the
  // bounding rect of all of them. Scattered changes (a debugger step rewriting
  // rows apart, occurrence marks across the editor) otherwise add a clip each,
  // and the frame reaches the merged-clip count at which DirtyRegionPolicy
  // promotes it to a full-window repaint -- a full repaint of everything to
  // save repainting the rows between them.
  static constexpr std::size_t kSeparateRectsPerSurface = 2;

  template <typename Damage>
  void Commit(DerivedSurface surface, Damage&& damage) {
    Surface& table = Table(surface);
    util::AddPerformanceCounter(util::PerfCounterId::WorkspaceDerivedDamageUnits, table.current.size());
    if (table.painted_once) {
      RectCollector collected;
      const std::size_t count = std::max(table.current.size(), table.painted.size());
      for (std::size_t i = 0; i < count; ++i) {
        const Unit* now = i < table.current.size() ? &table.current[i] : nullptr;
        const Unit* was = i < table.painted.size() ? &table.painted[i] : nullptr;
        if (now != nullptr && was != nullptr && now->fingerprint == was->fingerprint &&
            SameRect(now->rect, was->rect)) {
          continue;
        }
        util::AddPerformanceCounter(util::PerfCounterId::WorkspaceDerivedDamageChanged, 1);
        if (was != nullptr && was->rect.w > 0.0f && was->rect.h > 0.0f) {
          collected.Add(was->rect);
        }
        if (now != nullptr && now->rect.w > 0.0f && now->rect.h > 0.0f &&
            (was == nullptr || !SameRect(now->rect, was->rect))) {
          collected.Add(now->rect);
        }
      }
      collected.EmitTo(damage);
    }
    table.painted.swap(table.current);
    table.painted_once = true;
  }

  // Forgets what was painted: the next frame paints whole anyway (a resize, a
  // theme change, a backend that lost its contents).
  void Reset() {
    for (Surface& table : surfaces_) {
      table.painted.clear();
      table.painted_once = false;
    }
  }

 private:
  // The rects one Commit damages: kept apart up to the cap, their bounding rect
  // past it. Fixed storage: a frame's commit allocates nothing.
  class RectCollector {
   public:
    void Add(const SDL_FRect& rect) {
      if (count_ < kSeparateRectsPerSurface) {
        separate_[count_] = rect;
      }
      ++count_;
      if (count_ == 1) {
        bounds_ = rect;
        return;
      }
      const float left = std::min(bounds_.x, rect.x);
      const float top = std::min(bounds_.y, rect.y);
      const float right = std::max(bounds_.x + bounds_.w, rect.x + rect.w);
      const float bottom = std::max(bounds_.y + bounds_.h, rect.y + rect.h);
      bounds_ = SDL_FRect{left, top, right - left, bottom - top};
    }
    template <typename Damage>
    void EmitTo(Damage& damage) const {
      if (count_ > kSeparateRectsPerSurface) {
        damage(bounds_);
        return;
      }
      for (std::size_t i = 0; i < count_; ++i) {
        damage(separate_[i]);
      }
    }

   private:
    std::array<SDL_FRect, kSeparateRectsPerSurface> separate_{};
    SDL_FRect bounds_{};
    std::size_t count_ = 0;
  };

  struct Surface {
    std::vector<Unit> current;
    std::vector<Unit> painted;
    bool painted_once = false;
  };
  static bool SameRect(const SDL_FRect& a, const SDL_FRect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
  }
  Surface& Table(DerivedSurface surface) { return surfaces_[static_cast<std::size_t>(surface)]; }
  std::array<Surface, static_cast<std::size_t>(DerivedSurface::Count)> surfaces_{};
};

}  // namespace microide::workspace
