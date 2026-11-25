#include "render/SdlTtfTextBackend.h"

#include "render/GlyphSurfaceFormat.h"

#if MICROIDE_HAS_SDL3_TTF

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#if MICROIDE_HAS_FONTCONFIG
#include <fontconfig/fontconfig.h>
#endif

#include "platform/RuntimePaths.h"
#include "render/PixelAlign.h"
#include "render/RendererInfo.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StartupTrace.h"
#include "util/StringUtil.h"

namespace microide::render {

namespace {

constexpr float kMinPresentationScale = 0.1f;

template <typename Place>
bool ForEachAtlasGlyph(AsciiGlyphAtlas& atlas, std::string_view text, float cell_width_px,
                       int run_width_px, Place&& place) {
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char ch = text[index];
    if (ch == ' ') {
      continue;
    }
    int sx = 0;
    int sw = 0;
    int sh = 0;
    if (!atlas.SlotRect(ch, &sx, &sw, &sh)) {
      return false;
    }
    const int cell_left_px =
        static_cast<int>(std::lround(static_cast<float>(index) * cell_width_px));
    sw = std::min(sw, run_width_px - cell_left_px);
    if (sw <= 0 || sh <= 0) {
      continue;
    }
    if (!place(cell_left_px, sx, sw, sh)) {
      return false;
    }
  }
  return true;
}


}

std::unique_ptr<SdlTtfTextBackend> SdlTtfTextBackend::Create(SDL_Renderer* renderer) {
  auto backend = std::unique_ptr<SdlTtfTextBackend>(new SdlTtfTextBackend());
  if (!backend->Initialize(renderer)) {
    return nullptr;
  }
  return backend;
}

SdlTtfTextBackend::~SdlTtfTextBackend() {
  ClearGlyphAtlases();
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

  {
    util::PerformanceTrace::Scope ttf_init_scope("SdlTtfTextBackend::Initialize::TTF_Init");
    if (!TTF_Init()) {
      SDL_Log("microide text: TTF_Init failed: %s", SDL_GetError());
      return false;
    }
  }
  ttf_initialized_ = true;
  renderer_ = renderer;

  std::filesystem::path font_path;
  {
    util::PerformanceTrace::Scope locate_scope("SdlTtfTextBackend::Initialize::LocateFontFile");
    font_path = LocateFontFile();
  }
  if (font_path.empty()) {
    SDL_Log("microide text: no usable font found for SDL_ttf backend");
    return false;
  }

  {
    util::PerformanceTrace::Scope open_scope("SdlTtfTextBackend::Initialize::OpenPrimaryFont");
    if (!OpenPrimaryFont(font_path)) {
      SDL_Log("microide text: TTF_OpenFont failed for %s: %s", font_path.string().c_str(),
              SDL_GetError());
      return false;
    }
    regular_font_path_ = font_path;
  }

  texture_format_ = SDL_PIXELFORMAT_RGBA32;
  if (const SDL_PropertiesID renderer_properties = SDL_GetRendererProperties(renderer_);
      renderer_properties != 0) {
    const auto* formats = static_cast<const SDL_PixelFormat*>(SDL_GetPointerProperty(
        renderer_properties, SDL_PROP_RENDERER_TEXTURE_FORMATS_POINTER, nullptr));
    texture_format_ = ChooseGlyphSurfaceFormat(formats);
  }

  is_gpu_renderer_ = RendererIsGpu(renderer_);
  const char* atlas_env = SDL_getenv("MICROIDE_RENDER_GLYPH_ATLAS");
  glyph_atlas_enabled_ = (atlas_env == nullptr) || (atlas_env[0] != '0');

  {
    util::PerformanceTrace::Scope metrics_scope("SdlTtfTextBackend::Initialize::RefreshMetrics");
    RefreshMetrics();
  }
  return true;
}

int SdlTtfTextBackend::CurrentHorizontalDpi() const {
  return std::max(1, static_cast<int>(std::lround(
                         72.0f * std::max(kMinPresentationScale, presentation_scale_x_))));
}

int SdlTtfTextBackend::CurrentVerticalDpi() const {
  return std::max(1, static_cast<int>(std::lround(
                         72.0f * std::max(kMinPresentationScale, presentation_scale_y_))));
}

void SdlTtfTextBackend::ApplyFontSizeAtCurrentScale() {
  if (font_ == nullptr) {
    return;
  }
  const int hdpi = CurrentHorizontalDpi();
  const int vdpi = CurrentVerticalDpi();
  if (!TTF_SetFontSizeDPI(font_, font_point_size_, hdpi, vdpi)) {
    return;
  }
  for (TTF_Font* fallback_font : fallback_fonts_) {
    if (fallback_font != nullptr) {
      TTF_SetFontSizeDPI(fallback_font, font_point_size_, hdpi, vdpi);
    }
  }
  ClearGlyphAtlases();
  RefreshMetrics();
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

  presentation_scale_x_ = resolved_scale_x;
  presentation_scale_y_ = resolved_scale_y;
  ApplyFontSizeAtCurrentScale();
}

void SdlTtfTextBackend::SetFontPointSize(float points) {
  const float resolved = std::clamp(points, kMinFontPointSize, kMaxFontPointSize);
  if (font_ == nullptr || std::fabs(font_point_size_ - resolved) < 0.01f) {
    return;
  }
  font_point_size_ = resolved;
  ApplyFontSizeAtCurrentScale();
}

bool SdlTtfTextBackend::OpenPrimaryFont(const std::filesystem::path& path) {
  if (renderer_ == nullptr) {
    return false;
  }
  TTF_Font* opened = TTF_OpenFont(path.string().c_str(), font_point_size_);
  if (opened == nullptr) {
    return false;
  }
  if (font_ != nullptr) {
    TTF_CloseFont(font_);
  }
  font_ = opened;
  font_path_ = path;
  TTF_SetFontHinting(font_, TTF_HINTING_LIGHT_SUBPIXEL);
  TTF_SetFontKerning(font_, false);
  return true;
}

bool SdlTtfTextBackend::SetFontFamily(std::string_view family) {
  if (requested_font_family_ == family && font_ != nullptr) {
    return false;
  }
  requested_font_family_ = std::string(family);
  const std::filesystem::path resolved = ResolveFamilyToFile(family);
  if (resolved.empty()) {
    SDL_Log("microide text: font family \"%.*s\" not found; keeping current font",
            static_cast<int>(family.size()), family.data());
    return false;
  }
  if (resolved == regular_font_path_ && font_ != nullptr) {
    return false;
  }
  const std::filesystem::path previous_regular = regular_font_path_;
  regular_font_path_ = resolved;
  if (!OpenFaceForStyle()) {
    regular_font_path_ = previous_regular;
    SDL_Log("microide text: failed to open font family \"%.*s\" (%s); keeping current font",
            static_cast<int>(family.size()), family.data(), resolved.string().c_str());
    return false;
  }
  return true;
}

