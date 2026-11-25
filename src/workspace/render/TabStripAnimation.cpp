#include "workspace/render/TabStripAnimation.h"

#include <cmath>

namespace microide::workspace {

namespace {

constexpr float kSlideTauMs = 45.0f;
constexpr float kSlideSnapPx = 0.5f;

}

std::vector<float> ComputeSlideTargetXs(std::span<const SlideTab> tabs,
                                        std::size_t source_index,
                                        std::size_t insertion_slot,
                                        float ghost_width,
                                        float gap) {
  std::vector<float> targets(tabs.size(), 0.0f);
  if (tabs.empty()) {
    return targets;
  }

  const float start_x = tabs.front().x;
  const float gap_span = ghost_width + gap;
  float running_x = start_x;
  bool gap_opened = false;
  for (std::size_t i = 0; i < tabs.size(); ++i) {
    const SlideTab& tab = tabs[i];
    if (tab.index == source_index) {
      targets[i] = tab.x;
      continue;
    }
    if (!gap_opened && tab.index >= insertion_slot) {
      running_x += gap_span;
      gap_opened = true;
    }
    targets[i] = running_x;
    running_x += tab.width + gap;
  }
  return targets;
}

bool SlideOffsetsMoving(const std::vector<float>& current,
                        const std::vector<float>& target) {
  if (current.size() != target.size()) {
    return false;
  }
  for (std::size_t i = 0; i < current.size(); ++i) {
    if (std::fabs(target[i] - current[i]) > kSlideSnapPx) {
      return true;
    }
  }
  return false;
}

bool AdvanceSlideOffsets(std::vector<float>& current,
                         const std::vector<float>& target,
                         float dt_ms) {
  if (current.size() != target.size() || current.empty()) {
    return false;
  }
  const float alpha = 1.0f - std::exp(-std::fmax(0.0f, dt_ms) / kSlideTauMs);
  bool changed = false;
  for (std::size_t i = 0; i < current.size(); ++i) {
    const float delta = target[i] - current[i];
    if (std::fabs(delta) <= kSlideSnapPx) {
      changed = changed || current[i] != target[i];
      current[i] = target[i];
      continue;
    }
    current[i] += delta * alpha;
    changed = true;
  }
  return changed;
}

}
