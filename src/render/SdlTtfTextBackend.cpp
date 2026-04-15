#include "render/SdlTtfTextBackend.h"

#if MICROIDE_HAS_SDL3_TTF

#include <SDL3/SDL.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <unordered_set>
#include <utility>
#include <vector>

#include "util/StartupTrace.h"

namespace microide::render {

namespace {

constexpr float kFontPointSize = 13.0f;
constexpr std::size_t kMaxCacheEntries = 512;
constexpr float kMinPresentationScale = 0.1f;
constexpr SDL_Color kWhite = SDL_Color{0xff, 0xff, 0xff, 0xff};

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

  font_ascent_pixels_ = static_cast<float>(TTF_GetFontAscent(font_));
  line_height_ =
      static_cast<float>(TTF_GetFontHeight(font_)) / std::max(kMinPresentationScale, presentation_scale_y_);
  static constexpr std::string_view kAdvanceProbe = "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM";
  int probe_width = 0;
  int probe_height = 0;
  if (TTF_GetStringSize(font_, kAdvanceProbe.data(), kAdvanceProbe.size(), &probe_width,
                        &probe_height)) {
    char_width_ = static_cast<float>(probe_width) /
                  static_cast<float>(kAdvanceProbe.size()) /
                  std::max(kMinPresentationScale, presentation_scale_x_);
  }

  if (char_width_ <= 0.0f) {
    char_width_ = 8.0f;
  }
  if (line_height_ <= 0.0f) {
    line_height_ = 14.0f;
  }
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

  if (CanUseFastAscii(text)) {
    DrawFastAsciiString(renderer, x, y, color, nullptr, text);
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

  if (CanUseFastAscii(text)) {
    DrawFastAsciiString(renderer, x, y, color, &background, text);
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
  ClearGlyphCache();
}

void SdlTtfTextBackend::ClearGlyphCache() {
  for (GlyphEntry& entry : glyph_cache_) {
    if (entry.texture != nullptr) {
      SDL_DestroyTexture(entry.texture);
    }
    entry = GlyphEntry{};
  }
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

void SdlTtfTextBackend::DrawFastAsciiString(SDL_Renderer* renderer,
                                            float x,
                                            float y,
                                            SDL_Color color,
                                            const SDL_Color* background,
                                            std::string_view text) {
  if (renderer == nullptr || text.empty()) {
    return;
  }

  if (background != nullptr) {
    const SDL_FRect background_rect =
        SDL_FRect{x, y, static_cast<float>(text.size()) * char_width_, line_height_};
    SDL_SetRenderDrawColor(renderer, background->r, background->g, background->b, background->a);
    SDL_RenderFillRect(renderer, &background_rect);
  }

  float cursor_x = x;
  const float scale_x = std::max(kMinPresentationScale, presentation_scale_x_);
  const float scale_y = std::max(kMinPresentationScale, presentation_scale_y_);
  for (const unsigned char ch : text) {
    GlyphEntry* glyph = ResolveGlyph(ch);
    if (glyph != nullptr && glyph->texture != nullptr) {
      SDL_SetTextureColorMod(glyph->texture, color.r, color.g, color.b);
      SDL_SetTextureAlphaMod(glyph->texture, color.a);
      const SDL_FRect destination = SDL_FRect{
          std::round(cursor_x + static_cast<float>(glyph->minx) / scale_x),
          std::round(y + (font_ascent_pixels_ - static_cast<float>(glyph->maxy)) / scale_y),
          static_cast<float>(glyph->width) / scale_x,
          static_cast<float>(glyph->height) / scale_y,
      };
      SDL_RenderTexture(renderer, glyph->texture, nullptr, &destination);
    }
    cursor_x += char_width_;
  }
}

SdlTtfTextBackend::GlyphEntry* SdlTtfTextBackend::ResolveGlyph(unsigned char ch) {
  if (ch >= glyph_cache_.size()) {
    return nullptr;
  }

  GlyphEntry& entry = glyph_cache_[ch];
  if (entry.loaded) {
    return &entry;
  }

  entry.loaded = true;
  if (font_ == nullptr || ch == ' ') {
    return &entry;
  }

  int minx = 0;
  int maxy = 0;
  if (!TTF_GetGlyphMetrics(font_, ch, &minx, nullptr, nullptr, &maxy, nullptr)) {
    return &entry;
  }

  SDL_Surface* surface = TTF_RenderGlyph_Blended(font_, ch, kWhite);
  if (surface == nullptr) {
    return &entry;
  }

  SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
  if (texture == nullptr) {
    SDL_DestroySurface(surface);
    return &entry;
  }
  SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);

  entry.texture = texture;
  entry.width = surface->w;
  entry.height = surface->h;
  entry.minx = minx;
  entry.maxy = maxy;
  SDL_DestroySurface(surface);
  return &entry;
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

  const std::filesystem::path base_path = BasePath();
  if (!base_path.empty()) {
    add_candidate(base_path / "assets" / "fonts" / "JetBrainsMono-Regular.ttf");
    add_candidate(base_path / ".." / "assets" / "fonts" / "JetBrainsMono-Regular.ttf");
    add_candidate(base_path / ".." / ".." / "assets" / "fonts" / "JetBrainsMono-Regular.ttf");
  }

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
    if (!TTF_AddFallbackFont(font_, fallback_font)) {
      TTF_CloseFont(fallback_font);
      continue;
    }
    fallback_fonts_.push_back(fallback_font);
  }
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
