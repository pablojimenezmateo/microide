#pragma once

#include <SDL3/SDL.h>

#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "editor/DiagnosticsStore.h"
#include "editor/TextViewport.h"
#include "render/TextRenderer.h"
#include "render/Theme.h"

namespace microide::editor {

struct EditorViewMetrics {
  float gutter_width = 56.0f;
  float text_x = 0.0f;
  float first_line_y = 0.0f;
  float line_height = 14.0f;
  std::size_t visible_rows = 1;
  std::size_t visible_columns = 8;
};

struct EditorBlameLine {
  std::size_t line_index = 0;
  SDL_FRect rect{};
  std::string text;
  std::string commit_id;
  std::string author;
  std::string summary;
  std::string date;
  bool interactive = false;
};

struct EditorBlameOverlay {
  bool visible = false;
  std::vector<EditorBlameLine> lines;
};

class EditorViewRenderer {
 public:
  static EditorViewMetrics ComputeMetrics(const render::TextRenderer& text_renderer,
                                          const TextViewport& viewport,
                                          const SDL_FRect& rect);

  void Render(SDL_Renderer* renderer,
              const render::TextRenderer& text_renderer,
              const render::Theme& theme,
              TextViewport& viewport,
              const SDL_FRect& rect,
              bool draw_caret = true,
              std::string_view search_query = {},
              const std::optional<SelectionRange>& active_search_match = std::nullopt,
              const std::optional<EditorBlameOverlay>& blame_overlay = std::nullopt,
              std::span<const PublishedDiagnostic> diagnostics = {}) const;

 private:
  struct SearchMatchCacheKey {
    const TextViewport* viewport = nullptr;
    std::size_t layout_revision = 0;
    std::size_t line_index = 0;
    std::string query;

    bool operator==(const SearchMatchCacheKey& other) const noexcept {
      return viewport == other.viewport && layout_revision == other.layout_revision &&
             line_index == other.line_index && query == other.query;
    }
  };

  struct SearchMatchCacheKeyHash {
    std::size_t operator()(const SearchMatchCacheKey& key) const noexcept {
      std::size_t h = std::hash<const TextViewport*>{}(key.viewport);
      h ^= key.layout_revision * 2654435761ULL + 0x9e3779b9ULL + (h << 6) + (h >> 2);
      h ^= key.line_index * 2654435761ULL + 0x9e3779b9ULL + (h << 6) + (h >> 2);
      h ^= std::hash<std::string>{}(key.query) + 0x9e3779b9ULL + (h << 6) + (h >> 2);
      return h;
    }
  };

  static constexpr std::size_t kSearchMatchCacheLimit = 512;
  mutable std::unordered_map<SearchMatchCacheKey,
                             std::vector<std::pair<std::size_t, std::size_t>>,
                             SearchMatchCacheKeyHash>
      search_match_cache_;
  mutable std::deque<SearchMatchCacheKey> search_match_cache_order_;
};

}  // namespace microide::editor