bool SdlTtfTextBackend::SetFontStyle(FontStyle style) {
  if (font_style_ == style || font_ == nullptr) {
    font_style_ = style;
    return false;
  }
  const FontStyle previous = font_style_;
  font_style_ = style;
  if (!OpenFaceForStyle()) {
    font_style_ = previous;
    return false;
  }
  return true;
}

bool SdlTtfTextBackend::OpenFaceForStyle() {
  std::filesystem::path path = font_style_ == FontStyle::Regular
                                   ? regular_font_path_
                                   : StyledFaceFile(regular_font_path_, font_style_);
  int synthetic = TTF_STYLE_NORMAL;
  if (path.empty()) {
    path = regular_font_path_;
    synthetic = ((static_cast<int>(font_style_) & 1) != 0 ? TTF_STYLE_BOLD : 0) |
                ((static_cast<int>(font_style_) & 2) != 0 ? TTF_STYLE_ITALIC : 0);
  }
  if (path == font_path_ && synthetic == synthetic_style_ && font_ != nullptr) {
    return false;
  }
  if (!OpenPrimaryFont(path)) {
    return false;
  }
  synthetic_style_ = synthetic;
  TTF_SetFontStyle(font_, synthetic);
  if (fallback_fonts_loaded_) {
    LoadFallbackFonts();
  } else {
    CloseFallbackFonts();
  }
  ApplyFontSizeAtCurrentScale();
  return true;
}

SdlTtfTextBackend::PixelMetrics SdlTtfTextBackend::MeasurePixelMetrics(TTF_Font* font,
                                                                      const MetricsKey& key) {
  static std::map<MetricsKey, PixelMetrics> cache;
  if (const auto it = cache.find(key); it != cache.end()) {
    return it->second;
  }

  util::PerformanceTrace::Scope perf_scope("SdlTtfTextBackend::MeasurePixelMetrics");
  PixelMetrics metrics;
  metrics.font_height = TTF_GetFontHeight(font);
  for (unsigned char ch = 0x20; ch <= 0x7E; ++ch) {
    int minx = 0;
    int maxx = 0;
    int miny = 0;
    int maxy = 0;
    int advance = 0;
    if (!TTF_GetGlyphMetrics(font, ch, &minx, &maxx, &miny, &maxy, &advance)) {
      continue;
    }
    metrics.max_left_padding = std::max(metrics.max_left_padding, std::max(0, -minx));
    metrics.max_right_padding =
        std::max(metrics.max_right_padding, std::max(0, maxx - std::max(advance, 0)));
    metrics.max_advance = std::max(metrics.max_advance, std::max(0, advance));
  }
  cache.emplace(key, metrics);
  return metrics;
}

