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
#include "editor/DecoratedTextGridRenderer.h"
#include "editor/DiagnosticsStore.h"
#include "editor/EolDecorationLayout.h"
#include "editor/GutterIconRegistry.h"
#include "editor/PluginDecorationStore.h"
#include "editor/RowDecorationBuilder.h"
#include "editor/EditorViewModel.h"
#include "editor/FoldingModel.h"
#include "editor/IndentGuides.h"
#include "editor/TextViewport.h"
#include "editor/WelcomeView.h"
#include "render/TextRenderer.h"
#include "render/Theme.h"

namespace microide::editor {

struct EditorViewMetrics {
  float gutter_width = 56.0f;
  float text_x = 0.0f;
  /// Y offset of optional sticky-scroll band (pane top inset).
  float sticky_band_top_y = 0.0f;
  /// First document row paints at this Y (below any sticky-scroll band rows).
  float first_line_y = 0.0f;
  float line_height = 14.0f;
  /// Number of reserved sticky band rows (not additional document rows).
  std::size_t sticky_scroll_rows = 0;
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

SDL_FRect FoldGutterMarkerRect(float gutter_x,
                               float gutter_width,
                               float row_y,
                               float line_height);

class EditorViewRenderer {
 public:
  static EditorViewMetrics ComputeMetrics(const render::TextRenderer& text_renderer,
                                          const TextViewport& viewport,
                                          const SDL_FRect& rect,
                                          std::size_t sticky_scroll_rows = 0,
                                          bool show_line_numbers = true);

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
              const EditorViewModel* view_model = nullptr,
              bool bracket_match_highlight_enabled = false,
              bool indent_guides_enabled = false,
              bool render_whitespace_enabled = false,
              const FoldingModel* folding_model = nullptr,
              const WelcomeViewModel* welcome_view = nullptr,
              const FileDecorations* plugin_decorations = nullptr,
              bool show_line_numbers = true) const;

  // Test/diagnostic accessors for the bracket-match cache. The cache is keyed
  // on (viewport, content_revision, primary_caret_line, primary_caret_column)
  // and reused across consecutive frames when nothing actionable changes.
  const std::optional<BracketMatchPair>& last_bracket_match_pair() const {
    return last_bracket_match_pair_;
  }
  std::size_t bracket_match_cache_hits() const { return bracket_match_cache_hits_; }
  std::size_t bracket_match_cache_misses() const { return bracket_match_cache_misses_; }

  // Test/diagnostic accessors for the indent-guides cache. Cache is keyed on
  // (viewport, content_revision, layout_shape_revision, scroll_line,
  //  visible_rows_count, indent_width, caret_line, fold_revision,
  //  folding_model revision for active emphasis) and reused across
  // consecutive frames when nothing changes.
  const std::vector<IndentGuideRun>& last_indent_guide_runs() const {
    return indent_guides_cache_last_runs_ != nullptr ? *indent_guides_cache_last_runs_
                                                     : indent_guides_cache_empty_runs_;
  }
  std::size_t indent_guides_cache_hits() const { return indent_guides_cache_hits_; }
  std::size_t indent_guides_cache_misses() const { return indent_guides_cache_misses_; }
  const std::vector<FoldGutterMark>& last_fold_gutter_marks() const { return last_fold_gutter_marks_; }

 private:
  // Borrowed lookup key: the per-row paint loop probes the cache with the
  // frame's lowered-query string_view, so a cache hit allocates nothing. The
  // owning std::string is materialized only when a miss inserts (see CacheKey).
  struct SearchMatchCacheKeyView {
    const TextViewport* viewport = nullptr;
    std::uint64_t content_revision = 0;
    std::size_t line_index = 0;
    std::string_view query;
  };

  struct SearchMatchCacheKey {
    const TextViewport* viewport = nullptr;
    std::uint64_t content_revision = 0;
    std::size_t line_index = 0;
    std::string query;

    operator SearchMatchCacheKeyView() const noexcept {
      return SearchMatchCacheKeyView{viewport, content_revision, line_index, query};
    }
  };

  struct SearchMatchCacheKeyHash {
    using is_transparent = void;

    std::size_t operator()(const SearchMatchCacheKey& key) const noexcept {
      return (*this)(static_cast<SearchMatchCacheKeyView>(key));
    }

    std::size_t operator()(const SearchMatchCacheKeyView& key) const noexcept {
      std::size_t h = std::hash<const TextViewport*>{}(key.viewport);
      h ^= static_cast<std::size_t>(key.content_revision) * 2654435761ULL + 0x9e3779b9ULL +
           (h << 6) + (h >> 2);
      h ^= key.line_index * 2654435761ULL + 0x9e3779b9ULL + (h << 6) + (h >> 2);
      h ^= std::hash<std::string_view>{}(key.query) + 0x9e3779b9ULL + (h << 6) + (h >> 2);
      return h;
    }
  };

  struct SearchMatchCacheKeyEqual {
    using is_transparent = void;

