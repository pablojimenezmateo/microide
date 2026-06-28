#include "render/SdlTtfTextBackend.h"

#if MICROIDE_HAS_SDL3_TTF

#include <SDL3/SDL.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <unordered_set>
#include <utility>
#include <vector>

#include "platform/RuntimePaths.h"
#include "render/PixelAlign.h"
#include "render/RendererInfo.h"
#include "util/PerformanceCounters.h"
#include "util/StartupTrace.h"

namespace microide::render {

namespace {

constexpr float kFontPointSize = 13.0f;
// Cache stores whole-string composites. Editor content can produce more unique
// (text, color) pairs than the legacy per-glyph cache, so the upper bound is
// larger than the 2048 used when each entry held a single glyph.
constexpr std::size_t kMaxCacheEntries = 4096;
// Upper bound on approximate cached-texture VRAM. The entry cap alone does not
// bound memory: whole-string composites have arbitrary width, so a few thousand
// wide entries can pin hundreds of MB. Eviction enforces whichever bound (count
// or bytes) binds first. In normal use most text goes through the ASCII atlas,
// so the cache stays small and neither bound is hit.
constexpr std::size_t kMaxCacheBytes = 48ull * 1024 * 1024;
constexpr float kMinPresentationScale = 0.1f;
}  // namespace

std::size_t SdlTtfTextBackend::EntryByteCost(int width, int height) {
  const std::size_t w = width > 0 ? static_cast<std::size_t>(width) : 0;
  const std::size_t h = height > 0 ? static_cast<std::size_t>(height) : 0;
  return w * h * 4;
}

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
    SDL_Log("microide text: SDL_ttf initialization failed: renderer is null");
    return false;
  }

  if (!TTF_Init()) {
    SDL_Log("microide text: TTF_Init failed: %s", SDL_GetError());
    return false;
  }
  ttf_initialized_ = true;
  renderer_ = renderer;

  const auto font_path = LocateFontFile();
  if (font_path.empty()) {
    SDL_Log("microide text: no usable font found for SDL_ttf backend");
    return false;
  }

  font_ = TTF_OpenFont(font_path.string().c_str(), kFontPointSize);
  if (font_ == nullptr) {
    SDL_Log("microide text: TTF_OpenFont failed for %s: %s", font_path.string().c_str(),
            SDL_GetError());
    return false;
  }
  font_path_ = font_path;
  TTF_SetFontHinting(font_, TTF_HINTING_LIGHT_SUBPIXEL);
  TTF_SetFontKerning(font_, false);
  LoadFallbackFonts();

  // Batched-text path is GPU-only: it is a measured win on a GPU backend
  // (row + gutter runs collapse to per-row SDL_RenderGeometry submits, avoiding
  // composite build+upload churn on scroll) and pixel-identical to the composite
  // path, but it regresses on the software renderer (SDL_RenderGeometry
  // rasterizes per-pixel there). So it defaults on for GPU renderers only;
  // MICROIDE_RENDER_GLYPH_ATLAS=0 is an escape hatch to force the composite path.
  is_gpu_renderer_ = RendererIsGpu(renderer_);
  const char* atlas_env = SDL_getenv("MICROIDE_RENDER_GLYPH_ATLAS");
  glyph_atlas_enabled_ = (atlas_env == nullptr) || (atlas_env[0] != '0');

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

  int max_left_padding_pixels = 0;
  int max_right_padding_pixels = 0;
  int max_advance_pixels = 0;
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
    max_advance_pixels = std::max(max_advance_pixels, std::max(0, advance));
  }

  if (max_advance_pixels > 0) {
    char_width_ = static_cast<float>(max_advance_pixels) / scale_x;
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

  CacheEntry* entry = ResolveEntry(text, color);
  if (entry == nullptr || entry->texture == nullptr) {
    return;
  }

  const float scale_x = std::max(kMinPresentationScale, presentation_scale_x_);
  const float scale_y = std::max(kMinPresentationScale, presentation_scale_y_);
  // Snap only the origin onto the device-pixel grid: snapped*scale is integral,
  // and entry->width/height are whole physical pixels, so both edges land on the
  // grid and the NEAREST-sampled glyph texture maps 1:1. Origins within a line
  // share a fractional base plus integer cell multiples, so all runs snap by the
  // same delta and intra-line spacing is preserved.
  const SDL_FRect destination = SDL_FRect{
      DeviceAlignedOrigin(x, scale_x),
      DeviceAlignedOrigin(y, scale_y),
      static_cast<float>(entry->width) / scale_x,
      static_cast<float>(entry->height) / scale_y,
  };
  SDL_RenderTexture(renderer_, entry->texture, nullptr, &destination);
}

