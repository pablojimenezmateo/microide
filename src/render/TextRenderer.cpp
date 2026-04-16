#include "render/TextRenderer.h"

#include <algorithm>
#include <vector>

#include "render/DebugTextBackend.h"
#include "util/StartupTrace.h"

#if MICROIDE_HAS_SDL3_TTF
#include "render/SdlTtfTextBackend.h"
#endif

namespace microide::render {

namespace {

constexpr std::size_t kWidthCacheCapacity = 512;

std::size_t Utf8SequenceLength(std::string_view text, std::size_t offset) {
  if (offset >= text.size()) {
    return 0;
  }

  const unsigned char lead = static_cast<unsigned char>(text[offset]);
  if (lead <= 0x7F) {
    return 1;
  }

  auto continuation = [&](std::size_t count) {
    if (offset + count >= text.size()) {
      return false;
    }
    for (std::size_t i = 1; i <= count; ++i) {
      const unsigned char byte = static_cast<unsigned char>(text[offset + i]);
      if ((byte & 0xC0) != 0x80) {
        return false;
      }
    }
    return true;
  };

  if (lead >= 0xC2 && lead <= 0xDF && continuation(1)) {
    return 2;
  }
  if (lead == 0xE0 && continuation(2)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0xA0 && second <= 0xBF) {
      return 3;
    }
  }
  if (((lead >= 0xE1 && lead <= 0xEC) || (lead >= 0xEE && lead <= 0xEF)) && continuation(2)) {
    return 3;
  }
  if (lead == 0xED && continuation(2)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0x80 && second <= 0x9F) {
      return 3;
    }
  }
  if (lead == 0xF0 && continuation(3)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0x90 && second <= 0xBF) {
      return 4;
    }
  }
  if (lead >= 0xF1 && lead <= 0xF3 && continuation(3)) {
    return 4;
  }
  if (lead == 0xF4 && continuation(3)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0x80 && second <= 0x8F) {
      return 4;
    }
  }

  return 1;
}

}  // namespace

TextRenderer::TextRenderer()
    : backend_(std::make_unique<DebugTextBackend>()) {}

TextRenderer::~TextRenderer() = default;

void TextRenderer::EnsureInitialized(SDL_Renderer* renderer,
                                     float presentation_scale_x,
                                     float presentation_scale_y) {
  if (!attempted_optional_backend_) {
    util::StartupTrace::Scope trace_scope("TextRenderer::EnsureInitialized");
    attempted_optional_backend_ = true;

#if MICROIDE_HAS_SDL3_TTF
    if (auto backend = SdlTtfTextBackend::Create(renderer); backend != nullptr) {
      backend_ = std::move(backend);
    }
#else
    (void) renderer;
#endif
  }

  if (backend_ != nullptr) {
    backend_->SetPresentationScale(presentation_scale_x, presentation_scale_y);
  }

  const std::string backend_name = backend_ != nullptr ? std::string(backend_->Name()) : "unknown";
  if (!width_cache_initialized_ || width_cache_backend_name_ != backend_name ||
      width_cache_scale_x_ != presentation_scale_x || width_cache_scale_y_ != presentation_scale_y) {
    ClearWidthCache();
    width_cache_backend_name_ = backend_name;
    width_cache_scale_x_ = presentation_scale_x;
    width_cache_scale_y_ = presentation_scale_y;
    width_cache_initialized_ = true;
  }
}

float TextRenderer::CharWidth() const {
  return backend_ != nullptr ? backend_->CharWidth() : 8.0f;
}

float TextRenderer::LineHeight() const {
  return backend_ != nullptr ? backend_->LineHeight() : 14.0f;
}

TextClipPadding TextRenderer::ClipPadding() const {
  return backend_ != nullptr ? backend_->ClipPadding() : TextClipPadding{};
}

float TextRenderer::MeasureWidth(std::string_view text) const {
  if (text.empty()) {
    return 0.0f;
  }

  ++width_cache_queries_;
  const auto cached = width_cache_.find(std::string(text));
  if (cached != width_cache_.end()) {
    ++width_cache_hits_;
    return cached->second;
  }

  const float width = backend_ != nullptr ? backend_->MeasureWidth(text)
                                          : static_cast<float>(text.size()) * 8.0f;
  RememberMeasuredWidth(std::string(text), width);
  return width;
}

std::string_view TextRenderer::BackendName() const {
  return backend_ != nullptr ? backend_->Name() : "unknown";
}

std::string TextRenderer::TruncateToWidth(std::string_view text, float max_width) const {
  if (max_width <= 0.0f || text.empty()) {
    return {};
  }
  if (MeasureWidth(text) <= max_width) {
    return std::string(text);
  }

  static constexpr std::string_view kEllipsis = "...";
  const float ellipsis_width = MeasureWidth(kEllipsis);
  if (ellipsis_width >= max_width) {
    return {};
  }

  std::vector<std::size_t> boundaries;
  boundaries.reserve(text.size());
  for (std::size_t offset = 0; offset < text.size();) {
    offset += Utf8SequenceLength(text, offset);
    boundaries.push_back(offset);
  }

  std::size_t fit_length = 0;
  std::size_t low = 0;
  std::size_t high = boundaries.size();
  while (low < high) {
    const std::size_t mid = low + (high - low) / 2;
    const std::size_t prefix_length = boundaries[mid];
    const std::string candidate =
        std::string(text.substr(0, prefix_length)) + std::string(kEllipsis);
    if (MeasureWidth(candidate) <= max_width) {
      fit_length = prefix_length;
      low = mid + 1;
    } else {
      high = mid;
    }
  }

  return std::string(text.substr(0, fit_length)) + std::string(kEllipsis);
}

TextRendererCacheStats TextRenderer::CacheStats() const {
  return TextRendererCacheStats{
      .width_cache_queries = width_cache_queries_,
      .width_cache_hits = width_cache_hits_,
  };
}

void TextRenderer::ResetCacheStats() const {
  width_cache_queries_ = 0;
  width_cache_hits_ = 0;
}

void TextRenderer::DrawString(SDL_Renderer* renderer,
                              float x,
                              float y,
                              SDL_Color color,
                              std::string_view text) const {
  if (backend_ == nullptr || renderer == nullptr || text.empty()) {
    return;
  }
  backend_->DrawString(renderer, x, y, color, text);
}

void TextRenderer::DrawStringOn(SDL_Renderer* renderer,
                                float x,
                                float y,
                                SDL_Color color,
                                SDL_Color background,
                                std::string_view text) const {
  if (backend_ == nullptr || renderer == nullptr || text.empty()) {
    return;
  }
  backend_->DrawStringOn(renderer, x, y, color, background, text);
}

void TextRenderer::ClearWidthCache() const {
  width_cache_.clear();
  width_cache_order_.clear();
}

void TextRenderer::RememberMeasuredWidth(std::string text, float width) const {
  auto [it, inserted] = width_cache_.emplace(std::move(text), width);
  if (!inserted) {
    it->second = width;
    return;
  }

  width_cache_order_.push_back(it->first);
  while (width_cache_order_.size() > kWidthCacheCapacity) {
    width_cache_.erase(width_cache_order_.front());
    width_cache_order_.pop_front();
  }
}

}  // namespace microide::render
