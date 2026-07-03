#include "render/PluginDisplayList.h"

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

  int clip_depth = 0;
  for (const DisplayOp& op : list.ops) {
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
