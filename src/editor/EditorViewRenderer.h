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

#include "editor/BracketScanner.h"
#include "editor/DiagnosticsStore.h"
#include "editor/FoldingModel.h"
#include "editor/IndentGuides.h"
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
              std::span<const PublishedDiagnostic> diagnostics = {},
              bool bracket_match_highlight_enabled = false,
              bool indent_guides_enabled = false,
              bool render_whitespace_enabled = false,
              const FoldingModel* folding_model = nullptr) const;

  // Test/diagnostic accessors for the bracket-match cache. The cache is keyed
  // on (viewport, layout_revision, primary_caret_line, primary_caret_column)
  // and reused across consecutive frames when nothing actionable changes.
  const std::optional<BracketMatchPair>& last_bracket_match_pair() const {
    return bracket_match_cache_.pair;
  }
  std::size_t bracket_match_cache_hits() const { return bracket_match_cache_.hits; }
  std::size_t bracket_match_cache_misses() const { return bracket_match_cache_.misses; }

  // Test/diagnostic accessors for the indent-guides cache. Cache is keyed on
  // (viewport, layout_revision, scroll_line, visible_rows_count, indent_width,
  //  caret_line) and reused across consecutive frames when nothing changes.
  const std::vector<IndentGuideRun>& last_indent_guide_runs() const {
    return indent_guides_cache_.runs;
  }
  std::size_t indent_guides_cache_hits() const { return indent_guides_cache_.hits; }
  std::size_t indent_guides_cache_misses() const { return indent_guides_cache_.misses; }

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

  struct BracketMatchCache {
    const TextViewport* viewport = nullptr;
    std::size_t layout_revision = 0;
    std::size_t caret_line = 0;
    std::size_t caret_column = 0;
    bool valid = false;
    std::optional<BracketMatchPair> pair;
    std::size_t hits = 0;
    std::size_t misses = 0;
  };
  mutable BracketMatchCache bracket_match_cache_;

  struct IndentGuidesCache {
    const TextViewport* viewport = nullptr;
    std::size_t layout_revision = 0;
    std::size_t fold_revision = 0;
    std::size_t scroll_line = 0;
    std::size_t visible_rows_count = 0;
    std::size_t indent_width = 0;
    std::size_t caret_line = 0;
    bool valid = false;
    std::vector<IndentGuideRun> runs;
    std::size_t hits = 0;
    std::size_t misses = 0;
  };
  mutable IndentGuidesCache indent_guides_cache_;
};

}  // namespace microide::editor