void SdlTtfTextBackend::ClearCache() {
  ascii_atlas_.reset();
  if (gpu_atlas_texture_ != nullptr) {
    SDL_DestroyTexture(gpu_atlas_texture_);
    gpu_atlas_texture_ = nullptr;
    gpu_atlas_width_ = 0;
    gpu_atlas_height_ = 0;
  }
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

void SdlTtfTextBackend::EnsureAsciiAtlas() {
  if (ascii_atlas_ != nullptr || font_ == nullptr) {
    return;
  }
  ascii_atlas_ = AsciiGlyphAtlas::Build(font_);
}

SDL_Surface* SdlTtfTextBackend::BuildAsciiCompositeSurface(std::string_view text,
                                                           SDL_Color color) {
  if (font_ == nullptr || text.empty()) {
    return nullptr;
  }
  // Only opaque colours go through the atlas. Coverage is colour-independent, so
  // an opaque tint reproduces the per-colour render exactly; translucent text
  // falls back to the SDL_ttf whole-string path in ResolveEntry.
  if (color.a != 255) {
    return nullptr;
  }
  EnsureAsciiAtlas();
  if (ascii_atlas_ == nullptr) {
    return nullptr;
  }

  const float scale_x = std::max(kMinPresentationScale, presentation_scale_x_);
  const float cell_width_px = char_width_ * scale_x;
  const int font_height_px = std::max(1, TTF_GetFontHeight(font_));
  const int total_width_px =
      std::max(1, static_cast<int>(std::ceil(static_cast<float>(text.size()) * cell_width_px)));

  SDL_Surface* composite = SDL_CreateSurface(total_width_px, font_height_px, SDL_PIXELFORMAT_RGBA32);
  if (composite == nullptr) {
    return nullptr;
  }
  // Start transparent. Atlas blits supply the visible pixels at exact cell
  // positions; spaces and outside-cell gaps remain alpha=0.
  if (!SDL_FillSurfaceRect(composite, nullptr, 0)) {
    SDL_DestroySurface(composite);
    return nullptr;
  }

  for (std::size_t index = 0; index < text.size(); ++index) {
    const char ch = text[index];
    if (ch == ' ') {
      continue;
    }
    const int dst_x = static_cast<int>(std::lround(static_cast<float>(index) * cell_width_px));
    // Atlas blits are straight copies of colour-modulated coverage, preserving
    // the per-pixel alpha exactly. If a glyph is somehow uncovered, bail so
    // ResolveEntry falls back to the whole-string SDL_ttf render rather than
    // caching a composite with a missing glyph.
    if (!ascii_atlas_->BlitInto(composite, dst_x, ch, color)) {
      SDL_DestroySurface(composite);
      return nullptr;
    }
  }
  return composite;
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
  return h;
}

bool SdlTtfTextBackend::CacheKeyEqual::operator()(const CacheKeyView& lhs,
                                                  const CacheKeyView& rhs) const noexcept {
  return lhs.text == rhs.text && lhs.color.r == rhs.color.r && lhs.color.g == rhs.color.g &&
         lhs.color.b == rhs.color.b && lhs.color.a == rhs.color.a;
}

bool SdlTtfTextBackend::EnsureGpuAtlas() {
  if (gpu_atlas_texture_ != nullptr) {
    return true;
  }
  if (renderer_ == nullptr) {
    return false;
  }
  EnsureAsciiAtlas();
  if (ascii_atlas_ == nullptr || !ascii_atlas_->EnsureAllSlotsFilled()) {
    return false;
  }
  SDL_Surface* surface = ascii_atlas_->Surface();
  if (surface == nullptr) {
    return false;
  }
  SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
  if (texture == nullptr) {
    return false;
  }
  // Coverage is sampled and modulated by per-vertex colour; alpha-blend the
  // result over the destination. NEAREST keeps the device-pixel-snapped glyph
  // 1:1 with its rasterized coverage, matching the composite path.
  SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
  gpu_atlas_texture_ = texture;
  gpu_atlas_width_ = ascii_atlas_->SurfaceWidth();
  gpu_atlas_height_ = ascii_atlas_->SurfaceHeight();
  return true;
}

bool SdlTtfTextBackend::AppendAsciiRunGeometry(float x, float y, SDL_Color color,
                                               std::string_view text) {
  const float scale_x = std::max(kMinPresentationScale, presentation_scale_x_);
  const float scale_y = std::max(kMinPresentationScale, presentation_scale_y_);
  const float cell_width_px = char_width_ * scale_x;
  // Snap the run origin to the device grid exactly as DrawString does, then place
  // each glyph at the same integer cell offset the composite surface uses
  // (lround(index * cell_width_px)). This reproduces the composite path's pixel
  // positions so ASCII typography is identical to the non-atlas path.
  const float origin_x_dev = DeviceAlignedOrigin(x, scale_x) * scale_x;
  const float origin_y_dev = DeviceAlignedOrigin(y, scale_y) * scale_y;
  const float atlas_w = static_cast<float>(gpu_atlas_width_);
  const float atlas_h = static_cast<float>(gpu_atlas_height_);
  const SDL_FColor fcolor{
      static_cast<float>(color.r) / 255.0f,
      static_cast<float>(color.g) / 255.0f,
      static_cast<float>(color.b) / 255.0f,
      static_cast<float>(color.a) / 255.0f,
  };

  for (std::size_t index = 0; index < text.size(); ++index) {
    const char ch = text[index];
    if (ch == ' ') {
      continue;  // spaces contribute no coverage; advance only
    }
    int sx = 0;
    int sw = 0;
    int sh = 0;
    if (!ascii_atlas_->SlotRect(ch, &sx, &sw, &sh)) {
      // A glyph outside the atlas (or failed raster): the caller falls back to
      // the composite path for this whole run so output stays faithful.
      return false;
    }
    if (sw <= 0 || sh <= 0) {
      continue;
    }
    const float glyph_left_dev =
        origin_x_dev + std::round(static_cast<float>(index) * cell_width_px);
    const float left = glyph_left_dev / scale_x;
    const float top = origin_y_dev / scale_y;
    const float right = left + static_cast<float>(sw) / scale_x;
    const float bottom = top + static_cast<float>(sh) / scale_y;

    const float u0 = static_cast<float>(sx) / atlas_w;
    const float u1 = static_cast<float>(sx + sw) / atlas_w;
    const float v0 = 0.0f;
    const float v1 = static_cast<float>(sh) / atlas_h;

    const int base = static_cast<int>(geom_vertices_.size());
    geom_vertices_.push_back(SDL_Vertex{SDL_FPoint{left, top}, fcolor, SDL_FPoint{u0, v0}});
    geom_vertices_.push_back(SDL_Vertex{SDL_FPoint{right, top}, fcolor, SDL_FPoint{u1, v0}});
    geom_vertices_.push_back(SDL_Vertex{SDL_FPoint{left, bottom}, fcolor, SDL_FPoint{u0, v1}});
    geom_vertices_.push_back(SDL_Vertex{SDL_FPoint{right, bottom}, fcolor, SDL_FPoint{u1, v1}});
    geom_indices_.push_back(base + 0);
    geom_indices_.push_back(base + 1);
    geom_indices_.push_back(base + 2);
    geom_indices_.push_back(base + 2);
    geom_indices_.push_back(base + 1);
    geom_indices_.push_back(base + 3);
  }
  return true;
}

void SdlTtfTextBackend::DrawRuns(SDL_Renderer* renderer, const TextRun* runs, std::size_t count) {
  // Fall back to the per-run composite path unless the GPU batched path is armed.
  if (renderer == nullptr || renderer != renderer_ || !glyph_atlas_enabled_ ||
      !is_gpu_renderer_ || !EnsureGpuAtlas() || gpu_atlas_width_ <= 0 || gpu_atlas_height_ <= 0) {
    TextRendererBackend::DrawRuns(renderer, runs, count);
    return;
  }

  // Accumulate every opaque ASCII run in this row into one geometry buffer and
  // submit a single SDL_RenderGeometry. Per-vertex colour means a multi-colour
  // line is one draw call; batching across runs (not just within a run) is what
  // avoids the per-run geometry/fill state flapping that regressed the per-run
  // variant. Non-ASCII / translucent runs (and any glyph outside the atlas) draw
  // via the composite path; runs are non-overlapping so draw order is immaterial.
  geom_vertices_.clear();
  geom_indices_.clear();
  std::size_t batched_runs = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const TextRun& run = runs[i];
    if (run.text.empty()) {
      continue;
    }
    if (run.color.a == 255 && CanUseFastAscii(run.text)) {
      const std::size_t mark = geom_vertices_.size();
      if (AppendAsciiRunGeometry(run.x, run.y, run.color, run.text)) {
        ++batched_runs;
        continue;
      }
      // A glyph was uncovered: discard this run's partial geometry and composite it.
      geom_vertices_.resize(mark);
      geom_indices_.resize(mark / 4 * 6);
    }
    DrawString(renderer, run.x, run.y, run.color, run.text);
  }

  if (!geom_vertices_.empty()) {
    util::AddPerformanceCounter(util::PerfCounterId::RenderGlyphAtlasRuns, batched_runs);
    util::AddPerformanceCounter(util::PerfCounterId::RenderGlyphAtlasGlyphs,
                                geom_vertices_.size() / 4);
    if (!SDL_RenderGeometry(renderer, gpu_atlas_texture_, geom_vertices_.data(),
                            static_cast<int>(geom_vertices_.size()), geom_indices_.data(),
                            static_cast<int>(geom_indices_.size()))) {
      util::AddPerformanceCounter(util::PerfCounterId::RenderGlyphAtlasFallbacks);
    }
  }
}

