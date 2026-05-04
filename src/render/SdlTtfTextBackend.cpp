#include "render/SdlTtfTextBackend.h"

#if MICROIDE_HAS_SDL3_TTF

#include <SDL3/SDL.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <unordered_set>
#include <utility>
#include <vector>

#include "platform/RuntimePaths.h"
#include "util/StartupTrace.h"

namespace microide::render {

namespace {

constexpr float kFontPointSize = 13.0f;
constexpr std::size_t kMaxCacheEntries = 2048;
constexpr float kMinPresentationScale = 0.1f;
}  // namespace

std::unique_ptr<SdlTtfTextBackend> SdlTtfTextBackend::Create(SDL_Renderer* renderer) {
  auto backend = std::unique_ptr<SdlTtfTextBackend>(new SdlTtfTextBackend());
  if (!backend->Initialize(renderer)) {
    return nullptr;
  }
  return backend;
}

SdlTtfTextBackend::~SdlTtfTextBackend() {
  ClearCache();
  CloseFonts();
  if (ttf_initialized_) {
    TTF_Quit();
    ttf_initialized_ = false;
  }
}

bool SdlTtfTextBackend::Initialize(SDL_Renderer* renderer) {
  util::StartupTrace::Scope trace_scope("SdlTtfTextBackend::Initialize");
  if (renderer == nullptr) {
    return false;
  }

  if (!TTF_Init()) {
    return false;
  }
  ttf_initialized_ = true;
  renderer_ = renderer;

  const auto font_path = LocateFontFile();
  if (font_path.empty()) {
    return false;
  }

  font_ = TTF_OpenFont(font_path.string().c_str(), kFontPointSize);
  if (font_ == nullptr) {
    return false;
  }
  font_path_ = font_path;
  TTF_SetFontHinting(font_, TTF_HINTING_LIGHT_SUBPIXEL);
  TTF_SetFontKerning(font_, false);
  LoadFallbackFonts();

  RefreshMetrics();
  return true;
}

void SdlTtfTextBackend::SetPresentationScale(float scale_x, float scale_y) {
  const float resolved_scale_x =
      std::isfinite(scale_x) && scale_x > 0.0f ? scale_x : 1.0f;
  const float resolved_scale_y =
      std::isfinite(scale_y) && scale_y > 0.0f ? scale_y : 1.0f;
  if (font_ == nullptr ||
      (std::fabs(presentation_scale_x_ - resolved_scale_x) < 0.001f &&
       std::fabs(presentation_scale_y_ - resolved_scale_y) < 0.001f)) {
    return;
  }

  const int hdpi = std::max(1, static_cast<int>(std::lround(
                                  72.0f * std::max(kMinPresentationScale, resolved_scale_x))));
  const int vdpi = std::max(1, static_cast<int>(std::lround(
                                  72.0f * std::max(kMinPresentationScale, resolved_scale_y))));
  if (!TTF_SetFontSizeDPI(font_, kFontPointSize, hdpi, vdpi)) {
    return;
  }
  for (TTF_Font* fallback_font : fallback_fonts_) {
    if (fallback_font != nullptr) {
      TTF_SetFontSizeDPI(fallback_font, kFontPointSize, hdpi, vdpi);
    }
  }

  presentation_scale_x_ = resolved_scale_x;
  presentation_scale_y_ = resolved_scale_y;
  ClearCache();
  RefreshMetrics();
}

void SdlTtfTextBackend::RefreshMetrics() {
  if (font_ == nullptr) {
    return;
  }

  const float scale_x = std::max(kMinPresentationScale, presentation_scale_x_);
  const float scale_y = std::max(kMinPresentationScale, presentation_scale_y_);
  const int font_height_pixels = TTF_GetFontHeight(font_);
  line_height_ = static_cast<float>(font_height_pixels) / scale_y;

  static constexpr std::string_view kAdvanceProbe = "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM";
  int probe_width = 0;
  int probe_height = 0;
  if (TTF_GetStringSize(font_, kAdvanceProbe.data(), kAdvanceProbe.size(), &probe_width,
                        &probe_height)) {
    char_width_ = static_cast<float>(probe_width) /
                  static_cast<float>(kAdvanceProbe.size()) / scale_x;
  }

  int max_left_padding_pixels = 0;
  int max_right_padding_pixels = 0;
  for (unsigned char ch = 0x20; ch <= 0x7E; ++ch) {
    int minx = 0;
    int maxx = 0;
    int miny = 0;
    int maxy = 0;
    int advance = 0;
    if (!TTF_GetGlyphMetrics(font_, ch, &minx, &maxx, &miny, &maxy, &advance)) {
      continue;
    }
    max_left_padding_pixels = std::max(max_left_padding_pixels, std::max(0, -minx));
    max_right_padding_pixels =
        std::max(max_right_padding_pixels, std::max(0, maxx - std::max(advance, 0)));
  }

  if (char_width_ <= 0.0f) {
    char_width_ = 8.0f;
  }
  if (line_height_ <= 0.0f) {
    line_height_ = 14.0f;
  }

  clip_padding_.left = static_cast<float>(max_left_padding_pixels) / scale_x;
  clip_padding_.right = static_cast<float>(max_right_padding_pixels) / scale_x;
  clip_padding_.top = 1.0f;
  clip_padding_.bottom = 1.0f;
}

float SdlTtfTextBackend::MeasureWidth(std::string_view text) const {
  if (font_ == nullptr || text.empty()) {
    return 0.0f;
  }

  if (CanUseFastAscii(text)) {
    return static_cast<float>(text.size()) * char_width_;
  }

  int width = 0;
  int height = 0;
  if (TTF_GetStringSize(font_, text.data(), text.size(), &width, &height)) {
    return static_cast<float>(width) / std::max(kMinPresentationScale, presentation_scale_x_);
  }

  return static_cast<float>(text.size()) * char_width_;
}

void SdlTtfTextBackend::DrawString(SDL_Renderer* renderer,
                                   float x,
                                   float y,
                                   SDL_Color color,
                                   std::string_view text) {
  if (renderer == nullptr || renderer != renderer_ || text.empty()) {
    return;
  }

  CacheEntry* entry = ResolveEntry(text, color, nullptr);
  if (entry == nullptr || entry->texture == nullptr) {
    return;
  }

  const SDL_FRect destination = SDL_FRect{
      std::round(x),
      std::round(y),
      static_cast<float>(entry->width) / std::max(kMinPresentationScale, presentation_scale_x_),
      static_cast<float>(entry->height) / std::max(kMinPresentationScale, presentation_scale_y_),
  };
  SDL_RenderTexture(renderer_, entry->texture, nullptr, &destination);
}

void SdlTtfTextBackend::DrawStringOn(SDL_Renderer* renderer,
                                     float x,
                                     float y,
                                     SDL_Color color,
                                     SDL_Color background,
                                     std::string_view text) {
  if (renderer == nullptr || renderer != renderer_ || text.empty()) {
    return;
  }

  CacheEntry* entry = ResolveEntry(text, color, &background);
  if (entry == nullptr || entry->texture == nullptr) {
    return;
  }

  const SDL_FRect destination = SDL_FRect{
      std::round(x),
      std::round(y),
      static_cast<float>(entry->width) / std::max(kMinPresentationScale, presentation_scale_x_),
      static_cast<float>(entry->height) / std::max(kMinPresentationScale, presentation_scale_y_),
  };
  SDL_RenderTexture(renderer_, entry->texture, nullptr, &destination);
}

void SdlTtfTextBackend::ClearCache() {
  for (auto& [_, entry] : cache_) {
    if (entry.texture != nullptr) {
      SDL_DestroyTexture(entry.texture);
      entry.texture = nullptr;
    }
  }
  cache_.clear();
  cache_order_.clear();
}

bool SdlTtfTextBackend::CanUseFastAscii(std::string_view text) const {
  if (text.empty()) {
    return false;
  }

  for (const unsigned char ch : text) {
    if (ch < 0x20 || ch > 0x7E) {
      return false;
    }
  }
  return true;
}

std::filesystem::path SdlTtfTextBackend::LocateFontFile() {
  static constexpr std::array<const char*, 5> kSystemCandidates = {
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
      "/usr/share/fonts/truetype/liberation2/LiberationMono-Regular.ttf",
      "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
      "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
      "/usr/share/fonts/opentype/urw-base35/NimbusMonoPS-Regular.otf",
  };

  const std::filesystem::path bundled_font =
      platform::ResolveBundledAssetPath("fonts/JetBrainsMono-Regular.ttf");
  if (!bundled_font.empty() && std::filesystem::exists(bundled_font)) {
    return bundled_font;
  }

  for (const char* candidate : kSystemCandidates) {
    const std::filesystem::path path(candidate);
    if (std::filesystem::exists(path)) {
      return path;
    }
  }
  return {};
}

std::vector<std::filesystem::path> SdlTtfTextBackend::LocateFallbackFontFiles(
    const std::filesystem::path& primary_font) {
  static constexpr std::array<const char*, 6> kFallbackCandidates = {
      "assets/fonts/JetBrainsMono-Regular.ttf",
      "/usr/share/fonts/truetype/noto/NotoSansSymbols-Regular.ttf",
      "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf",
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
  };

  std::vector<std::filesystem::path> candidates;
  std::unordered_set<std::string> seen;
  std::filesystem::path primary_resolved;
  if (!primary_font.empty()) {
    std::error_code primary_error;
    const std::filesystem::path normalized_primary =
        std::filesystem::weakly_canonical(primary_font, primary_error);
    primary_resolved =
        primary_error ? primary_font.lexically_normal() : normalized_primary;
  }
  const auto add_candidate = [&](const std::filesystem::path& candidate) {
    if (candidate.empty() || !std::filesystem::exists(candidate)) {
      return;
    }
    std::error_code candidate_error;
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(candidate, candidate_error);
    const std::filesystem::path resolved =
        candidate_error ? candidate.lexically_normal() : normalized;
    if (!primary_resolved.empty() && resolved == primary_resolved) {
      return;
    }
    const std::string key = resolved.string();
    if (!seen.insert(key).second) {
      return;
    }
    candidates.push_back(resolved);
  };

  add_candidate(platform::ResolveBundledAssetPath("fonts/JetBrainsMono-Regular.ttf"));

  for (const char* candidate : kFallbackCandidates) {
    add_candidate(candidate);
  }
  return candidates;
}

void SdlTtfTextBackend::CloseFonts() {
  if (font_ != nullptr) {
    TTF_ClearFallbackFonts(font_);
  }
  for (TTF_Font* fallback_font : fallback_fonts_) {
    if (fallback_font != nullptr) {
      TTF_CloseFont(fallback_font);
    }
  }
  fallback_fonts_.clear();
  if (font_ != nullptr) {
    TTF_CloseFont(font_);
    font_ = nullptr;
  }
}

void SdlTtfTextBackend::LoadFallbackFonts() {
  if (font_ == nullptr) {
    return;
  }

  for (const auto& fallback_path : LocateFallbackFontFiles(font_path_)) {
    TTF_Font* fallback_font = TTF_OpenFont(fallback_path.string().c_str(), kFontPointSize);
    if (fallback_font == nullptr) {
      continue;
    }
    TTF_SetFontHinting(fallback_font, TTF_HINTING_LIGHT_SUBPIXEL);
    TTF_SetFontKerning(fallback_font, false);
    if (!TTF_AddFallbackFont(font_, fallback_font)) {
      TTF_CloseFont(fallback_font);
      continue;
    }
    fallback_fonts_.push_back(fallback_font);
  }
}

std::size_t SdlTtfTextBackend::CacheKeyHash::operator()(const CacheKeyView& key) const noexcept {
  auto mix = [](std::size_t seed, std::size_t value) {
    return seed ^ (value + 0x9e3779b9ULL + (seed << 6) + (seed >> 2));
  };

  std::size_t h = std::hash<std::string_view>{}(key.text);
  const std::uint32_t packed_foreground =
      (static_cast<std::uint32_t>(key.color.r) << 24) |
      (static_cast<std::uint32_t>(key.color.g) << 16) |
      (static_cast<std::uint32_t>(key.color.b) << 8) |
      static_cast<std::uint32_t>(key.color.a);
  h = mix(h, packed_foreground);
  h = mix(h, key.has_background ? 1u : 0u);
  if (key.has_background) {
    const std::uint32_t packed_background =
        (static_cast<std::uint32_t>(key.background.r) << 24) |
        (static_cast<std::uint32_t>(key.background.g) << 16) |
        (static_cast<std::uint32_t>(key.background.b) << 8) |
        static_cast<std::uint32_t>(key.background.a);
    h = mix(h, packed_background);
  }
  return h;
}

bool SdlTtfTextBackend::CacheKeyEqual::operator()(const CacheKeyView& lhs,
                                                  const CacheKeyView& rhs) const noexcept {
  return lhs.text == rhs.text && lhs.color.r == rhs.color.r && lhs.color.g == rhs.color.g &&
         lhs.color.b == rhs.color.b && lhs.color.a == rhs.color.a &&
         lhs.has_background == rhs.has_background &&
         (!lhs.has_background ||
          (lhs.background.r == rhs.background.r && lhs.background.g == rhs.background.g &&
           lhs.background.b == rhs.background.b && lhs.background.a == rhs.background.a));
}

SdlTtfTextBackend::CacheEntry* SdlTtfTextBackend::ResolveEntry(std::string_view text,
                                                               SDL_Color color,
                                                               const SDL_Color* background) {
  const CacheKeyView key_view{
      .text = text,
      .color = color,
      .has_background = background != nullptr,
      .background = background == nullptr ? SDL_Color{} : *background,
  };
  if (auto it = cache_.find(key_view); it != cache_.end()) {
    cache_order_.splice(cache_order_.end(), cache_order_, it->second.order);
    return &it->second;
  }

  SDL_Surface* surface =
      background == nullptr
          ? TTF_RenderText_Blended(font_, text.data(), text.size(), color)
          : TTF_RenderText_LCD(font_, text.data(), text.size(), color, *background);
  if (surface == nullptr) {
    return nullptr;
  }

  SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
  if (texture == nullptr) {
    SDL_DestroySurface(surface);
    return nullptr;
  }
  SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);

  CacheEntry entry;
  entry.texture = texture;
  entry.width = surface->w;
  entry.height = surface->h;
  SDL_DestroySurface(surface);

  CacheKey key{
      .text = std::string(text),
      .color = color,
      .has_background = background != nullptr,
      .background = background == nullptr ? SDL_Color{} : *background,
  };
  auto [map_it, inserted] = cache_.emplace(std::move(key), std::move(entry));
  if (!inserted) {
    if (map_it->second.texture != nullptr) {
      SDL_DestroyTexture(map_it->second.texture);
    }
    map_it->second = std::move(entry);
  }
  cache_order_.push_back(map_it->first);
  map_it->second.order = std::prev(cache_order_.end());

  while (cache_order_.size() > kMaxCacheEntries) {
    auto evict_it = cache_.find(cache_order_.front());
    cache_order_.pop_front();
    if (evict_it != cache_.end()) {
      if (evict_it->second.texture != nullptr) {
        SDL_DestroyTexture(evict_it->second.texture);
      }
      cache_.erase(evict_it);
    }
  }

  return &map_it->second;
}

}  // namespace microide::render

#endif  // MICROIDE_HAS_SDL3_TTF