void SdlTtfTextBackend::RefreshMetrics() {
  if (font_ == nullptr) {
    return;
  }

  const float scale_x = std::max(kMinPresentationScale, presentation_scale_x_);
  const float scale_y = std::max(kMinPresentationScale, presentation_scale_y_);
  const PixelMetrics pixels = MeasurePixelMetrics(
      font_, MetricsKey{.font_path = font_path_.string(),
                        .point_size = font_point_size_,
                        .hdpi = CurrentHorizontalDpi(),
                        .vdpi = CurrentVerticalDpi(),
                        .synthetic_style = synthetic_style_});
  line_height_ = static_cast<float>(pixels.font_height) / scale_y;

  const int max_left_padding_pixels = pixels.max_left_padding;
  const int max_right_padding_pixels = pixels.max_right_padding;

  if (pixels.max_advance > 0) {
    char_width_ = static_cast<float>(pixels.max_advance) / scale_x;
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

  return static_cast<float>(util::GridCellCount(text)) * char_width_;
}

void SdlTtfTextBackend::DrawString(SDL_Renderer* renderer,
                                   StringTextureCache& cache,
                                   float x,
                                   float y,
                                   SDL_Color color,
                                   std::string_view text) {
  if (renderer == nullptr || renderer != renderer_ || text.empty()) {
    return;
  }

  const StringTextureCache::Entry* entry = ResolveEntry(cache, text, color);
  if (entry == nullptr || entry->texture == nullptr) {
    return;
  }

  const float scale_x = std::max(kMinPresentationScale, presentation_scale_x_);
  const float scale_y = std::max(kMinPresentationScale, presentation_scale_y_);
  const SDL_FRect destination = SDL_FRect{
      DeviceAlignedOrigin(x, scale_x),
      DeviceAlignedOrigin(y, scale_y),
      DeviceExactLogical(static_cast<float>(entry->width), scale_x),
      DeviceExactLogical(static_cast<float>(entry->height), scale_y),
  };
  SDL_RenderTexture(renderer_, entry->texture, nullptr, &destination);
}

void SdlTtfTextBackend::ClearGlyphAtlases() {
  ascii_atlas_.reset();
  cluster_atlas_.reset();
  if (atlas_texture_ != nullptr) {
    SDL_DestroyTexture(atlas_texture_);
    atlas_texture_ = nullptr;
    atlas_texture_width_ = 0;
    atlas_texture_height_ = 0;
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

void SdlTtfTextBackend::EnsureAsciiAtlas() {
  if (ascii_atlas_ != nullptr || font_ == nullptr) {
    return;
  }
  ascii_atlas_ = AsciiGlyphAtlas::Build(font_, texture_format_);
}

void SdlTtfTextBackend::EnsureClusterAtlas() {
  if (cluster_atlas_ != nullptr || font_ == nullptr) {
    return;
  }
  const float scale_x = std::max(kMinPresentationScale, presentation_scale_x_);
  const int slot_width = static_cast<int>(std::ceil(char_width_ * scale_x * 2.0f)) + 2;
  cluster_atlas_ = GlyphClusterAtlas::Build(font_, texture_format_, slot_width,
                                            std::max(1, TTF_GetFontHeight(font_)));
}

SurfacePtr SdlTtfTextBackend::BuildAsciiCompositeSurface(std::string_view text,
                                                           SDL_Color color) {
  if (font_ == nullptr || text.empty()) {
    return nullptr;
  }
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

  SurfacePtr composite = AcquireCompositeSurface(total_width_px, font_height_px);
  if (composite == nullptr) {
    return nullptr;
  }

  util::PerformanceTrace::Scope blit_scope("render::TextBackend::CompositeGlyphBlits");
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char ch = text[index];
    if (ch == ' ') {
      continue;
    }
    const int dst_x = static_cast<int>(std::lround(static_cast<float>(index) * cell_width_px));
    if (!ascii_atlas_->BlitInto(composite.get(), dst_x, ch, color)) {
      return nullptr;
    }
  }
  return composite;
}

SurfacePtr SdlTtfTextBackend::AcquireCompositeSurface(int width, int height) {
  util::PerformanceTrace::Scope alloc_scope("render::TextBackend::CompositeAllocate");
  const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  SurfacePtr composite;
  if (SDL_BYTESPERPIXEL(texture_format_) == sizeof(std::uint32_t) &&
      pixels * sizeof(std::uint32_t) <= kMaxCompositeScratchBytes) {
    if (composite_scratch_.size() < pixels) {
      composite_scratch_.resize(std::max(pixels, composite_scratch_.size() * 2));
    }
    composite.reset(SDL_CreateSurfaceFrom(width, height, texture_format_,
                                          composite_scratch_.data(),
                                          width * static_cast<int>(sizeof(std::uint32_t))));
  } else {
    composite.reset(SDL_CreateSurface(width, height, texture_format_));
  }
  if (composite == nullptr || !SDL_FillSurfaceRect(composite.get(), nullptr, 0)) {
    return nullptr;
  }
  return composite;
}

SurfacePtr SdlTtfTextBackend::BuildGridCompositeSurface(std::string_view text,
                                                          SDL_Color color) {
  util::AddPerformanceCounter(util::PerfCounterId::RenderGridCompositeSurfaces);
  std::uint64_t cluster_rasterizations = 0;
  std::uint64_t cluster_atlas_blits = 0;
  if (font_ == nullptr || text.empty()) {
    return nullptr;
  }
  EnsureFallbackFontsLoaded();
  const bool atlas_ok = color.a == 255;
  if (atlas_ok) {
    EnsureAsciiAtlas();
    EnsureClusterAtlas();
  }

  const float scale_x = std::max(kMinPresentationScale, presentation_scale_x_);
  const float cell_width_px = char_width_ * scale_x;
  const int font_height_px = std::max(1, TTF_GetFontHeight(font_));
  const std::size_t total_cells = util::GridCellCount(text);
  const int total_width_px = std::max(
      1, static_cast<int>(std::ceil(static_cast<float>(std::max<std::size_t>(total_cells, 1)) *
                                    cell_width_px)));

  SurfacePtr composite = AcquireCompositeSurface(total_width_px, font_height_px);
  if (composite == nullptr) {
    return nullptr;
  }

  util::PerformanceTrace::Scope blit_scope("render::TextBackend::CompositeGlyphBlits");
  std::size_t cell = 0;
  for (std::size_t offset = 0; offset < text.size();) {
    const std::size_t base_length =
        static_cast<unsigned char>(text[offset]) < 0x80 ? 1 : util::Utf8SequenceLength(text, offset);
    const char32_t base =
        base_length == 1 && static_cast<unsigned char>(text[offset]) < 0x80
            ? static_cast<char32_t>(static_cast<unsigned char>(text[offset]))
            : util::DecodeUtf8Codepoint(text.substr(offset, base_length));
    std::size_t width = util::GridCellWidth(base);
    std::size_t end = offset + base_length;
    while (end < text.size()) {
      const std::size_t mark_length =
          static_cast<unsigned char>(text[end]) < 0x80 ? 1 : util::Utf8SequenceLength(text, end);
      if (mark_length == 1 && static_cast<unsigned char>(text[end]) < 0x80) {
        break;
      }
      if (util::GridCellWidth(util::DecodeUtf8Codepoint(text.substr(end, mark_length))) != 0) {
        break;
      }
      end += mark_length;
    }
    if (width == 0) {
      width = 1;
    }
    const std::string_view cluster = text.substr(offset, end - offset);
    const int dst_x = static_cast<int>(std::lround(static_cast<float>(cell) * cell_width_px));
    std::size_t blank_after = 0;
    while (blank_after < width && end + blank_after < text.size() && text[end + blank_after] == ' ') {
      ++blank_after;
    }
    const int span_px =
        static_cast<int>(std::lround(static_cast<float>(cell + width + blank_after) * cell_width_px)) -
        dst_x;
    const bool ascii_cell = cluster.size() == 1 && base >= 0x20 && base <= 0x7E;
    if (!ascii_cell) {
      EnsureCodepointCovered(base);
    }
    if (ascii_cell && base == ' ') {
    } else if (ascii_cell && atlas_ok && ascii_atlas_ != nullptr &&
               ascii_atlas_->BlitInto(composite.get(), dst_x, static_cast<char>(base), color)) {
    } else if (atlas_ok && cluster_atlas_ != nullptr &&
               cluster_atlas_->BlitInto(composite.get(), dst_x, span_px, cluster, color)) {
      ++cluster_atlas_blits;
    } else {
      ++cluster_rasterizations;
      const SurfacePtr glyph(
          TTF_RenderText_Blended(font_, cluster.data(), cluster.size(), color));
      if (glyph != nullptr) {
        SDL_SetSurfaceBlendMode(glyph.get(), SDL_BLENDMODE_NONE);
        SDL_Rect source{0, 0, std::min(glyph->w, span_px), std::min(glyph->h, font_height_px)};
        SDL_Rect destination{dst_x, 0, source.w, source.h};
        SDL_BlitSurface(glyph.get(), &source, composite.get(), &destination);
      }
    }
    cell += width;
    offset = end;
  }
  util::AddPerformanceCounter(util::PerfCounterId::RenderGridCompositeClusterRasterizations,
                              cluster_rasterizations);
  util::AddPerformanceCounter(util::PerfCounterId::RenderGridCompositeClusterAtlasBlits,
                              cluster_atlas_blits);
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

  std::error_code font_ec;
  std::filesystem::path bundled_font =
      platform::ResolveBundledAssetPath("fonts/JetBrainsMono-Regular.ttf");
  if (!bundled_font.empty() && std::filesystem::exists(bundled_font, font_ec) && !font_ec) {
    return bundled_font;
  }

  for (const char* candidate : kSystemCandidates) {
    const std::filesystem::path path(candidate);
    font_ec.clear();
    if (std::filesystem::exists(path, font_ec) && !font_ec) {
      return path;
    }
  }
  return {};
}

namespace {

std::string NormalizeFontToken(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    if (c == ' ' || c == '-' || c == '_' || c == '.') {
      continue;
    }
    out.push_back(util::ToLowerAsciiChar(static_cast<char>(c)));
  }
  return out;
}

bool FamilyNameMatches(std::string_view requested, std::string_view family) {
  return !requested.empty() && !family.empty() &&
         (family.starts_with(requested) || requested.starts_with(family));
}

bool IsFontStyleToken(std::string_view token) {
  static constexpr std::string_view kStyles[] = {
      "regular", "bold", "italic", "oblique", "bolditalic", "boldoblique",
      "light", "medium", "semibold", "demibold", "thin", "black", "heavy",
      "book", "roman", "extralight", "ultralight", "extrabold", "ultrabold",
      "condensed", "semicondensed", "extracondensed", "expanded", "extended",
      "semilight", "semibolditalic", "retina", "text", "display", "variablefont",
      "hairline", "narrow", "wide",
      "reg", "rg", "bd", "it", "ita", "ital", "obl", "obli", "md", "lt", "blk",
      "sb", "demi", "semibd", "demibd", "xbd", "extbd", "ultbd", "ultlt", "cond",
      "cn", "nrw", "extd"};
  return std::find(std::begin(kStyles), std::end(kStyles), token) != std::end(kStyles);
}

std::vector<std::string> StyleSubWords(std::string_view segment) {
  std::vector<std::string> words;
  std::string current;
  const auto flush = [&] {
    if (!current.empty()) {
      words.push_back(current);
      current.clear();
    }
  };
  for (std::size_t i = 0; i < segment.size(); ++i) {
    const char raw = segment[i];
    if (raw == ' ' || raw == '-' || raw == '_' || raw == '.') {
      flush();
      continue;
    }
    if (!current.empty() && i > 0) {
      const unsigned char prev = static_cast<unsigned char>(segment[i - 1]);
      const unsigned char cur = static_cast<unsigned char>(raw);
      const bool camel = util::IsAsciiLower(static_cast<unsigned char>(prev)) && util::IsAsciiUpper(static_cast<unsigned char>(cur));
      const bool digit_edge = util::IsAsciiAlpha(static_cast<unsigned char>(prev)) && util::IsAsciiDigit(static_cast<unsigned char>(cur));
      if (camel || digit_edge) {
        flush();
      }
    }
    current.push_back(util::ToLowerAsciiChar(static_cast<char>(raw)));
  }
  flush();
  return words;
}

bool SegmentIsAllStyleTokens(std::string_view segment) {
  const std::vector<std::string> words = StyleSubWords(segment);
  if (words.empty()) {
    return false;
  }
  for (const std::string& word : words) {
    if (!IsFontStyleToken(word)) {
      return false;
    }
  }
  return true;
}

std::string FontDisplayNameFromStem(std::string_view stem) {
  std::string base(stem);
  if (const std::size_t bracket = base.find('['); bracket != std::string::npos) {
    base = base.substr(0, bracket);
  }
  while (!base.empty() && (base.back() == '-' || base.back() == '_' || base.back() == ' ')) {
    base.pop_back();
  }
  for (;;) {
    const std::size_t sep = base.find_last_of("-_");
    if (sep == std::string::npos || !SegmentIsAllStyleTokens(base.substr(sep + 1))) {
      break;
    }
    base = base.substr(0, sep);
    while (!base.empty() && (base.back() == '-' || base.back() == '_' || base.back() == ' ')) {
      base.pop_back();
    }
  }
  for (char& c : base) {
    if (c == '-' || c == '_') {
      c = ' ';
    }
  }
  std::string out;
  out.reserve(base.size() + 4);
  for (std::size_t i = 0; i < base.size(); ++i) {
    const char c = base[i];
    if (i > 0 && c != ' ' && base[i - 1] != ' ') {
      const unsigned char prev = static_cast<unsigned char>(base[i - 1]);
      const unsigned char cur = static_cast<unsigned char>(c);
      const bool camel = util::IsAsciiLower(static_cast<unsigned char>(prev)) && util::IsAsciiUpper(static_cast<unsigned char>(cur));
      const bool digit_edge = util::IsAsciiAlpha(static_cast<unsigned char>(prev)) && util::IsAsciiDigit(static_cast<unsigned char>(cur));
      if (camel || digit_edge) {
        out.push_back(' ');
      }
    }
    out.push_back(c);
  }
  std::string collapsed;
  collapsed.reserve(out.size());
  bool prev_space = false;
  for (char c : out) {
    if (c == ' ') {
      if (prev_space) {
        continue;
      }
      prev_space = true;
    } else {
      prev_space = false;
    }
    collapsed.push_back(c);
  }
  return collapsed.empty() ? std::string(stem) : collapsed;
}

}

std::vector<std::filesystem::path> SdlTtfTextBackend::FontSearchRoots() {
  std::vector<std::filesystem::path> roots;
  std::unordered_set<std::string> seen;
  const auto add = [&](std::filesystem::path path) {
    if (path.empty()) {
      return;
    }
    if (seen.insert(path.string()).second) {
      roots.push_back(std::move(path));
    }
  };

  const char* home = SDL_getenv("HOME");

  if (const char* xdg_data_home = SDL_getenv("XDG_DATA_HOME");
      xdg_data_home != nullptr && xdg_data_home[0] != '\0') {
    add(std::filesystem::path(xdg_data_home) / "fonts");
  } else if (home != nullptr) {
    add(std::filesystem::path(home) / ".local/share/fonts");
  }
  if (home != nullptr) {
    add(std::filesystem::path(home) / ".fonts");
    add(std::filesystem::path(home) / ".nix-profile/share/fonts");
  }

  const char* xdg_data_dirs = SDL_getenv("XDG_DATA_DIRS");
  const std::string data_dirs =
      (xdg_data_dirs != nullptr && xdg_data_dirs[0] != '\0') ? xdg_data_dirs
                                                             : "/usr/local/share:/usr/share";
  for (std::size_t start = 0; start <= data_dirs.size();) {
    const std::size_t colon = data_dirs.find(':', start);
    const std::size_t end = colon == std::string::npos ? data_dirs.size() : colon;
    if (end > start) {
      add(std::filesystem::path(data_dirs.substr(start, end - start)) / "fonts");
    }
    if (colon == std::string::npos) {
      break;
    }
    start = colon + 1;
  }

  for (const char* extra : {"/run/current-system/sw/share/fonts",
                            "/run/host/usr/share/fonts", "/usr/share/fonts",
                            "/usr/local/share/fonts"}) {
    add(extra);
  }
  return roots;
}

std::vector<std::string> SdlTtfTextBackend::AvailableFontFamilies() const {
  std::vector<std::string> families;
#if MICROIDE_HAS_FONTCONFIG
  if (FcConfig* config = FcInitLoadConfigAndFonts()) {
    FcPattern* pattern = FcPatternCreate();
    FcObjectSet* object_set = FcObjectSetBuild(FC_FAMILY, nullptr);
    if (pattern != nullptr && object_set != nullptr) {
      if (FcFontSet* set = FcFontList(config, pattern, object_set)) {
        families.reserve(static_cast<std::size_t>(set->nfont));
        for (int i = 0; i < set->nfont; ++i) {
          FcChar8* family = nullptr;
          if (FcPatternGetString(set->fonts[i], FC_FAMILY, 0, &family) == FcResultMatch &&
              family != nullptr) {
            families.emplace_back(reinterpret_cast<const char*>(family));
          }
        }
        FcFontSetDestroy(set);
      }
    }
    if (object_set != nullptr) {
      FcObjectSetDestroy(object_set);
    }
    if (pattern != nullptr) {
      FcPatternDestroy(pattern);
    }
    FcConfigDestroy(config);
  }
#else
  for (const std::filesystem::path& root : FontSearchRoots()) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
      continue;
    }
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end && !ec;
         it.increment(ec)) {
      std::error_code file_ec;
      if (!it->is_regular_file(file_ec)) {
        continue;
      }
      const std::filesystem::path& path = it->path();
      const std::string ext = NormalizeFontToken(path.extension().string());
      if (ext != "ttf" && ext != "otf") {
        continue;
      }
      std::string name = FontDisplayNameFromStem(path.stem().string());
      if (!name.empty()) {
        families.push_back(std::move(name));
      }
    }
  }