SdlTtfTextBackend::CacheEntry* SdlTtfTextBackend::ResolveEntry(std::string_view text,
                                                               SDL_Color color) {
  const CacheKeyView key_view{
      .text = text,
      .color = color,
  };
  if (auto it = cache_.find(key_view); it != cache_.end()) {
    util::AddPerformanceCounter(util::PerfCounterId::RenderTextTextureCacheHits);
    cache_order_.splice(cache_order_.end(), cache_order_, it->second.order);
    return &it->second;
  }
  util::AddPerformanceCounter(util::PerfCounterId::RenderTextTextureCacheMisses);

  // ASCII strings render through a cell-positioned composite (per-glyph blits
  // into a single surface) so the resulting texture is one whole-string draw
  // call at runtime, while preserving the deterministic monospaced cell layout
  // that the ASCII-spacing regressions depend on. Non-ASCII falls back to the
  // SDL_ttf whole-string blended path, which already handles shaped glyphs.
  SDL_Surface* surface = nullptr;
  if (CanUseFastAscii(text)) {
    surface = BuildAsciiCompositeSurface(text, color);
  }
  if (surface == nullptr) {
    surface = TTF_RenderText_Blended(font_, text.data(), text.size(), color);
  }
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
  };
  const std::size_t entry_bytes = EntryByteCost(entry.width, entry.height);
  auto [map_it, inserted] = cache_.emplace(std::move(key), std::move(entry));
  if (!inserted) {
    cache_bytes_ -= EntryByteCost(map_it->second.width, map_it->second.height);
    if (map_it->second.texture != nullptr) {
      SDL_DestroyTexture(map_it->second.texture);
    }
    map_it->second = std::move(entry);
  }
  cache_bytes_ += entry_bytes;
  cache_order_.push_back(map_it->first);
  map_it->second.order = std::prev(cache_order_.end());

  // Evict oldest entries until both the count and VRAM budgets are satisfied.
  // Keep at least the just-inserted entry (>1 guard) so a single oversized
  // composite that alone exceeds kMaxCacheBytes never evicts itself, which would
  // dangle the returned pointer.
  while (cache_order_.size() > 1 &&
         (cache_order_.size() > kMaxCacheEntries || cache_bytes_ > kMaxCacheBytes)) {
    auto evict_it = cache_.find(cache_order_.front());
    cache_order_.pop_front();
    if (evict_it != cache_.end()) {
      util::AddPerformanceCounter(util::PerfCounterId::RenderTextTextureCacheEvictions);
      cache_bytes_ -= EntryByteCost(evict_it->second.width, evict_it->second.height);
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
