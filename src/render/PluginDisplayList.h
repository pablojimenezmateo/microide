#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <vector>

namespace microide::render {

enum class DrawOp : std::uint8_t {
  Rect,
  Line,
  Polyline,
  Text,
  Image,
  ClipPush,
  ClipPop,
};

struct DisplayOp {
  DrawOp op = DrawOp::Rect;
  SDL_FRect rect{0.0f, 0.0f, 0.0f, 0.0f};
  SDL_Color color{0, 0, 0, 0};
  std::uint32_t data_offset = 0;
  std::uint32_t data_count = 0;

  bool operator==(const DisplayOp& o) const {
    return op == o.op && rect.x == o.rect.x && rect.y == o.rect.y && rect.w == o.rect.w &&
           rect.h == o.rect.h && color.r == o.color.r && color.g == o.color.g &&
           color.b == o.color.b && color.a == o.color.a && data_offset == o.data_offset &&
           data_count == o.data_count;
  }
};

inline constexpr std::size_t kMaxDisplayOps = 1u << 16;
inline constexpr std::size_t kMaxDisplayPoints = 1u << 18;
inline constexpr std::size_t kMaxDisplayTextBytes = 1u << 20;
inline constexpr std::size_t kMaxDisplayImages = 256;

struct PluginDisplayList {
  std::vector<DisplayOp> ops;
  std::string text_arena;
  std::vector<SDL_FPoint> point_arena;
  std::vector<std::uint64_t> image_hashes;
  float content_width = 0.0f;
  float content_height = 0.0f;
  std::uint64_t content_hash = 0;

  bool empty() const { return ops.empty(); }

  bool operator==(const PluginDisplayList& o) const {
    return content_hash == o.content_hash && content_width == o.content_width &&
           content_height == o.content_height && ops.size() == o.ops.size();
  }
};

bool ValidateDisplayList(const PluginDisplayList& list, std::string* error);

std::uint64_t ComputeDisplayListHash(const PluginDisplayList& list);

}
