#include "render/SdlTtfTextBackend.h"

#if MICROIDE_HAS_SDL3_TTF

#include <SDL3/SDL.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <utility>
#include <vector>

#include "util/StartupTrace.h"

namespace microide::render {

namespace {

constexpr float kFontPointSize = 13.0f;
constexpr std::size_t kMaxCacheEntries = 512;

std::filesystem::path BasePath() {
  const char* raw_base_path = SDL_GetBasePath();
  if (raw_base_path == nullptr || raw_base_path[0] == '\0') {
    return {};
  }
  return std::filesystem::path(raw_base_path);
}

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
  if (font_ != nullptr) {
    TTF_CloseFont(font_);
    font_ = nullptr;
  }
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
  TTF_SetFontHinting(font_, TTF_HINTING_LIGHT_SUBPIXEL);

  line_height_ = static_cast<float>(TTF_GetFontHeight(font_));
  static constexpr std::string_view kAdvanceProbe = "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM";
  int probe_width = 0;
  int probe_height = 0;
  if (TTF_GetStringSize(font_, kAdvanceProbe.data(), kAdvanceProbe.size(), &probe_width,
                        &probe_height)) {
    char_width_ = static_cast<float>(probe_width) /
                  static_cast<float>(kAdvanceProbe.size());
  }

  if (char_width_ <= 0.0f) {
    char_width_ = 8.0f;
  }
  if (line_height_ <= 0.0f) {
    line_height_ = 14.0f;
  }
  return true;
}

float SdlTtfTextBackend::MeasureWidth(std::string_view text) const {
  if (font_ == nullptr || text.empty()) {
    return 0.0f;
  }

  int width = 0;
  int height = 0;
  if (TTF_GetStringSize(font_, text.data(), text.size(), &width, &height)) {
    return static_cast<float>(width);
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
      static_cast<float>(entry->width),
      static_cast<float>(entry->height),
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
      static_cast<float>(entry->width),
      static_cast<float>(entry->height),
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

std::filesystem::path SdlTtfTextBackend::LocateFontFile() {
  static constexpr std::array<const char*, 5> kSystemCandidates = {
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
      "/usr/share/fonts/truetype/liberation2/LiberationMono-Regular.ttf",
      "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
      "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
      "/usr/share/fonts/opentype/urw-base35/NimbusMonoPS-Regular.otf",
  };

  const std::filesystem::path bundled_font = std::filesystem::path("assets/fonts/JetBrainsMono-Regular.ttf");
  if (std::filesystem::exists(bundled_font)) {
    return bundled_font;
  }

  const std::filesystem::path base_path = BasePath();
  if (!base_path.empty()) {
    const std::vector<std::filesystem::path> relative_candidates = {
        base_path / "assets" / "fonts" / "JetBrainsMono-Regular.ttf",
        base_path / ".." / "assets" / "fonts" / "JetBrainsMono-Regular.ttf",
        base_path / ".." / ".." / "assets" / "fonts" / "JetBrainsMono-Regular.ttf",
    };

    for (const auto& candidate : relative_candidates) {
      if (std::filesystem::exists(candidate)) {
        return candidate.lexically_normal();
      }
    }
  }

  for (const char* candidate : kSystemCandidates) {
    const std::filesystem::path path(candidate);
    if (std::filesystem::exists(path)) {
      return path;
    }
  }
  return {};
}

SdlTtfTextBackend::CacheEntry* SdlTtfTextBackend::ResolveEntry(std::string_view text,
                                                               SDL_Color color,
                                                               const SDL_Color* background) {
  const std::string key = BuildCacheKey(text, color, background);
  if (auto it = cache_.find(key); it != cache_.end()) {
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

  cache_order_.push_back(key);
  cache_.insert_or_assign(key, entry);
  while (cache_order_.size() > kMaxCacheEntries) {
    const std::string old_key = cache_order_.front();
    cache_order_.pop_front();
    auto it = cache_.find(old_key);
    if (it != cache_.end()) {
      if (it->second.texture != nullptr) {
        SDL_DestroyTexture(it->second.texture);
      }
      cache_.erase(it);
    }
  }

  auto it = cache_.find(key);
  return it == cache_.end() ? nullptr : &it->second;
}

std::string SdlTtfTextBackend::BuildCacheKey(std::string_view text,
                                             SDL_Color color,
                                             const SDL_Color* background) const {
  std::string key(text);
  key.push_back('\n');
  key += std::to_string(color.r);
  key.push_back(',');
  key += std::to_string(color.g);
  key.push_back(',');
  key += std::to_string(color.b);
  key.push_back(',');
  key += std::to_string(color.a);
  key.push_back('\n');
  if (background == nullptr) {
    key += "none";
    return key;
  }
  key += std::to_string(background->r);
  key.push_back(',');
  key += std::to_string(background->g);
  key.push_back(',');
  key += std::to_string(background->b);
  key.push_back(',');
  key += std::to_string(background->a);
  return key;
}

}  // namespace microide::render

#endif  // MICROIDE_HAS_SDL3_TTF
