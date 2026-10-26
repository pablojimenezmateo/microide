#include "render/TextRenderer.h"

#include <algorithm>
#include <optional>
#include <vector>

#include "util/PerformanceCounters.h"
#include "util/StartupTrace.h"
#include "util/StringUtil.h"

namespace microide::render {

namespace {

constexpr std::size_t kWidthCacheCapacity = 4096;

}  // namespace

TextRenderer::TextRenderer() : TextRenderer(std::make_shared<TextBackendPool>()) {}

TextRenderer::TextRenderer(std::shared_ptr<TextBackendPool> pool) : pool_(std::move(pool)) {
  (void) pool_->Acquire(font_, backend_);
  ClearCaches();
}

TextRenderer::~TextRenderer() = default;

void TextRenderer::EnsureInitialized(SDL_Renderer* renderer,
                                     float presentation_scale_x,
                                     float presentation_scale_y) {
  pool_->Bind(renderer, presentation_scale_x, presentation_scale_y);
  if (!initialized_) {
    util::StartupTrace::Scope trace_scope("TextRenderer::EnsureInitialized");
    initialized_ = true;
    (void) AcquireBackend();
  }
  ValidateCaches();
}

bool TextRenderer::SetFontPointSize(float points) {
  const float point_size = FontRequest::ClampPointSize(points);
  if (font_.point_size == point_size) {
    return false;
  }
  FontRequest font = font_;
  font.point_size = point_size;
  return RequestFont(std::move(font));
}

bool TextRenderer::SetFontFamily(std::string_view family) {
  if (font_.family == family) {
    return false;
  }
  FontRequest font = font_;
  font.family = std::string(family);
  return RequestFont(std::move(font));
}

bool TextRenderer::RequestFont(FontRequest font) {
  if (font == font_) {
    return false;
  }
  font_ = std::move(font);
  // Before the pool is bound this keeps the stand-in, and the request is
  // resolved by the first EnsureInitialized.
  return AcquireBackend();
}

bool TextRenderer::AcquireBackend() {
  const bool changed = pool_->Acquire(font_, backend_);
  if (changed) {
    ClearCaches();
  }
  return changed;
}

std::vector<std::string> TextRenderer::AvailableFontFamilies() const {
  return backend_->AvailableFontFamilies();
}

float TextRenderer::CharWidth() const {
  return backend_->CharWidth();
}

float TextRenderer::LineHeight() const {
  return backend_->LineHeight();
}

TextClipPadding TextRenderer::ClipPadding() const {
  return backend_->ClipPadding();
}

float TextRenderer::MeasureWidth(std::string_view text) const {
  if (text.empty()) {
    return 0.0f;
  }

  // TD-2026-07-17A-105: draw-time strings are already truncated to a bounded length by
  // the backend, but measurement passed the full (possibly untrusted, very large) string
  // to the shaper AND cached it as a key — so a huge status/hover/settings/notification
  // string could shape a large buffer and retain a large cache key up to the cache
  // capacity. For over-budget input, measure only a bounded UTF-8-boundary prefix (a
  // "clipped width" — clipped labels never render past it anyway) and never cache it.
  constexpr std::size_t kMaxMeasureBytes = 8192;  // matches the backend's draw truncation
  if (text.size() > kMaxMeasureBytes) {
    ++width_cache_queries_;
    util::AddPerformanceCounter(util::PerfCounterId::RenderTextWidthCacheQueries);
    const std::string_view clipped =
        text.substr(0, util::Utf8ByteBudgetPrefixLength(text, kMaxMeasureBytes));
    return backend_->MeasureWidth(clipped);
  }

  // Measurements the backend answers arithmetically are not memoized at all: the
  // memo would cost a heap copy of the string and an LRU slot to save one
  // multiply, and the strings arriving here are not only chrome labels — the
  // merge and compare surfaces measure every visible ROW of the document, so the
  // cache was being filled with document text and evicting the labels whose
  // measurement actually shapes (TD-2026-08-30-269). Counted as a hit so the
  // hit-rate statistic keeps meaning "measurements that did not shape".
  if (const std::optional<float> cheap = backend_->MeasureWidthIfCheap(text); cheap.has_value()) {
    ++width_cache_queries_;
    ++width_cache_hits_;
    util::AddPerformanceCounter(util::PerfCounterId::RenderTextWidthCacheQueries);
    util::AddPerformanceCounter(util::PerfCounterId::RenderTextWidthCacheHits);
    return *cheap;
  }

  ValidateCaches();
  ++width_cache_queries_;
  util::AddPerformanceCounter(util::PerfCounterId::RenderTextWidthCacheQueries);
  const auto cached = width_cache_.find(text);
  if (cached != width_cache_.end()) {
    ++width_cache_hits_;
    util::AddPerformanceCounter(util::PerfCounterId::RenderTextWidthCacheHits);
    return cached->second;
  }

  const float width = backend_->MeasureWidth(text);
  RememberMeasuredWidth(std::string(text), width);
  return width;
}

std::string_view TextRenderer::BackendName() const {
  return backend_->Name();
}

std::string TextRenderer::TruncateToWidth(std::string_view text, float max_width) const {
  return std::string(TruncateToWidthEphemeralView(text, max_width));
}

std::string_view TextRenderer::TruncateToWidthEphemeralView(std::string_view text, float max_width) const {
  if (max_width <= 0.0f || text.empty()) {
    return {};
  }
  if (MeasureWidth(text) <= max_width) {
    return text;
  }

  // U+2026, not three periods: one glyph instead of three leaves room for two more
  // characters of the label being truncated, and it matches the "…" every label that
  // spells its own ellipsis already uses ("Open File…", "Discard…"). Non-ASCII text
  // skips the glyph atlas for the whole string, which costs nothing that matters here
  // — truncation only ever runs on chrome labels, and each result is texture-cached.
  static constexpr std::string_view kEllipsis = "\xE2\x80\xA6";
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
  if (renderer == nullptr || text.empty()) {
    return;
  }
  ValidateCaches();
  backend_->DrawString(renderer, texture_cache_, x, y, color, text);
}

void TextRenderer::DrawRuns(SDL_Renderer* renderer, const TextRun* runs,
                            std::size_t count) const {
  if (renderer == nullptr || runs == nullptr || count == 0) {
    return;
  }
  ValidateCaches();
  backend_->DrawRuns(renderer, texture_cache_, runs, count);
}

void TextRenderer::DrawEphemeralRuns(SDL_Renderer* renderer, const TextRun* runs,
                                     std::size_t count) const {
  if (renderer == nullptr || runs == nullptr || count == 0) {
    return;
  }
  ValidateCaches();
  backend_->DrawEphemeralRuns(renderer, texture_cache_, runs, count);
}

void TextRenderer::DrawStringOn(SDL_Renderer* renderer,
                                float x,
                                float y,
                                SDL_Color color,
                                SDL_Color background,
                                std::string_view text) const {
  if (renderer == nullptr || text.empty()) {
    return;
  }
  ValidateCaches();
  backend_->DrawStringOn(renderer, texture_cache_, x, y, color, background, text);
}

void TextRenderer::ValidateCaches() const {
  if (width_cache_backend_ != backend_.get() ||
      width_cache_scale_epoch_ != pool_->ScaleEpoch()) {
    ClearCaches();
  }
}

void TextRenderer::ClearCaches() const {
  width_cache_.clear();
  width_cache_order_.clear();
  texture_cache_.Clear();
  width_cache_backend_ = backend_.get();
  width_cache_scale_epoch_ = pool_->ScaleEpoch();
  // Metrics changed (or first init): signal external MeasureWidth-derived caches to drop.
  ++metrics_generation_;
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
