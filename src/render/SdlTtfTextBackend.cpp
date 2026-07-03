#include "render/SdlTtfTextBackend.h"

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
#include "util/StartupTrace.h"

namespace microide::render {

namespace {

// Bounds for the runtime-configurable font point size. These mirror the
// `editor.font_size` setting range (8..32) so a stored project value can never
// drive the backend outside a sane glyph range.
constexpr float kMinFontPointSize = 8.0f;
constexpr float kMaxFontPointSize = 32.0f;
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

  if (!OpenPrimaryFont(font_path)) {
    SDL_Log("microide text: TTF_OpenFont failed for %s: %s", font_path.string().c_str(),
            SDL_GetError());
    return false;
  }
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

void SdlTtfTextBackend::ApplyFontSizeAtCurrentScale() {
  if (font_ == nullptr) {
    return;
  }
  const int hdpi = std::max(1, static_cast<int>(std::lround(
                                  72.0f * std::max(kMinPresentationScale, presentation_scale_x_))));
  const int vdpi = std::max(1, static_cast<int>(std::lround(
                                  72.0f * std::max(kMinPresentationScale, presentation_scale_y_))));
  if (!TTF_SetFontSizeDPI(font_, font_point_size_, hdpi, vdpi)) {
    return;
  }
  for (TTF_Font* fallback_font : fallback_fonts_) {
    if (fallback_font != nullptr) {
      TTF_SetFontSizeDPI(fallback_font, font_point_size_, hdpi, vdpi);
    }
  }
  // ClearCache also drops the ASCII coverage atlas and the GPU atlas texture, so
  // both the composite and batched-text paths rebuild against the new glyph size.
  ClearCache();
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
  // Record the request up front so an unresolvable or unopenable family is
  // remembered and the early-out above suppresses a full font re-lookup on every
  // subsequent frame (ApplyLiveSettings / ApplyTerminalFontPreferences call this
  // per frame). Failure paths keep the current font; only success rebuilds glyphs.
  requested_font_family_ = std::string(family);
  const std::filesystem::path resolved = ResolveFamilyToFile(family);
  if (resolved.empty()) {
    // Keep the current font; an unresolved family must never brick text rendering.
    SDL_Log("microide text: font family \"%.*s\" not found; keeping current font",
            static_cast<int>(family.size()), family.data());
    return false;
  }
  if (resolved == font_path_ && font_ != nullptr) {
    // Same underlying file (e.g. empty family resolving to the current default):
    // the request is already recorded; nothing visually changed.
    return false;
  }
  if (!OpenPrimaryFont(resolved)) {
    SDL_Log("microide text: failed to open font family \"%.*s\" (%s); keeping current font",
            static_cast<int>(family.size()), family.data(), resolved.string().c_str());
    return false;
  }
  // Fallbacks are relative to the new primary; reload, then rebuild caches/metrics
  // for the new glyph shapes at the current size/scale.
  LoadFallbackFonts();
  ApplyFontSizeAtCurrentScale();
  return true;
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

namespace {

// Normalize a family/stem/extension for fuzzy matching: lowercase, drop spaces,
// hyphens, underscores and dots so "JetBrains Mono" matches "JetBrainsMono-Regular"
// and an extension like ".ttf" normalizes to "ttf" (not ".ttf").
std::string NormalizeFontToken(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    if (c == ' ' || c == '-' || c == '_' || c == '.') {
      continue;
    }
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

// True when `token` (already lowercased, no separators) is a weight/style word we
// want to strip from a file stem to recover the family name for the picker list.
bool IsFontStyleToken(std::string_view token) {
  static constexpr std::array<std::string_view, 24> kStyles = {
      "regular",    "bold",       "italic",    "oblique",   "bolditalic",
      "light",      "medium",     "semibold",  "demibold",  "thin",
      "black",      "heavy",      "book",      "roman",     "extralight",
      "ultralight", "extrabold",  "ultrabold", "condensed", "semilight",
      "retina",     "text",       "display",   "variablefont"};
  return std::find(kStyles.begin(), kStyles.end(), token) != kStyles.end();
}

// Derive a human-facing family name from a font file stem for the picker list:
// drop a trailing style token (after the last '-'/'_'), then split camelCase and
// digit boundaries into words. The exact spelling is cosmetic — selection routes
// back through ResolveFamilyToFile's fuzzy matcher, which ignores spacing/case.
std::string FontDisplayNameFromStem(std::string_view stem) {
  std::string base(stem);
  // Drop variable-font axis tags like "[wght]" / "[wght,wdth]".
  if (const std::size_t bracket = base.find('['); bracket != std::string::npos) {
    base = base.substr(0, bracket);
  }
  while (!base.empty() && (base.back() == '-' || base.back() == '_' || base.back() == ' ')) {
    base.pop_back();
  }
  if (const std::size_t sep = base.find_last_of("-_"); sep != std::string::npos) {
    if (IsFontStyleToken(NormalizeFontToken(base.substr(sep + 1)))) {
      base = base.substr(0, sep);
    }
  }
  // Remaining '-'/'_' separators become spaces.
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
      const bool camel = std::islower(prev) && std::isupper(cur);
      const bool digit_edge = std::isalpha(prev) && std::isdigit(cur);
      if (camel || digit_edge) {
        out.push_back(' ');
      }
    }
    out.push_back(c);
  }
  // Collapse any accidental double spaces from the substitutions above.
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

}  // namespace

std::vector<std::filesystem::path> SdlTtfTextBackend::FontSearchRoots() {
  // This scan is only the fallback for builds without fontconfig. To stay portable
  // across distros we derive roots from the XDG base directories (which NixOS,
  // Flatpak, Nix home-manager, Arch, Fedora, etc. all populate) rather than a fixed
  // FHS list, then add well-known extras as belt-and-suspenders.
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

  // 1. Per-user: $XDG_DATA_HOME/fonts (default ~/.local/share/fonts) + legacy ~/.fonts.
  if (const char* xdg_data_home = SDL_getenv("XDG_DATA_HOME");
      xdg_data_home != nullptr && xdg_data_home[0] != '\0') {
    add(std::filesystem::path(xdg_data_home) / "fonts");
  } else if (home != nullptr) {
    add(std::filesystem::path(home) / ".local/share/fonts");
  }
  if (home != nullptr) {
    add(std::filesystem::path(home) / ".fonts");
    // Nix home-manager installs into the user profile.
    add(std::filesystem::path(home) / ".nix-profile/share/fonts");
  }

  // 2. System: each $XDG_DATA_DIRS entry + "/fonts" (default /usr/local/share:/usr/share).
  //    On NixOS this carries the current-system + profile store paths; on Flatpak
  //    the runtime's /app + host paths.
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

  // 3. Well-known extras not always present in XDG_DATA_DIRS: the NixOS system
  //    profile, the Flatpak host mount, and the FHS defaults (in case XDG is unset
  //    or minimal in the environment we were launched from).
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
  // Prefer fontconfig: it yields real family names (properly cased/spaced) and
  // dedupes weights for us.
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
  // Fallback: scan the standard font trees and derive family names from stems.
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
          return std::tolower(x) < std::tolower(y);
        });
  };
  const auto ci_equal = [](const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
             return std::tolower(x) == std::tolower(y);
           });
  };
  std::sort(families.begin(), families.end(), ci_less);
  families.erase(std::unique(families.begin(), families.end(), ci_equal), families.end());
  return families;
}

