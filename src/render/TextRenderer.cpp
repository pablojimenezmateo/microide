#include "render/TextRenderer.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <vector>

#include "render/DebugTextBackend.h"
#include "util/PerformanceCounters.h"
#include "util/StartupTrace.h"
#include "util/StringUtil.h"

#if MICROIDE_HAS_SDL3_TTF
#include "render/SdlTtfTextBackend.h"
#endif

namespace microide::render {

namespace {

constexpr std::size_t kWidthCacheCapacity = 4096;

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
    } else {
      SDL_Log("microide text: SDL_ttf backend unavailable; falling back to SDL debug text");
    }
#else
    (void) renderer;
    SDL_Log("microide text: built without SDL_ttf; falling back to SDL debug text");
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

void TextRenderer::SetFontPointSize(float points) {
  if (backend_ == nullptr) {
    return;
  }
  backend_->SetFontPointSize(points);
  // Glyph advances change with the point size, so the measured-width cache is
  // stale; drop it and re-anchor metrics on the next query.
  ClearWidthCache();
  width_cache_initialized_ = false;
}

bool TextRenderer::SetFontFamily(std::string_view family) {
  if (backend_ == nullptr) {
    return false;
  }
  if (!backend_->SetFontFamily(family)) {
    return false;
  }
  // A different typeface changes every glyph advance; invalidate the width cache.
  ClearWidthCache();
  width_cache_initialized_ = false;
  return true;
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
  util::AddPerformanceCounter(util::PerfCounterId::RenderTextWidthCacheQueries);
  const auto cached = width_cache_.find(text);
  if (cached != width_cache_.end()) {
    ++width_cache_hits_;
    util::AddPerformanceCounter(util::PerfCounterId::RenderTextWidthCacheHits);
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
  return std::string(TruncateToWidthView(text, max_width));
}

std::string_view TextRenderer::TruncateToWidthView(std::string_view text, float max_width) const {
  if (max_width <= 0.0f || text.empty()) {
    return {};
  }
  if (MeasureWidth(text) <= max_width) {
    return text;
  }

  static constexpr std::string_view kEllipsis = "...";
  const float ellipsis_width = MeasureWidth(kEllipsis);
  if (ellipsis_width >= max_width) {
    return {};
  }

  const float budget = max_width - ellipsis_width;

  // Collect UTF-8 code-point boundary offsets. Thread-local scratch keeps the
  // per-call truncation path allocation-free after warmup.
  thread_local std::vector<std::size_t> boundaries;
  boundaries.clear();
  for (std::size_t offset = 0; offset < text.size();) {
    offset += util::Utf8SequenceLength(text, offset);
    boundaries.push_back(offset);
  }

  // Binary search for the longest prefix that fits within budget, measuring
  // only the prefix (not prefix+ellipsis) to avoid per-step string allocation.
  std::size_t fit_length = 0;
  std::size_t low = 0;
  std::size_t high = boundaries.size();
  while (low < high) {
    const std::size_t mid = low + (high - low) / 2;
    const std::string_view prefix = text.substr(0, boundaries[mid]);
    if (MeasureWidth(prefix) <= budget) {
      fit_length = boundaries[mid];
      low = mid + 1;
    } else {
      high = mid;
    }
  }

  thread_local std::string truncated;
  truncated.assign(text.substr(0, fit_length));
  truncated.append(kEllipsis);
  return truncated;
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

void TextRenderer::DrawRuns(SDL_Renderer* renderer, const TextRun* runs,
                            std::size_t count) const {
  if (backend_ == nullptr || renderer == nullptr || runs == nullptr || count == 0) {
    return;
  }
  backend_->DrawRuns(renderer, runs, count);
}

bool TextRenderer::BatchesRuns() const {
  return backend_ != nullptr && backend_->BatchesRuns();
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

  // Store a view into the map key — unordered_map element references stay valid across rehashes,
  // so this is both safe and ~2× more memory-efficient than carrying duplicate std::string copies.
  width_cache_order_.push_back(std::string_view(it->first));
  while (width_cache_order_.size() > kWidthCacheCapacity) {
    // unordered_map::erase is not transparent; transparent find() yields an iterator that erase
    // does accept. Avoids materializing a temporary std::string just to drop the entry.
    if (auto evict_it = width_cache_.find(width_cache_order_.front());
        evict_it != width_cache_.end()) {
      width_cache_.erase(evict_it);
    }
    width_cache_order_.pop_front();
  }
}

}  // namespace microide::render
