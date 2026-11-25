#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>

#include "util/Fnv1a.h"
#include "util/PerformanceCounters.h"
#include "workspace/RectsEqual.h"

namespace microide::workspace {

enum class DerivedSurface : std::uint8_t {
  StatusBar,
  BottomPanelHeader,
  BottomPanelOutput,
  BottomPanelTerminalChrome,
  BottomPanelTerminalGrid,
  NotificationToasts,
  DebugPane,
  EditorPane0,
  EditorPane7 = EditorPane0 + 7,
  GitPanel,
  SidebarTree,
  SidebarList,
  SidebarSearch,
  SidebarModeRow,
  Chrome,
  MenuPopup,
  MenuSubmenu,
  MenuOverflowPopup,
  ContextMenu,
  Overlay,
  SettingsCard,
  Count,
};

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
  PaintFingerprint& Add(const char*) = delete;
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

  std::vector<Unit>& Begin(DerivedSurface surface) {
    std::vector<Unit>& units = Table(surface).current;
    units.clear();
    return units;
  }

  static constexpr std::size_t kSeparateRectsPerSurface = 2;

  enum class Coalesce { Bounding, Apart };

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
            RectsEqual(now->rect, was->rect)) {
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
        const bool had = was != nullptr && was->rect.w > 0.0f && was->rect.h > 0.0f;
        const bool has = now != nullptr && now->rect.w > 0.0f && now->rect.h > 0.0f;
        if (had && has && !RectsEqual(now->rect, was->rect)) {
          if (const std::optional<SDL_FRect> joined = ExactUnion(was->rect, now->rect); joined.has_value()) {
            add(*joined);
            continue;
          }
        }
        if (had) {
          add(was->rect);
        }
        if (has && (was == nullptr || !RectsEqual(now->rect, was->rect))) {
          add(now->rect);
        }
      }
      collected.EmitTo(damage);
    }
    table.painted.swap(table.current);
    table.painted_once = true;
  }


 private:
  static std::optional<SDL_FRect> ExactUnion(const SDL_FRect& a, const SDL_FRect& b) {
    const auto joins = [](float a0, float a1, float b0, float b1) { return a0 <= b1 && b0 <= a1; };
    if (a.y == b.y && a.h == b.h && joins(a.x, a.x + a.w, b.x, b.x + b.w)) {
      const float left = std::min(a.x, b.x);
      return SDL_FRect{left, a.y, std::max(a.x + a.w, b.x + b.w) - left, a.h};
    }
    if (a.x == b.x && a.w == b.w && joins(a.y, a.y + a.h, b.y, b.y + b.h)) {
      const float top = std::min(a.y, b.y);
      return SDL_FRect{a.x, top, a.w, std::max(a.y + a.h, b.y + b.h) - top};
    }
    return std::nullopt;
  }

  class RectCollector {
   public:
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
  Surface& Table(DerivedSurface surface) { return surfaces_[static_cast<std::size_t>(surface)]; }
  std::array<Surface, static_cast<std::size_t>(DerivedSurface::Count)> surfaces_{};
};

}