std::filesystem::path SdlTtfTextBackend::ResolveFamilyToFile(std::string_view family) {
  if (family.empty()) {
    return LocateFontFile();
  }
  // Allow an explicit path (absolute or existing) so power users can point straight
  // at a font file.
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
        FcChar8* file = nullptr;
        if (FcPatternGetString(matched, FC_FILE, 0, &file) == FcResultMatch && file != nullptr) {
          result = std::filesystem::path(reinterpret_cast<const char*>(file));
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

  // Fallback: scan the standard font directories once per family (cheap: only on a
  // font-family setting change) and fuzzy-match the file stem. Prefer an exact
  // stem match, then a "<family>-regular" variant, then any containing match.
  const std::string needle = NormalizeFontToken(family);
  if (needle.empty()) {
    return LocateFontFile();
  }
  std::filesystem::path exact;
  std::filesystem::path regular;
  std::filesystem::path contains;
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
      if (contains.empty() && stem.find(needle) != std::string::npos) {
        contains = path;
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
  return contains;  // empty when nothing matched -> caller keeps the current font
}

std::vector<std::filesystem::path> SdlTtfTextBackend::LocateFallbackFontFiles(
    const std::filesystem::path& primary_font) {
  // Broad-Unicode / symbol fallback fonts, resolved once and cached: the file set
  // is session-stable, and LoadFallbackFonts runs on every font-family change, so
  // re-resolving each time would add avoidable work. For each fallback we try a
  // fast known Debian/Ubuntu path first (a cheap exists() check, no scan); only when
  // that is absent (Arch, Fedora, NixOS, Flatpak, …) do we resolve the family by
  // name via ResolveFamilyToFile (fontconfig when available, else the XDG scan), so
  // symbol/CJK glyph fallback is portable rather than Debian-only.
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
      if (candidate.empty() || !std::filesystem::exists(candidate)) {
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
      if (std::filesystem::exists(debian)) {
        push(debian);  // fast path: no directory scan / fontconfig query
      } else {
        push(ResolveFamilyToFile(fallback.family));  // portable resolution
      }
    }
    return resolved;
  }();

  // Exclude the current primary (already the main font); return the rest in order.
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
  // Free any fallbacks registered against a previous primary before opening a new
  // set; otherwise repeated family switches leak TTF_Font handles and grow the
  // vector unbounded.
  CloseFallbackFonts();

  for (const auto& fallback_path : LocateFallbackFontFiles(font_path_)) {
    TTF_Font* fallback_font = TTF_OpenFont(fallback_path.string().c_str(), font_point_size_);
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
