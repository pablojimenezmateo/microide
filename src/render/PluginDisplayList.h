#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

namespace microide::render {

// Flat, POD op buffer that a plugin emits to describe a custom content surface
// (REST response view, chart, table, etc.). It is intentionally NOT a tree so it
// is cheap to ship from Lua and replay with zero allocation. The host owns all
// drawing: PluginDisplayListRenderer replays this buffer reusing TextRenderer +
// fill/line primitives. Plugins never touch SDL.
//
// Coordinates are content-local (origin 0,0 at the surface's top-left); the host
// translates by the surface origin and clips to the surface rect at replay time.
enum class DrawOp : std::uint8_t {
  Rect,       // filled rectangle: rect = bounds, color
  Line,       // single line: rect packs (x1,y1,x2,y2), color
  Polyline,   // connected segments: point_arena[data_offset .. +data_count], color
  Text,       // text_arena.substr(data_offset, data_count) at (rect.x, rect.y), color
  Image,      // raster blit: image_hashes[data_offset] into rect bounds
  ClipPush,   // intersect the clip stack with rect (content-local)
  ClipPop,    // pop the last ClipPush
};

// One draw operation. 32 bytes; trivially copyable. The meaning of `rect`,
// `data_offset`, and `data_count` is op-dependent (see DrawOp above).
struct DisplayOp {
  DrawOp op = DrawOp::Rect;
  SDL_FRect rect{0.0f, 0.0f, 0.0f, 0.0f};
  SDL_Color color{0, 0, 0, 0};
  std::uint32_t data_offset = 0;
  std::uint32_t data_count = 0;

  // SDL_FRect / SDL_Color have no operator==, so compare fields explicitly.
  bool operator==(const DisplayOp& o) const {
    return op == o.op && rect.x == o.rect.x && rect.y == o.rect.y && rect.w == o.rect.w &&
           rect.h == o.rect.h && color.r == o.color.r && color.g == o.color.g &&
           color.b == o.color.b && color.a == o.color.a && data_offset == o.data_offset &&
           data_count == o.data_count;
  }
};

// Hard caps so a malformed or hostile plugin cannot force an unbounded allocation
// or a pathological replay. Inputs above these are rejected, never clamped.
inline constexpr std::size_t kMaxDisplayOps = 1u << 16;        // 65,536 ops
inline constexpr std::size_t kMaxDisplayPoints = 1u << 18;     // 262,144 points
inline constexpr std::size_t kMaxDisplayTextBytes = 1u << 20;  // 1 MiB of text
inline constexpr std::size_t kMaxDisplayImages = 256;

// A complete, self-contained display list. Text ops slice `string_view`s out of
// `text_arena` so the replay path never materializes a string (keeps the
// render-TU no-allocation lint happy). `content_width/height` are the intrinsic
// size the host uses for panel scroll extent and inline-inset gap height.
struct PluginDisplayList {
  std::vector<DisplayOp> ops;
  std::string text_arena;
  std::vector<SDL_FPoint> point_arena;
  std::vector<std::uint64_t> image_hashes;
  float content_width = 0.0f;
  float content_height = 0.0f;
  std::uint64_t content_hash = 0;

  bool empty() const { return ops.empty(); }

  // Equality is by content hash + intrinsic size: the hash folds in every op and
  // arena byte, so this is a cheap, complete change check for the store's no-op
  // detection (and avoids comparing SDL_FPoint, which has no operator==).
  bool operator==(const PluginDisplayList& o) const {
    return content_hash == o.content_hash && content_width == o.content_width &&
           content_height == o.content_height && ops.size() == o.ops.size();
  }
};

// True when every op's data references stay inside the arenas and the buffer is
// within the size caps. The host validates exactly once (at publish) so replay
// can trust the buffer and skip per-op bounds checks. Returns false (with an
// optional reason in *error) on any violation; never allocates on the failure
// path beyond the error string.
bool ValidateDisplayList(const PluginDisplayList& list, std::string* error);

// Stable FNV-1a hash over the ops + arenas + intrinsic size. Used for caching and
// change detection (a republish with an identical hash is a no-op). Does not
// depend on the order plugins built the buffer in any way other than op order.
std::uint64_t ComputeDisplayListHash(const PluginDisplayList& list);

}  // namespace microide::render
