#include "render/PluginDisplayList.h"

#include <cmath>
#include <cstring>

namespace microide::render {

namespace {

void HashBytes(std::uint64_t& h, const void* data, std::size_t size) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < size; ++i) {
    h ^= static_cast<std::uint64_t>(bytes[i]);
    h *= 0x100000001b3ULL;  // FNV-1a prime
  }
}

bool RectIsFinite(const SDL_FRect& rect) {
  return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.w) &&
         std::isfinite(rect.h);
}

}  // namespace

bool ValidateDisplayList(const PluginDisplayList& list, std::string* error) {
  const auto fail = [&](const char* message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };

  if (list.ops.size() > kMaxDisplayOps) {
    return fail("display list exceeds the maximum op count");
  }
  if (list.point_arena.size() > kMaxDisplayPoints) {
    return fail("display list exceeds the maximum polyline point count");
  }
  if (list.text_arena.size() > kMaxDisplayTextBytes) {
    return fail("display list exceeds the maximum text size");
  }
  if (list.image_hashes.size() > kMaxDisplayImages) {
    return fail("display list exceeds the maximum image count");
  }

  // Plugin-supplied coordinates flow straight from Lua with no finiteness check.
  // Replay casts them to int (e.g. ToIntRect for clip rects), and static_cast<int>
  // of a NaN/±inf or out-of-int-range float is undefined behavior (UBSAN trap; a
  // garbage clip rect in release). Reject non-finite geometry here — validation is
  // the sole gate replay trusts.
  for (const SDL_FPoint& point : list.point_arena) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
      return fail("display list has a non-finite polyline point");
    }
  }

  // Content dimensions feed the host's scroll extents / intrinsic layout size, so
  // a NaN/±inf or negative value poisons layout math even when every op is valid.
  // Validation is the sole gate the host trusts, so reject it here.
  if (!std::isfinite(list.content_width) || !std::isfinite(list.content_height) ||
      list.content_width < 0.0f || list.content_height < 0.0f) {
    return fail("display list has a non-finite or negative content dimension");
  }

  int clip_depth = 0;
  for (const DisplayOp& op : list.ops) {
    if (!RectIsFinite(op.rect)) {
      return fail("display list op has a non-finite rectangle");
    }
    switch (op.op) {
      case DrawOp::Text: {
        // The slice must fit the arena. Even an empty (count == 0) op must have an
        // in-bounds offset: replay forms `text_arena.data() + data_offset`, and a
        // past-the-end offset is UB (invalid pointer formation) regardless of
        // count. Validation is the sole gate replay trusts, so it must hold here.
        const std::uint64_t end =
            static_cast<std::uint64_t>(op.data_offset) + op.data_count;
        if (end > list.text_arena.size()) {
          return fail("display list text op references outside the text arena");
        }
        break;
      }
      case DrawOp::Polyline: {
        if (op.data_count < 2) {
          return fail("display list polyline op needs at least two points");
        }
        const std::uint64_t end =
            static_cast<std::uint64_t>(op.data_offset) + op.data_count;
        if (end > list.point_arena.size()) {
          return fail("display list polyline op references outside the point arena");
        }
        break;
      }
      case DrawOp::Image:
        if (op.data_offset >= list.image_hashes.size()) {
          return fail("display list image op references an unknown image handle");
        }
        break;
      case DrawOp::ClipPush:
        ++clip_depth;
        break;
      case DrawOp::ClipPop:
        if (clip_depth == 0) {
          return fail("display list has an unbalanced clip pop");
        }
        --clip_depth;
        break;
      case DrawOp::Rect:
      case DrawOp::Line:
        break;
    }
  }
  if (clip_depth != 0) {
    return fail("display list has an unbalanced clip push");
  }
  return true;
}

std::uint64_t ComputeDisplayListHash(const PluginDisplayList& list) {
  std::uint64_t h = 0xcbf29ce484222325ULL;  // FNV-1a offset basis
  for (const DisplayOp& op : list.ops) {
    const std::uint8_t op_byte = static_cast<std::uint8_t>(op.op);
    HashBytes(h, &op_byte, sizeof(op_byte));
    HashBytes(h, &op.rect, sizeof(op.rect));
    HashBytes(h, &op.color, sizeof(op.color));
    HashBytes(h, &op.data_offset, sizeof(op.data_offset));
    HashBytes(h, &op.data_count, sizeof(op.data_count));
  }
  HashBytes(h, list.text_arena.data(), list.text_arena.size());
  if (!list.point_arena.empty()) {
    HashBytes(h, list.point_arena.data(), list.point_arena.size() * sizeof(SDL_FPoint));
  }
  if (!list.image_hashes.empty()) {
    HashBytes(h, list.image_hashes.data(),
              list.image_hashes.size() * sizeof(std::uint64_t));
  }
  HashBytes(h, &list.content_width, sizeof(list.content_width));
  HashBytes(h, &list.content_height, sizeof(list.content_height));
  return h;
}

}  // namespace microide::render