#endif
  const auto ci_less = [](const std::string& a, const std::string& b) {
    return std::lexicographical_compare(
        a.begin(), a.end(), b.begin(), b.end(), [](unsigned char x, unsigned char y) {
          return util::ToLowerAsciiChar(static_cast<char>(x)) < util::ToLowerAsciiChar(static_cast<char>(y));
        });
  };
  const auto ci_equal = [](const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
             return util::ToLowerAsciiChar(static_cast<char>(x)) == util::ToLowerAsciiChar(static_cast<char>(y));
           });
  };
  std::sort(families.begin(), families.end(), ci_less);
  families.erase(std::unique(families.begin(), families.end(), ci_equal), families.end());
  return families;
}

std::string SdlTtfTextBackend::FontDisplayNameFromStemForTesting(std::string_view stem) {
  return FontDisplayNameFromStem(stem);
}

std::filesystem::path SdlTtfTextBackend::StyledFaceFile(const std::filesystem::path& regular,
                                                     FontStyle style) {
  if (regular.empty() || style == FontStyle::Regular) {
    return {};
  }
  static std::map<std::pair<std::string, int>, std::filesystem::path> cache;
  const auto key = std::make_pair(regular.string(), static_cast<int>(style));
  if (const auto it = cache.find(key); it != cache.end()) {
    return it->second;
  }
  util::PerformanceTrace::Scope perf_scope("SdlTtfTextBackend::StyledFaceFile");
  const bool bold = (static_cast<int>(style) & 1) != 0;
  const bool italic = (static_cast<int>(style) & 2) != 0;
  std::filesystem::path found;

  {
    std::string base = regular.stem().string();
    for (const std::string_view tail : {"-Regular", "Regular", "-Book", "-Roman"}) {
      if (base.size() > tail.size() && base.ends_with(tail)) {
        base.resize(base.size() - tail.size());
        break;
      }
    }
    std::vector<std::string> suffixes;
    if (bold && italic) {
      suffixes = {"-BoldItalic", "-BoldOblique", "-Bold-Italic"};
    } else if (bold) {
      suffixes = {"-Bold"};
    } else {
      suffixes = {"-Italic", "-Oblique"};
    }
    for (const std::string& suffix : suffixes) {
      const std::filesystem::path candidate =
          regular.parent_path() / (base + suffix + regular.extension().string());
      std::error_code ec;
      if (candidate != regular && std::filesystem::is_regular_file(candidate, ec) && !ec) {
        found = candidate;
        break;
      }
    }
  }

#if MICROIDE_HAS_FONTCONFIG
  if (found.empty()) {
    if (FcConfig* config = FcInitLoadConfigAndFonts()) {
      std::string family;
      if (FcPattern* by_file = FcPatternCreate()) {
        FcPatternAddString(by_file, FC_FILE, reinterpret_cast<const FcChar8*>(regular.c_str()));
        if (FcObjectSet* objects = FcObjectSetBuild(FC_FAMILY, nullptr)) {
          if (FcFontSet* set = FcFontList(config, by_file, objects)) {
            FcChar8* name = nullptr;
            if (set->nfont > 0 && FcPatternGetString(set->fonts[0], FC_FAMILY, 0, &name) == FcResultMatch &&
                name != nullptr) {
              family = reinterpret_cast<const char*>(name);
            }
            FcFontSetDestroy(set);
          }
          FcObjectSetDestroy(objects);
        }
        FcPatternDestroy(by_file);
      }
      if (!family.empty()) {
        if (FcPattern* wanted = FcPatternCreate()) {
          FcPatternAddString(wanted, FC_FAMILY, reinterpret_cast<const FcChar8*>(family.c_str()));
          FcPatternAddInteger(wanted, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
          FcPatternAddInteger(wanted, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
          FcConfigSubstitute(config, wanted, FcMatchPattern);
          FcDefaultSubstitute(wanted);
          FcResult result = FcResultNoMatch;
          if (FcPattern* matched = FcFontMatch(config, wanted, &result)) {
            FcChar8* matched_family = nullptr;
            FcChar8* file = nullptr;
            int weight = FC_WEIGHT_REGULAR;
            int slant = FC_SLANT_ROMAN;
            (void)FcPatternGetInteger(matched, FC_WEIGHT, 0, &weight);
            (void)FcPatternGetInteger(matched, FC_SLANT, 0, &slant);
            if (FcPatternGetString(matched, FC_FAMILY, 0, &matched_family) == FcResultMatch &&
                matched_family != nullptr &&
                NormalizeFontToken(reinterpret_cast<const char*>(matched_family)) ==
                    NormalizeFontToken(family) &&
                (weight >= FC_WEIGHT_DEMIBOLD) == bold && (slant != FC_SLANT_ROMAN) == italic &&
                FcPatternGetString(matched, FC_FILE, 0, &file) == FcResultMatch && file != nullptr &&
                std::filesystem::path(reinterpret_cast<const char*>(file)) != regular) {
              found = std::filesystem::path(reinterpret_cast<const char*>(file));
            }
            FcPatternDestroy(matched);
          }
          FcPatternDestroy(wanted);
        }
      }
      FcConfigDestroy(config);
    }
  }
#endif
  cache.emplace(key, found);
  return found;
}

std::filesystem::path SdlTtfTextBackend::ResolveFamilyToFile(std::string_view family) {
  if (family.empty()) {
    return LocateFontFile();
  }
  {
    const std::filesystem::path direct(family);
    std::error_code ec;
    if (std::filesystem::exists(direct, ec) && !std::filesystem::is_directory(direct, ec)) {
      return direct;
    }
  }

#if MICROIDE_HAS_FONTCONFIG
  if (FcConfig* config = FcInitLoadConfigAndFonts()) {
    std::filesystem::path result;
    const std::string family_str(family);
    FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>(family_str.c_str()));
    if (pattern != nullptr) {
      FcConfigSubstitute(config, pattern, FcMatchPattern);
      FcDefaultSubstitute(pattern);
      FcResult match_result = FcResultNoMatch;
      if (FcPattern* matched = FcFontMatch(config, pattern, &match_result)) {
        const std::string needle = NormalizeFontToken(family);
        bool family_matches = false;
        for (int i = 0; !family_matches && !needle.empty(); ++i) {
          FcChar8* candidate = nullptr;
          if (FcPatternGetString(matched, FC_FAMILY, i, &candidate) != FcResultMatch ||
              candidate == nullptr) {
            break;
          }
          const std::string cand =
              NormalizeFontToken(reinterpret_cast<const char*>(candidate));
          family_matches = FamilyNameMatches(needle, cand);
        }
        if (family_matches) {
          FcChar8* file = nullptr;
          if (FcPatternGetString(matched, FC_FILE, 0, &file) == FcResultMatch &&
              file != nullptr) {
            result = std::filesystem::path(reinterpret_cast<const char*>(file));
          }
        }
        FcPatternDestroy(matched);
      }
      FcPatternDestroy(pattern);
    }
    FcConfigDestroy(config);
    if (!result.empty()) {
      return result;
    }
  }
#endif

  const std::string needle = NormalizeFontToken(family);
  if (needle.empty()) {
    return LocateFontFile();
  }
  std::filesystem::path exact;
  std::filesystem::path regular;
  std::filesystem::path same_family;
  for (const std::filesystem::path& root : FontSearchRoots()) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
      continue;
    }
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end && !ec;
         it.increment(ec)) {
      if (!it->is_regular_file(ec)) {
        continue;
      }
      const std::filesystem::path& path = it->path();
      const std::string ext = NormalizeFontToken(path.extension().string());
      if (ext != "ttf" && ext != "otf") {
        continue;
      }
      const std::string stem = NormalizeFontToken(path.stem().string());
      if (stem == needle) {
        exact = path;
        break;
      }
      if (regular.empty() && stem == needle + "regular") {
        regular = path;
      }
      if (same_family.empty() &&
          FamilyNameMatches(needle, NormalizeFontToken(FontDisplayNameFromStem(path.stem().string())))) {
        same_family = path;
      }
    }
    if (!exact.empty()) {
      break;
    }
  }
  if (!exact.empty()) {
    return exact;
  }
  if (!regular.empty()) {
    return regular;
  }
  return same_family;
}

