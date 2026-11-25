#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace microide::render {
struct Theme;
}

namespace microide::workspace::overview {

inline constexpr float kLaneWidth = 6.0f;
inline constexpr float kLaneGap = 3.0f;
inline constexpr float kMinMarkerHeight = 2.0f;

struct MarkerInput {
  int start_row = 0;
  int end_row = 0;
  SDL_Color color{};
  int priority = 0;
};

struct Marker {
  SDL_FRect rect{};
  SDL_Color color{};
};

SDL_FRect LaneRect(const SDL_FRect& track,
                   float min_x = -std::numeric_limits<float>::infinity());
SDL_FRect LaneInnerRect(const SDL_FRect& lane);

bool BuildMarker(const SDL_FRect& inner_lane, std::size_t total_rows, const MarkerInput& input,
                 Marker& out);

std::vector<Marker> BuildMarkers(const SDL_FRect& inner_lane, std::size_t total_rows,
                                 std::span<const MarkerInput> inputs);
void BuildMarkersInto(const SDL_FRect& inner_lane, std::size_t total_rows,
                      std::span<const MarkerInput> inputs, std::vector<Marker>& out);

void ReduceMarkersInto(const SDL_FRect& inner_lane, std::size_t total_rows,
                       std::span<const MarkerInput> inputs,
                       std::vector<std::uint32_t>& bucket_scratch,
                       std::vector<SDL_Color>& palette_scratch, std::vector<Marker>& out);
std::vector<Marker> ReduceMarkers(const SDL_FRect& inner_lane, std::size_t total_rows,
                                  std::span<const MarkerInput> inputs,
                                  std::vector<std::uint32_t>& bucket_scratch,
                                  std::vector<SDL_Color>& palette_scratch);

void DrawLane(SDL_Renderer* renderer, const render::Theme& theme, const SDL_FRect& lane,
              std::span<const Marker> markers);

std::uint64_t ThemeMarkerToken(const render::Theme& theme);

}
