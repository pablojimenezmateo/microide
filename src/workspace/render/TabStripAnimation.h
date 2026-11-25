#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

namespace microide::workspace {

inline float ClampedGhostX(float strip_x,
                           float strip_w,
                           float ghost_width,
                           float pointer_x,
                           float grab_offset_x) {
  const float max_x = std::max(strip_x, strip_x + strip_w - ghost_width);
  return std::clamp(pointer_x - grab_offset_x, strip_x, max_x);
}

struct SlideTab {
  std::size_t index = 0;
  float x = 0.0f;
  float width = 0.0f;
};

std::vector<float> ComputeSlideTargetXs(std::span<const SlideTab> tabs,
                                        std::size_t source_index,
                                        std::size_t insertion_slot,
                                        float ghost_width,
                                        float gap);

bool AdvanceSlideOffsets(std::vector<float>& current,
                         const std::vector<float>& target,
                         float dt_ms);

bool SlideOffsetsMoving(const std::vector<float>& current,
                        const std::vector<float>& target);

}
