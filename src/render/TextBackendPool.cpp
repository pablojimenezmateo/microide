#include "render/TextBackendPool.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <utility>

#include "render/DebugTextBackend.h"
#include "util/StartupTrace.h"

#if MICROIDE_HAS_SDL3_TTF
#include "render/SdlTtfTextBackend.h"
#endif

namespace microide::render {

namespace {

std::unique_ptr<TextRendererBackend> CreatePlatformBackend(SDL_Renderer* renderer) {
  util::StartupTrace::Scope trace_scope("TextBackendPool::CreatePlatformBackend");
#if MICROIDE_HAS_SDL3_TTF
  if (auto backend = SdlTtfTextBackend::Create(renderer); backend != nullptr) {
    return backend;
  }
  SDL_Log("microide text: SDL_ttf backend unavailable; falling back to SDL debug text");
#else
  (void) renderer;
  SDL_Log("microide text: built without SDL_ttf; falling back to SDL debug text");
#endif
  return std::make_unique<DebugTextBackend>();
}

// Returns whether the family changed; the caller knows whether the size did.
bool ApplyFont(TextRendererBackend& backend, const FontRequest& font) {
  const bool family_changed = backend.SetFontFamily(font.family);
  backend.SetFontPointSize(font.point_size);
  return family_changed;
}

}  // namespace

float FontRequest::ClampPointSize(float points) {
  return std::clamp(points, kMinFontPointSize, kMaxFontPointSize);
}

TextBackendPool::TextBackendPool() : TextBackendPool(Factory(&CreatePlatformBackend)) {}

TextBackendPool::TextBackendPool(Factory factory)
    : factory_(std::move(factory)), unbound_stand_in_(std::make_shared<DebugTextBackend>()) {}

void TextBackendPool::Bind(SDL_Renderer* renderer, float scale_x, float scale_y) {
  if (!bound_) {
    bound_ = true;
    renderer_ = renderer;
  } else if (scale_x == scale_x_ && scale_y == scale_y_) {
    return;
  }
  scale_x_ = scale_x;
  scale_y_ = scale_y;
  ++scale_epoch_;
  for (const Entry& entry : entries_) {
    if (const std::shared_ptr<TextRendererBackend> backend = entry.backend.lock()) {
      backend->SetPresentationScale(scale_x_, scale_y_);
    }
  }
}

bool TextBackendPool::Acquire(const FontRequest& font, std::shared_ptr<TextRendererBackend>& held) {
  if (!bound_) {
    const bool changed = held != unbound_stand_in_;
    held = unbound_stand_in_;
    return changed;
  }

  std::erase_if(entries_, [](const Entry& entry) { return entry.backend.expired(); });
  for (const Entry& entry : entries_) {
    if (entry.font == font) {
      std::shared_ptr<TextRendererBackend> shared = entry.backend.lock();
      if (shared == held) {
        return false;
      }
      held = std::move(shared);
      return true;
    }
  }

  // Nobody else draws with `held`, so it can take the new font itself: a size
  // change on a lone backend resizes its open fonts rather than opening new ones.
  if (held.use_count() == 1) {
    if (Entry* entry = FindEntry(held.get()); entry != nullptr) {
      const bool size_changed = entry->font.point_size != font.point_size;
      entry->font = font;
      return ApplyFont(*held, font) || size_changed;
    }
  }

  std::shared_ptr<TextRendererBackend> created = factory_(renderer_);
  created->SetPresentationScale(scale_x_, scale_y_);
  (void) ApplyFont(*created, font);
  entries_.push_back(Entry{.font = font, .backend = created});
  held = std::move(created);
  return true;
}

std::size_t TextBackendPool::LiveBackendCount() const {
  return static_cast<std::size_t>(std::count_if(
      entries_.begin(), entries_.end(), [](const Entry& entry) { return !entry.backend.expired(); }));
}

TextBackendPool::Entry* TextBackendPool::FindEntry(const TextRendererBackend* backend) {
  const auto it = std::find_if(entries_.begin(), entries_.end(), [backend](const Entry& entry) {
    return entry.backend.lock().get() == backend;
  });
  return it != entries_.end() ? &*it : nullptr;
}

}  // namespace microide::render
