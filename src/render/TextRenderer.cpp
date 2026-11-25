#include "render/TextRenderer.h"

#include "render/PaintElement.h"

#include <algorithm>
#include <optional>
#include <vector>

#include "util/PerformanceCounters.h"
#include "util/StartupTrace.h"
#include "util/StringUtil.h"

namespace microide::render {

namespace {

constexpr std::size_t kWidthCacheCapacity = 4096;

}

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
  for (const std::unique_ptr<TextRenderer>& face : faces_) {
    if (face != nullptr) {
      face->EnsureInitialized(renderer, presentation_scale_x, presentation_scale_y);
    }
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
  for (const std::unique_ptr<TextRenderer>& face : faces_) {
    if (face != nullptr) {
      FontRequest styled = font;
      styled.style = face->font_.style;
      (void) face->RequestFont(std::move(styled));
    }
  }
  font_ = std::move(font);
  return AcquireBackend();
}

bool TextRenderer::AcquireBackend() {
  const bool changed = pool_->Acquire(font_, backend_);
  if (changed) {
    ClearCaches();
  }
  return changed;
}

const TextRenderer& TextRenderer::Face(FontStyle style) const {
  if (style == FontStyle::Regular || style == font_.style) {
    return *this;
  }
  std::unique_ptr<TextRenderer>& face = faces_[static_cast<std::size_t>(style) - 1];
  if (face == nullptr) {
    face = std::make_unique<TextRenderer>(pool_);
    FontRequest styled = font_;
    styled.style = style;
    face->initialized_ = initialized_;
    (void) face->RequestFont(std::move(styled));
  }
  face->ValidateCaches();
  return *face;
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

  constexpr std::size_t kMaxMeasureBytes = 8192;
  if (text.size() > kMaxMeasureBytes) {
    ++width_cache_queries_;
    util::AddPerformanceCounter(util::PerfCounterId::RenderTextWidthCacheQueries);
    const std::string_view clipped =
        text.substr(0, util::Utf8ByteBudgetPrefixLength(text, kMaxMeasureBytes));
    return backend_->MeasureWidth(clipped);
  }

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

  static constexpr std::string_view kEllipsis = "\xE2\x80\xA6";
  const float ellipsis_width = MeasureWidth(kEllipsis);
  if (ellipsis_width >= max_width) {
    return {};
  }

  const float budget = max_width - ellipsis_width;

  thread_local std::vector<std::size_t> boundaries;
  boundaries.clear();
  for (std::size_t offset = 0; offset < text.size();) {
    offset += util::Utf8SequenceLength(text, offset);
    boundaries.push_back(offset);
  }

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
  if (PaintOverrunRecordingEnabled()) {
    NotePaint(renderer, SDL_FRect{x, y, MeasureWidth(text), LineHeight()});
  }
  backend_->DrawString(renderer, texture_cache_, x, y, color, text);
}

void TextRenderer::NoteRuns(SDL_Renderer* renderer, const TextRun* runs, std::size_t count) const {
  if (!PaintOverrunRecordingEnabled()) {
    return;
  }
  for (std::size_t i = 0; i < count; ++i) {
    if (!runs[i].text.empty()) {
      NotePaint(renderer, SDL_FRect{runs[i].x, runs[i].y, MeasureWidth(runs[i].text), LineHeight()});
    }
  }
}

void TextRenderer::DrawRuns(SDL_Renderer* renderer, const TextRun* runs,
                            std::size_t count) const {
  if (renderer == nullptr || runs == nullptr || count == 0) {
    return;
  }
  ValidateCaches();
  NoteRuns(renderer, runs, count);
  backend_->DrawRuns(renderer, texture_cache_, runs, count);
}

void TextRenderer::DrawEphemeralRuns(SDL_Renderer* renderer, const TextRun* runs,
                                     std::size_t count) const {
  if (renderer == nullptr || runs == nullptr || count == 0) {
    return;
  }
  ValidateCaches();
  NoteRuns(renderer, runs, count);
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
  if (PaintOverrunRecordingEnabled()) {
    NotePaint(renderer, SDL_FRect{x, y, MeasureWidth(text), LineHeight()});
  }
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
  ++metrics_generation_;
}

void TextRenderer::RememberMeasuredWidth(std::string text, float width) const {
  auto [it, inserted] = width_cache_.emplace(std::move(text), width);
  if (!inserted) {
    it->second = width;
    return;
  }

  width_cache_order_.push_back(std::string_view(it->first));
  while (width_cache_order_.size() > kWidthCacheCapacity) {
    if (auto evict_it = width_cache_.find(width_cache_order_.front());
        evict_it != width_cache_.end()) {
      width_cache_.erase(evict_it);
    }
    width_cache_order_.pop_front();
  }
}

}