std::vector<std::filesystem::path> SdlTtfTextBackend::LocateFallbackFontFiles(
    const std::filesystem::path& primary_font) {
  static const std::vector<std::filesystem::path> kResolvedFallbacks = [] {
    struct Fallback {
      const char* family;
      const char* debian_path;
    };
    static constexpr std::array<Fallback, 5> kFallbacks = {
        Fallback{"Noto Sans Symbols",
                 "/usr/share/fonts/truetype/noto/NotoSansSymbols-Regular.ttf"},
        Fallback{"Noto Sans Symbols 2",
                 "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf"},
        Fallback{"Noto Sans", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"},
        Fallback{"DejaVu Sans", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"},
        Fallback{"FreeSans", "/usr/share/fonts/truetype/freefont/FreeSans.ttf"},
    };
    std::vector<std::filesystem::path> resolved;
    std::unordered_set<std::string> seen;
    const auto push = [&](std::filesystem::path candidate) {
      std::error_code exists_ec;
      if (candidate.empty() || !std::filesystem::exists(candidate, exists_ec) || exists_ec) {
        return;
      }
      std::error_code ec;
      std::filesystem::path normalized = std::filesystem::weakly_canonical(candidate, ec);
      if (ec) {
        normalized = candidate.lexically_normal();
      }
      if (seen.insert(normalized.string()).second) {
        resolved.push_back(std::move(normalized));
      }
    };
    push(platform::ResolveBundledAssetPath("fonts/JetBrainsMono-Regular.ttf"));
    for (const Fallback& fallback : kFallbacks) {
      const std::filesystem::path debian(fallback.debian_path);
      std::error_code debian_ec;
      if (std::filesystem::exists(debian, debian_ec) && !debian_ec) {
        push(debian);
      } else {
        push(ResolveFamilyToFile(fallback.family));
      }
    }
    return resolved;
  }();

  std::filesystem::path primary_resolved;
  if (!primary_font.empty()) {
    std::error_code ec;
    const std::filesystem::path normalized = std::filesystem::weakly_canonical(primary_font, ec);
    primary_resolved = ec ? primary_font.lexically_normal() : normalized;
  }
  std::vector<std::filesystem::path> candidates;
  candidates.reserve(kResolvedFallbacks.size());
  for (const std::filesystem::path& path : kResolvedFallbacks) {
    if (!primary_resolved.empty() && path == primary_resolved) {
      continue;
    }
    candidates.push_back(path);
  }
  return candidates;
}

void SdlTtfTextBackend::CloseFallbackFonts() {
  if (font_ != nullptr) {
    TTF_ClearFallbackFonts(font_);
  }
  for (TTF_Font* fallback_font : fallback_fonts_) {
    if (fallback_font != nullptr) {
      TTF_CloseFont(fallback_font);
    }
  }
  fallback_fonts_.clear();
  fallback_fonts_loaded_ = false;
  checked_codepoints_.clear();
  checked_codepoints_.shrink_to_fit();
}

void SdlTtfTextBackend::CloseFonts() {
  CloseFallbackFonts();
  if (font_ != nullptr) {
    TTF_CloseFont(font_);
    font_ = nullptr;
  }
}

void SdlTtfTextBackend::LoadFallbackFonts() {
  if (font_ == nullptr) {
    return;
  }
  util::PerformanceTrace::Scope perf_scope("SdlTtfTextBackend::LoadFallbackFonts");
  CloseFallbackFonts();
  fallback_fonts_loaded_ = true;

  const int hdpi = CurrentHorizontalDpi();
  const int vdpi = CurrentVerticalDpi();
  for (const auto& fallback_path : LocateFallbackFontFiles(font_path_)) {
    TTF_Font* fallback_font = TTF_OpenFont(fallback_path.string().c_str(), font_point_size_);
    if (fallback_font == nullptr) {
      continue;
    }
    TTF_SetFontHinting(fallback_font, TTF_HINTING_LIGHT_SUBPIXEL);
    TTF_SetFontKerning(fallback_font, false);
    TTF_SetFontSizeDPI(fallback_font, font_point_size_, hdpi, vdpi);
    if (!TTF_AddFallbackFont(font_, fallback_font)) {
      TTF_CloseFont(fallback_font);
      continue;
    }
    fallback_fonts_.push_back(fallback_font);
  }
}

namespace {

#if MICROIDE_HAS_FONTCONFIG
std::filesystem::path FaceCoveringCodepoint(char32_t codepoint) {
  static const bool initialized = FcInit() == FcTrue;
  FcConfig* config = initialized ? FcConfigGetCurrent() : nullptr;
  if (config == nullptr) {
    return {};
  }
  std::filesystem::path result;
  FcPattern* pattern = FcPatternCreate();
  FcCharSet* wanted = FcCharSetCreate();
  if (pattern != nullptr && wanted != nullptr && FcCharSetAddChar(wanted, codepoint) == FcTrue) {
    FcPatternAddCharSet(pattern, FC_CHARSET, wanted);
    FcPatternAddBool(pattern, FC_SCALABLE, FcTrue);
    FcConfigSubstitute(config, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult sort_result = FcResultNoMatch;
    if (FcFontSet* sorted = FcFontSort(config, pattern, FcFalse, nullptr, &sort_result)) {
      for (int i = 0; i < sorted->nfont && result.empty(); ++i) {
        FcPattern* font = sorted->fonts[i];
        FcCharSet* has = nullptr;
        FcBool color = FcFalse;
        FcBool scalable = FcTrue;
        FcChar8* file = nullptr;
        if (FcPatternGetCharSet(font, FC_CHARSET, 0, &has) != FcResultMatch || has == nullptr ||
            FcCharSetHasChar(has, codepoint) != FcTrue) {
          continue;
        }
#ifdef FC_COLOR
        (void)FcPatternGetBool(font, FC_COLOR, 0, &color);
#endif
        (void)FcPatternGetBool(font, FC_SCALABLE, 0, &scalable);
        if (color == FcTrue || scalable != FcTrue) {
          continue;
        }
        if (FcPatternGetString(font, FC_FILE, 0, &file) == FcResultMatch && file != nullptr) {
          result = std::filesystem::path(reinterpret_cast<const char*>(file));
        }
      }
      FcFontSetDestroy(sorted);
    }
  }
  if (wanted != nullptr) {
    FcCharSetDestroy(wanted);
  }
  if (pattern != nullptr) {
    FcPatternDestroy(pattern);
  }
  return result;
}
#endif

}

std::filesystem::path SdlTtfTextBackend::FaceCoveringCodepointForTesting(char32_t codepoint) {
#if MICROIDE_HAS_FONTCONFIG
  return FaceCoveringCodepoint(codepoint);
#else
  (void)codepoint;
  return {};
#endif
}

void SdlTtfTextBackend::EnsureCodepointCovered(char32_t codepoint) const {
  constexpr std::size_t kScalarCount = 0x110000;
  if (font_ == nullptr || codepoint >= kScalarCount) {
    return;
  }
  if (checked_codepoints_.empty()) {
    checked_codepoints_.assign(kScalarCount / 64, 0);
  }
  std::uint64_t& word = checked_codepoints_[codepoint / 64];
  const std::uint64_t bit = std::uint64_t{1} << (codepoint % 64);
  if ((word & bit) != 0) {
    return;
  }
  word |= bit;
  if (TTF_FontHasGlyph(font_, codepoint)) {
    return;
  }
  for (TTF_Font* fallback : fallback_fonts_) {
    if (fallback != nullptr && TTF_FontHasGlyph(fallback, codepoint)) {
      return;
    }
  }
#if MICROIDE_HAS_FONTCONFIG
  util::AddPerformanceCounter(util::PerfCounterId::RenderFontFallbackQueries);
  const std::filesystem::path face = FaceCoveringCodepoint(codepoint);
  if (face.empty() || face == font_path_) {
    return;
  }
  TTF_Font* added = TTF_OpenFont(face.string().c_str(), font_point_size_);
  if (added == nullptr) {
    return;
  }
  TTF_SetFontHinting(added, TTF_HINTING_LIGHT_SUBPIXEL);
  TTF_SetFontKerning(added, false);
  TTF_SetFontSizeDPI(added, font_point_size_, CurrentHorizontalDpi(), CurrentVerticalDpi());
  if (!TTF_AddFallbackFont(font_, added)) {
    TTF_CloseFont(added);
    return;
  }
  fallback_fonts_.push_back(added);
#endif
}

void SdlTtfTextBackend::EnsureFallbackFontsLoaded() const {
  if (fallback_fonts_loaded_ || font_ == nullptr) {
    return;
  }
  const_cast<SdlTtfTextBackend*>(this)->LoadFallbackFonts();
}

bool SdlTtfTextBackend::EnsureAtlasTexture() {
  if (atlas_texture_ != nullptr) {
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
  SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
  atlas_texture_ = texture;
  atlas_texture_width_ = ascii_atlas_->SurfaceWidth();
  atlas_texture_height_ = ascii_atlas_->SurfaceHeight();
  return true;
}

int SdlTtfTextBackend::AsciiRunSurfaceWidthPx(std::size_t char_count) const {
  const float scale_x = std::max(kMinPresentationScale, presentation_scale_x_);
  const float cell_width_px = char_width_ * scale_x;
  return std::max(1, static_cast<int>(std::ceil(static_cast<float>(char_count) * cell_width_px)));
}

bool SdlTtfTextBackend::AppendAsciiRunGeometry(float x, float y, SDL_Color color,
                                               std::string_view text) {
  const float scale_x = std::max(kMinPresentationScale, presentation_scale_x_);
  const float scale_y = std::max(kMinPresentationScale, presentation_scale_y_);
  const float cell_width_px = char_width_ * scale_x;
  const float origin_x_dev = DeviceAlignedOrigin(x, scale_x) * scale_x;
  const float origin_y_dev = DeviceAlignedOrigin(y, scale_y) * scale_y;
  const float atlas_w = static_cast<float>(atlas_texture_width_);
  const float atlas_h = static_cast<float>(atlas_texture_height_);
  const int run_width_px = AsciiRunSurfaceWidthPx(text.size());
  const SDL_FColor fcolor{
      static_cast<float>(color.r) / 255.0f,
      static_cast<float>(color.g) / 255.0f,
      static_cast<float>(color.b) / 255.0f,
      static_cast<float>(color.a) / 255.0f,
  };

  return ForEachAtlasGlyph(*ascii_atlas_, text, cell_width_px, run_width_px,
                           [&](int cell_left_px, int sx, int sw, int sh) {
    const float glyph_left_dev = origin_x_dev + static_cast<float>(cell_left_px);
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
    return true;
  });
}

bool SdlTtfTextBackend::DrawAsciiRunFromAtlas(float x, float y, SDL_Color color,
                                              std::string_view text) {
  if (ascii_atlas_ == nullptr || atlas_texture_ == nullptr) {
    return false;
  }
  const float scale_x = std::max(kMinPresentationScale, presentation_scale_x_);
  const float scale_y = std::max(kMinPresentationScale, presentation_scale_y_);
  const float cell_width_px = char_width_ * scale_x;
  const float origin_x_dev = std::round(x * scale_x);
  const float top = DeviceAlignedOrigin(y, scale_y);
  const int run_width_px = AsciiRunSurfaceWidthPx(text.size());

  SDL_SetTextureColorMod(atlas_texture_, color.r, color.g, color.b);
  SDL_SetTextureAlphaMod(atlas_texture_, 255);

  std::size_t drawn = 0;
  const bool placed = ForEachAtlasGlyph(*ascii_atlas_, text, cell_width_px, run_width_px,
                                        [&](int cell_left_px, int sx, int sw, int sh) {
    const SDL_FRect source{static_cast<float>(sx), 0.0f, static_cast<float>(sw),
                           static_cast<float>(sh)};
    const SDL_FRect destination{
        DeviceExactLogical(origin_x_dev + static_cast<float>(cell_left_px), scale_x),
        top,
        DeviceExactLogical(static_cast<float>(sw), scale_x),
        DeviceExactLogical(static_cast<float>(sh), scale_y),
    };
    if (!SDL_RenderTexture(renderer_, atlas_texture_, &source, &destination)) {
      util::AddPerformanceCounter(util::PerfCounterId::RenderGlyphAtlasFallbacks);
      return false;
    }
    ++drawn;
    return true;
  });
  if (!placed) {
    return false;
  }
  util::AddPerformanceCounter(util::PerfCounterId::RenderGlyphAtlasGlyphs, drawn);
  return true;
}

void SdlTtfTextBackend::DrawEphemeralRuns(SDL_Renderer* renderer, StringTextureCache& cache,
                                          const TextRun* runs, std::size_t count) {
  if (is_gpu_renderer_) {
    DrawRuns(renderer, cache, runs, count);
    return;
  }
  if (renderer == nullptr || renderer != renderer_ || !glyph_atlas_enabled_ ||
      !EnsureAtlasTexture() || atlas_texture_width_ <= 0 || atlas_texture_height_ <= 0) {
    TextRendererBackend::DrawRuns(renderer, cache, runs, count);
    return;
  }

  std::size_t atlas_runs = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const TextRun& run = runs[i];
    if (run.text.empty()) {
      continue;
    }
    if (run.color.a == 255 && CanUseFastAscii(run.text) &&
        DrawAsciiRunFromAtlas(run.x, run.y, run.color, run.text)) {
      ++atlas_runs;
      continue;
    }
    DrawString(renderer, cache, run.x, run.y, run.color, run.text);
  }
  util::AddPerformanceCounter(util::PerfCounterId::RenderGlyphAtlasRuns, atlas_runs);
}

void SdlTtfTextBackend::DrawRuns(SDL_Renderer* renderer, StringTextureCache& cache,
                                 const TextRun* runs, std::size_t count) {
  if (renderer == nullptr || renderer != renderer_ || !glyph_atlas_enabled_ ||
      !is_gpu_renderer_ || !EnsureAtlasTexture() || atlas_texture_width_ <= 0 || atlas_texture_height_ <= 0) {
    TextRendererBackend::DrawRuns(renderer, cache, runs, count);
    return;
  }

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
      geom_vertices_.resize(mark);
      geom_indices_.resize(mark / 4 * 6);
    }
    DrawString(renderer, cache, run.x, run.y, run.color, run.text);
  }

  if (!geom_vertices_.empty()) {
    util::AddPerformanceCounter(util::PerfCounterId::RenderGlyphAtlasRuns, batched_runs);
    util::AddPerformanceCounter(util::PerfCounterId::RenderGlyphAtlasGlyphs,
                                geom_vertices_.size() / 4);
    if (!SDL_RenderGeometry(renderer, atlas_texture_, geom_vertices_.data(),
                            static_cast<int>(geom_vertices_.size()), geom_indices_.data(),
                            static_cast<int>(geom_indices_.size()))) {
      util::AddPerformanceCounter(util::PerfCounterId::RenderGlyphAtlasFallbacks);
    }
  }
}

const StringTextureCache::Entry* SdlTtfTextBackend::ResolveEntry(StringTextureCache& cache,
                                                                std::string_view text,
                                                                SDL_Color color) {
  constexpr std::size_t kMaxRenderTextChars = 8192;
  if (text.size() > kMaxRenderTextChars) {
    text = text.substr(0, kMaxRenderTextChars);
  }
  if (const StringTextureCache::Entry* cached = cache.Find(text, color); cached != nullptr) {
    return cached;
  }

  SurfacePtr surface;
  {
    util::PerformanceTrace::Scope raster_scope("render::TextBackend::RasterizeString");
    if (CanUseFastAscii(text)) {
      surface = BuildAsciiCompositeSurface(text, color);
    }
    if (surface == nullptr) {
      surface = BuildGridCompositeSurface(text, color);
    }
  }
  if (surface == nullptr) {
    return nullptr;
  }

  util::PerformanceTrace::Scope upload_scope("render::TextBackend::UploadStringTexture");
  SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface.get());
  if (texture == nullptr) {
    return nullptr;
  }
  SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
  return cache.Insert(text, color, texture, surface->w, surface->h);
}

}

#endif
