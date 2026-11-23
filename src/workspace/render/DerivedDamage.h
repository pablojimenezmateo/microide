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
  // The terminal's grid: each pane's body, then a unit per visible row.
  BottomPanelTerminalGrid,
  NotificationToasts,
  DebugPane,
  // One per editor group (kMaxEditorGroups): each pane's visible rows.
  EditorPane0,
  EditorPane7 = EditorPane0 + 7,
  GitPanel,
  // The sidebar's file tree: its header buttons, visible rows and scrollbar.
  SidebarTree,
  // The sidebar's mode tab row, shared by every sidebar view.
  SidebarModeRow,
  // The window's chrome a frame decides as a whole: its regions, the caret, the
  // floating cards and popups (each a unit at a fixed position; an absent one
  // keeps its position with an empty rect).
  Chrome,
  // The open menus, one surface each (MenuPopupPaint.h): the menu bar's popup, its
  // submenu, the compact layout's overflow popup and the context menu. A popup's
  // card is its first unit, then a unit per row.
  MenuPopup,
  MenuSubmenu,
  MenuOverflowPopup,
  ContextMenu,
  // The overlay card (palette, file finder, pickers, completion) or the in-file
  // find widget: the card, then a unit per visible row, then its scrollbar.
  Overlay,
  // The Settings / Help & About card (SettingsCardPaint.h): its backdrop, its
  // chrome, the scope tooltip, the font picker, then a unit per rail and value row.
  SettingsCard,
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
  // Every surface commits on every prepared frame, showing or not (a hidden one
  // with no units), so its first commit is the app's first frame. A surface that
  // skipped Commit while hidden would keep units it no longer paints and damage
  // nothing when it came back. Nothing forgets what was painted either: a full
  // repaint (a resize, a theme change, a lost backbuffer) paints over the state
  // the tables describe, so they stay right across it.
  //
  // A surface damages at most kSeparateRectsPerSurface rects; past that, the
  // nearest ones merge (RectCollector). Scattered changes (a debugger step rewriting
  // rows apart, occurrence marks across the editor) otherwise add a clip each,
  // and the frame reaches the merged-clip count at which DirtyRegionPolicy
  // promotes it to a full-window repaint -- a full repaint of everything to
  // save repainting the rows between them.
  static constexpr std::size_t kSeparateRectsPerSurface = 2;

  // Rects that change apart, for a surface whose units are unrelated pieces of
  // the window (DerivedSurface::Chrome: the caret, a popup, the title bar): their
  // bounding rect would be most of the window. DirtyRegionPolicy still merges or
  // promotes the frame's clips as a whole.
  enum class Coalesce { Bounding, Apart };

  // How many units with a rect a surface laid out for the frame last prepared:
  // for tests that a surface said to be derived is.
  std::size_t LaidOutUnitCount(DerivedSurface surface) const {
    const std::vector<Unit>& painted = surfaces_[static_cast<std::size_t>(surface)].painted;
    return static_cast<std::size_t>(
        std::count_if(painted.begin(), painted.end(), [](const Unit& unit) { return unit.rect.w > 0.0f && unit.rect.h > 0.0f; }));
  }

  template <typename Damage>
  void Commit(DerivedSurface surface, Damage&& damage, Coalesce coalesce = Coalesce::Bounding) {
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
        const auto add = [&](const SDL_FRect& rect) {
          if (coalesce == Coalesce::Apart) {
            damage(rect);
          } else {
            collected.Add(rect);
          }
        };
        if (was != nullptr && was->rect.w > 0.0f && was->rect.h > 0.0f) {
          add(was->rect);
        }
        if (now != nullptr && now->rect.w > 0.0f && now->rect.h > 0.0f &&
            (was == nullptr || !SameRect(now->rect, was->rect))) {
          add(now->rect);
        }
      }
      collected.EmitTo(damage);
    }
    table.painted.swap(table.current);
    table.painted_once = true;
  }


 private:
  // The rects one Commit damages: kept apart up to the cap, their bounding rect
  // past it. Fixed storage: a frame's commit allocates nothing.
  class RectCollector {
   public:
    // Keeps at most kSeparateRectsPerSurface rects: one more joins the pair whose
    // union adds the least area, so nearby changes merge and far ones stay apart
    // (a header and two bands: the header joins the nearer band).
    void Add(const SDL_FRect& rect) {
      rects_[count_++] = rect;
      if (count_ <= kSeparateRectsPerSurface) {
        return;
      }
      std::size_t best_a = 0;
      std::size_t best_b = 1;
      float best_growth = 0.0f;
      for (std::size_t a = 0; a < count_; ++a) {
        for (std::size_t b = a + 1; b < count_; ++b) {
          const float growth = Area(Union(rects_[a], rects_[b])) - Area(rects_[a]) - Area(rects_[b]);
          if ((a == 0 && b == 1) || growth < best_growth) {
            best_growth = growth;
            best_a = a;
            best_b = b;
          }
        }
      }
      rects_[best_a] = Union(rects_[best_a], rects_[best_b]);
      rects_[best_b] = rects_[--count_];
    }
    template <typename Damage>
    void EmitTo(Damage& damage) const {
      for (std::size_t i = 0; i < count_; ++i) {
        damage(rects_[i]);
      }
    }

   private:
    static float Area(const SDL_FRect& r) { return r.w * r.h; }
    static SDL_FRect Union(const SDL_FRect& a, const SDL_FRect& b) {
      const float left = std::min(a.x, b.x);
      const float top = std::min(a.y, b.y);
      const float right = std::max(a.x + a.w, b.x + b.w);
      const float bottom = std::max(a.y + a.h, b.y + b.h);
      return SDL_FRect{left, top, right - left, bottom - top};
    }
    std::array<SDL_FRect, kSeparateRectsPerSurface + 1> rects_{};
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