    static bool Eq(const SearchMatchCacheKeyView& lhs,
                   const SearchMatchCacheKeyView& rhs) noexcept {
      return lhs.viewport == rhs.viewport && lhs.content_revision == rhs.content_revision &&
             lhs.line_index == rhs.line_index && lhs.query == rhs.query;
    }
    bool operator()(const SearchMatchCacheKeyView& lhs,
                    const SearchMatchCacheKeyView& rhs) const noexcept {
      return Eq(lhs, rhs);
    }
    bool operator()(const SearchMatchCacheKey& lhs, const SearchMatchCacheKey& rhs) const noexcept {
      return Eq(lhs, rhs);
    }
    bool operator()(const SearchMatchCacheKey& lhs,
                    const SearchMatchCacheKeyView& rhs) const noexcept {
      return Eq(lhs, rhs);
    }
    bool operator()(const SearchMatchCacheKeyView& lhs,
                    const SearchMatchCacheKey& rhs) const noexcept {
      return Eq(lhs, rhs);
    }
  };

  static constexpr std::size_t kSearchMatchCacheLimit = 512;
  mutable std::unordered_map<SearchMatchCacheKey,
                             std::vector<std::pair<std::size_t, std::size_t>>,
                             SearchMatchCacheKeyHash, SearchMatchCacheKeyEqual>
      search_match_cache_;
  mutable std::deque<SearchMatchCacheKey> search_match_cache_order_;

  struct BracketMatchCacheEntry {
    const TextViewport* viewport = nullptr;
    // Bracket positions depend on bytes only, not on theme, layout shape, or
    // overlay state — keyed on content_revision exclusively.
    std::uint64_t content_revision = 0;
    std::size_t caret_line = 0;
    std::size_t caret_column = 0;
    std::optional<BracketMatchPair> pair;
  };
  static constexpr std::size_t kBracketMatchCacheLimit = 12;
  mutable std::vector<BracketMatchCacheEntry> bracket_match_cache_entries_;
  mutable std::optional<BracketMatchPair> last_bracket_match_pair_;
  mutable std::size_t bracket_match_cache_hits_ = 0;
  mutable std::size_t bracket_match_cache_misses_ = 0;

  struct IndentGuidesCacheEntry {
    const TextViewport* viewport = nullptr;
    // Indent guides depend on the bytes of each line (content) and on
    // indent-width / tab-size geometry (layout shape).
    std::uint64_t content_revision = 0;
    std::uint64_t layout_shape_revision = 0;
    std::size_t fold_revision = 0;
    std::size_t fold_emphasis_revision = 0;
    std::size_t scroll_line = 0;
    std::size_t visible_rows_count = 0;
    std::size_t indent_width = 0;
    std::size_t caret_line = 0;
    std::vector<IndentGuideRun> runs;
  };
  static constexpr std::size_t kIndentGuidesCacheLimit = 12;
  mutable std::vector<IndentGuidesCacheEntry> indent_guides_cache_entries_;
  mutable const std::vector<IndentGuideRun>* indent_guides_cache_last_runs_ = nullptr;
  mutable std::vector<IndentGuideRun> indent_guides_cache_empty_runs_;
  mutable std::size_t indent_guides_cache_hits_ = 0;
  mutable std::size_t indent_guides_cache_misses_ = 0;

  mutable std::vector<FoldGutterMark> last_fold_gutter_marks_;

  // Scratch buffers reused across rows and frames so the editor render hot
  // path does not allocate (and free) three std::vector instances per visible
  // row per frame. Cleared between rows; capacity persists across frames.
  mutable DecoratedTextRow scratch_row_;
  mutable DecoratedTextRow sticky_scratch_row_;
  // Source-column fills (search / occurrence / selection / bracket) and
  // pre-positioned fills (indent guides / whitespace) marshalled per row and
  // fed to BuildDecoratedRow. Reused across rows so the hot path stays
  // allocation-free.
  mutable std::vector<RowFillSpan> column_fill_scratch_;
  mutable std::vector<DecoratedTextFill> prepositioned_fill_scratch_;
  // End-of-line inline-text / code-lens segments laid out per logical line and
  // drawn after the decorated row. Reused across rows so the hot path stays
  // allocation-free; the geometry matches WorkspaceShell's click hit-test, which
  // calls the same BuildEolDecorationSegments helper.
  mutable std::vector<EolDecorationSegment> eol_decoration_scratch_;
  mutable std::vector<InlayCellSpan> inlay_span_scratch_;
  mutable std::vector<std::size_t> visible_rows_for_guides_scratch_;
  mutable std::string lowered_search_query_scratch_;
  mutable std::string lowered_line_scratch_;

  // Deferred gutter line numbers: collected during the row loop and flushed once
  // through TextRenderer::DrawRuns after it, so on the GPU atlas backend the whole
  // gutter is one batched submit instead of one composite (build+upload) per
  // scrolled line number. `text` is a short digit string (SSO -> no heap). Drawn
  // last in the gutter, preserving the prior numbers-over-markers z-order.
  struct GutterNumber {
    float x = 0.0f;
    float y = 0.0f;
    SDL_Color color{};
    std::string text;
  };
  mutable std::vector<GutterNumber> gutter_number_scratch_;
  mutable std::vector<render::TextRun> gutter_number_run_scratch_;
};

}  // namespace microide::editor
