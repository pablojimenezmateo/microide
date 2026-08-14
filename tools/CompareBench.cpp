#include "compare/CompareModel.h"
#include "editor/DecoratedTextGridRenderer.h"
#include "editor/EditorViewRenderer.h"
#include "editor/SyntaxHighlighter.h"
#include "editor/TextLayout.h"
#include "editor/TextViewport.h"
#include "project/GitCompareService.h"
#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "util/Parse.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct SoftwareCanvas {
  SoftwareCanvas() = default;
  SoftwareCanvas(const SoftwareCanvas&) = delete;
  SoftwareCanvas& operator=(const SoftwareCanvas&) = delete;

  SoftwareCanvas(SoftwareCanvas&& other) noexcept
      : surface(other.surface),
        renderer(other.renderer) {
    other.surface = nullptr;
    other.renderer = nullptr;
  }

  SoftwareCanvas& operator=(SoftwareCanvas&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    Reset();
    surface = other.surface;
    renderer = other.renderer;
    other.surface = nullptr;
    other.renderer = nullptr;
    return *this;
  }

  ~SoftwareCanvas() {
    Reset();
  }

  void Reset() {
    if (renderer != nullptr) {
      SDL_DestroyRenderer(renderer);
      renderer = nullptr;
    }
    if (surface != nullptr) {
      SDL_DestroySurface(surface);
      surface = nullptr;
    }
  }

  SDL_Surface* surface = nullptr;
  SDL_Renderer* renderer = nullptr;
};

struct CompareSyntaxBenchData {
  std::vector<std::vector<microide::editor::SyntaxTokenKind>> left_tokens_by_row;
  std::vector<std::vector<microide::editor::SyntaxTokenKind>> right_tokens_by_row;
  std::size_t highlighted_rows = 0;
  std::size_t reused_rows = 0;
  std::size_t token_count = 0;
};

struct PreparedCompareRows {
  std::vector<microide::editor::DecoratedTextRow> left_rows;
  std::vector<microide::editor::DecoratedTextRow> right_rows;
};

struct RunMetrics {
  double read_ms = 0.0;
  double diff_total_ms = 0.0;
  double split_lines_ms = 0.0;
  double line_alignment_ms = 0.0;
  double hunk_alignment_ms = 0.0;
  double intraline_ms = 0.0;
  double row_assembly_ms = 0.0;
  double syntax_ms = 0.0;
  double row_decoration_build_ms = 0.0;
  double row_paint_ms = 0.0;
  double editor_render_ms = 0.0;
  double compare_width_cache_hit_rate = 0.0;
  double editor_visible_layout_cache_hit_rate = 0.0;
  double editor_highlight_cache_hit_rate = 0.0;
  double editor_width_cache_hit_rate = 0.0;
  std::size_t rows = 0;
  std::size_t hunks = 0;
  std::size_t syntax_highlighted_rows = 0;
  std::size_t syntax_reused_rows = 0;
  std::size_t syntax_token_count = 0;
  std::size_t exact_line_alignment_calls = 0;
  std::size_t anchored_line_alignment_calls = 0;
  std::size_t exact_hunk_alignment_calls = 0;
  std::size_t fallback_hunk_alignment_calls = 0;
  std::size_t token_intraline_calls = 0;
  std::size_t codepoint_intraline_calls = 0;
};

std::optional<std::string> ReadFileText(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

double DurationMs(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

double HitRate(std::size_t hits, std::size_t queries) {
  if (queries == 0) {
    return 0.0;
  }
  return (static_cast<double>(hits) * 100.0) / static_cast<double>(queries);
}

std::string FormatDouble(double value) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(3) << value;
  return stream.str();
}

void PrintMetric(std::string_view label, double value) {
  std::cout << label << ": " << FormatDouble(value) << '\n';
}

void PrintMetricSummary(std::string_view label, const std::vector<double>& values) {
  if (values.empty()) {
    return;
  }
  const auto [min_it, max_it] = std::minmax_element(values.begin(), values.end());
  double total = 0.0;
  for (double value : values) {
    total += value;
  }
  const double average = total / static_cast<double>(values.size());
  std::cout << label << "-min: " << FormatDouble(*min_it) << '\n';
  std::cout << label << "-avg: " << FormatDouble(average) << '\n';
  std::cout << label << "-max: " << FormatDouble(*max_it) << '\n';
}

bool EnsureSdlInitialized() {
  setenv("SDL_VIDEODRIVER", "dummy", 0);
  return SDL_Init(SDL_INIT_VIDEO);
}

std::optional<SoftwareCanvas> CreateSoftwareCanvas(int width, int height) {
  SoftwareCanvas canvas;
  canvas.surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA8888);
  if (canvas.surface == nullptr) {
    return std::nullopt;
  }
  canvas.renderer = SDL_CreateSoftwareRenderer(canvas.surface);
  if (canvas.renderer == nullptr) {
    return std::nullopt;
  }
  return canvas;
}

SDL_Color BlendColor(SDL_Color base, SDL_Color tint, float amount) {
  const float clamped_amount = std::clamp(amount, 0.0f, 1.0f);
  const auto blend = [&](Uint8 base_component, Uint8 tint_component) {
    return static_cast<Uint8>(
        std::lround(static_cast<float>(base_component) * (1.0f - clamped_amount) +
                    static_cast<float>(tint_component) * clamped_amount));
  };
  return SDL_Color{
      blend(base.r, tint.r),
      blend(base.g, tint.g),
      blend(base.b, tint.b),
      0xff,
  };
}

CompareSyntaxBenchData TokenizeCompareRows(const std::filesystem::path& path,
                                          const microide::compare::CompareModel& model) {
  CompareSyntaxBenchData data;
  data.left_tokens_by_row.resize(model.rows.size());
  data.right_tokens_by_row.resize(model.rows.size());

  std::vector<std::string> all_left_lines;
  std::vector<std::string> all_right_lines;
  all_left_lines.reserve(model.rows.size());
  all_right_lines.reserve(model.rows.size());
  for (const auto& row : model.rows) {
    all_left_lines.emplace_back(row.left_text);
    all_right_lines.emplace_back(row.right_text);
  }

  auto left_state = microide::editor::SyntaxHighlighter::InitialState(path, all_left_lines);
  auto right_state = microide::editor::SyntaxHighlighter::InitialState(path, all_right_lines);

  for (std::size_t index = 0; index < model.rows.size(); ++index) {
    const auto& row = model.rows[index];
    const bool reuse_tokens =
        row.kind == microide::compare::CompareRowKind::Unchanged && row.left_line > 0 &&
        row.right_line > 0 && row.left_text == row.right_text && left_state == right_state;
    if (reuse_tokens) {
      auto highlighted =
          microide::editor::SyntaxHighlighter::HighlightLine(row.left_text, path, left_state);
      left_state = highlighted.end_state;
      right_state = highlighted.end_state;
      data.left_tokens_by_row[index] = highlighted.tokens;
      data.right_tokens_by_row[index] = highlighted.tokens;
      data.highlighted_rows += 1;
      data.reused_rows += 1;
      data.token_count += highlighted.tokens.size() * 2;
      continue;
    }

    if (row.left_line > 0) {
      auto highlighted =
          microide::editor::SyntaxHighlighter::HighlightLine(row.left_text, path, left_state);
      left_state = highlighted.end_state;
      data.left_tokens_by_row[index] = std::move(highlighted.tokens);
      data.highlighted_rows += 1;
      data.token_count += data.left_tokens_by_row[index].size();
    }
    if (row.right_line > 0) {
      auto highlighted =
          microide::editor::SyntaxHighlighter::HighlightLine(row.right_text, path, right_state);
      right_state = highlighted.end_state;
      data.right_tokens_by_row[index] = std::move(highlighted.tokens);
      data.highlighted_rows += 1;
      data.token_count += data.right_tokens_by_row[index].size();
    }
  }

  return data;
}

void AppendChangedUnderlines(microide::editor::DecoratedTextRow& row_desc,
                             const microide::render::TextRenderer& text_renderer,
                             float x,
                             float y,
                             float line_height,
                             std::size_t visible_columns,
                             std::string_view text,
                             const std::vector<microide::compare::CompareTextSpan>& changed_spans,
                             SDL_Color underline_color) {
  if (text.empty() || changed_spans.empty()) {
    return;
  }

  const microide::editor::VisibleTextWindow window =
      microide::editor::SliceVisibleColumns(text, 0, visible_columns);
  if (window.text.empty()) {
    return;
  }

  const std::size_t window_end = window.byte_offset + window.text.size();
  for (const auto& span : changed_spans) {
    if (span.end <= window.byte_offset) {
      continue;
    }
    if (span.start >= window_end) {
      break;
    }

    const std::size_t clipped_start = std::max(span.start, window.byte_offset);
    const std::size_t clipped_end = std::min(span.end, window_end);
    if (clipped_end <= clipped_start) {
      continue;
    }

    const std::size_t local_start = clipped_start - window.byte_offset;
    const std::size_t local_end = clipped_end - window.byte_offset;
    const std::string_view prefix_text(window.text.data(), local_start);
    const std::string_view changed_text(window.text.data() + local_start,
                                        local_end - local_start);
    const float start_x = x + text_renderer.MeasureWidth(prefix_text);
    const float span_width = text_renderer.MeasureWidth(changed_text);
    if (span_width <= 0.0f) {
      continue;
    }
    row_desc.underlines.push_back(microide::editor::DecoratedUnderline{
        .rect = SDL_FRect{start_x, y + line_height - 2.0f, span_width, 1.0f},
        .color = underline_color,
    });
  }
}

PreparedCompareRows BuildPreparedCompareRows(
    const microide::compare::CompareModel& model,
    const CompareSyntaxBenchData& syntax,
    const microide::render::TextRenderer& text_renderer,
    const microide::render::Theme& theme) {
  PreparedCompareRows prepared;
  prepared.left_rows.reserve(model.rows.size());
  prepared.right_rows.reserve(model.rows.size());

  constexpr float kLineHeight = 14.0f;
  constexpr float kLeftX = 24.0f;
  constexpr float kRightX = 780.0f;
  constexpr float kGutterWidth = 56.0f;
  constexpr float kPaneWidth = 680.0f;
  constexpr std::size_t kVisibleColumns = 120;
  constexpr int kRowBandCount = 64;

  for (std::size_t index = 0; index < model.rows.size(); ++index) {
    const auto& compare_row = model.rows[index];
    const float y = 8.0f + static_cast<float>(index % kRowBandCount) * kLineHeight;
    const SDL_Color left_background = [&]() {
      switch (compare_row.kind) {
        case microide::compare::CompareRowKind::Deleted:
          return BlendColor(theme.editor_background, theme.diff_deleted, 0.28f);
        case microide::compare::CompareRowKind::Modified:
          return BlendColor(theme.editor_background, theme.diff_modified, 0.28f);
        case microide::compare::CompareRowKind::Added:
        case microide::compare::CompareRowKind::Unchanged:
        default:
          return theme.editor_background;
      }
    }();
    const SDL_Color right_background = [&]() {
      switch (compare_row.kind) {
        case microide::compare::CompareRowKind::Added:
          return BlendColor(theme.editor_background, theme.diff_added, 0.28f);
        case microide::compare::CompareRowKind::Modified:
          return BlendColor(theme.editor_background, theme.diff_modified, 0.28f);
        case microide::compare::CompareRowKind::Deleted:
        case microide::compare::CompareRowKind::Unchanged:
        default:
          return theme.editor_background;
      }
    }();
    const SDL_Color left_color =
        compare_row.kind == microide::compare::CompareRowKind::Deleted
            ? theme.diff_deleted
            : compare_row.kind == microide::compare::CompareRowKind::Modified
                  ? theme.diff_modified
                  : theme.text_secondary;
    const SDL_Color right_color =
        compare_row.kind == microide::compare::CompareRowKind::Added
            ? theme.diff_added
            : compare_row.kind == microide::compare::CompareRowKind::Modified
                  ? theme.diff_modified
                  : theme.text_secondary;

    microide::editor::DecoratedTextRow left_row;
    left_row.fills.push_back(microide::editor::DecoratedTextFill{
        .rect = SDL_FRect{kLeftX, y - 1.0f, kGutterWidth + kPaneWidth, kLineHeight},
        .color = left_background,
    });
    microide::editor::AppendVisibleSyntaxTextRuns(
        left_row, text_renderer, theme, kLeftX + kGutterWidth, y, compare_row.left_text, 0,
        kVisibleColumns, left_color, syntax.left_tokens_by_row[index]);
    AppendChangedUnderlines(
        left_row, text_renderer, kLeftX + kGutterWidth, y, kLineHeight, kVisibleColumns,
        compare_row.left_text, compare_row.left_changed_spans,
        compare_row.kind == microide::compare::CompareRowKind::Deleted ? theme.diff_deleted
                                                                       : theme.diff_modified);
    prepared.left_rows.push_back(std::move(left_row));

    microide::editor::DecoratedTextRow right_row;
    right_row.fills.push_back(microide::editor::DecoratedTextFill{
        .rect = SDL_FRect{kRightX, y - 1.0f, kGutterWidth + kPaneWidth, kLineHeight},
        .color = right_background,
    });
    microide::editor::AppendVisibleSyntaxTextRuns(
        right_row, text_renderer, theme, kRightX + kGutterWidth, y, compare_row.right_text, 0,
        kVisibleColumns, right_color, syntax.right_tokens_by_row[index]);
    AppendChangedUnderlines(
        right_row, text_renderer, kRightX + kGutterWidth, y, kLineHeight, kVisibleColumns,
        compare_row.right_text, compare_row.right_changed_spans,
        compare_row.kind == microide::compare::CompareRowKind::Added ? theme.diff_added
                                                                     : theme.diff_modified);
    prepared.right_rows.push_back(std::move(right_row));
  }

  return prepared;
}

double MeasurePreparedRowPaint(const PreparedCompareRows& prepared,
                               const microide::render::TextRenderer& text_renderer,
                               SDL_Renderer* renderer) {
  const microide::editor::DecoratedTextGridRenderer row_renderer;
  const Clock::time_point start = Clock::now();
  for (std::size_t index = 0; index < prepared.left_rows.size(); ++index) {
    row_renderer.RenderRow(renderer, text_renderer, prepared.left_rows[index]);
    row_renderer.RenderRow(renderer, text_renderer, prepared.right_rows[index]);
  }
  return DurationMs(start, Clock::now());
}

double MeasureWarmEditorRender(std::string_view content,
                               const std::filesystem::path& path,
                               SDL_Renderer* renderer,
                               double* visible_layout_hit_rate,
                               double* highlight_hit_rate,
                               double* width_cache_hit_rate) {
  microide::editor::TextViewport viewport;
  viewport.LoadContent(content, path);
  microide::render::TextRenderer text_renderer;
  text_renderer.EnsureInitialized(renderer);
  const microide::render::Theme theme = microide::render::MakeDefaultTheme();
  microide::editor::EditorViewRenderer editor_renderer;
  const SDL_FRect rect = SDL_FRect{0.0f, 0.0f, 1280.0f, 720.0f};

  editor_renderer.Render(renderer, text_renderer, theme, viewport, rect, false);
  viewport.ResetCacheStats();
  text_renderer.ResetCacheStats();

  const Clock::time_point start = Clock::now();
  editor_renderer.Render(renderer, text_renderer, theme, viewport, rect, false);
  const double render_ms = DurationMs(start, Clock::now());

  const microide::editor::TextViewportCacheStats viewport_stats = viewport.CacheStats();
  const microide::render::TextRendererCacheStats text_stats = text_renderer.CacheStats();
  *visible_layout_hit_rate =
      HitRate(viewport_stats.visible_line_hits, viewport_stats.visible_line_queries);
  *highlight_hit_rate = HitRate(viewport_stats.highlight_hits, viewport_stats.highlight_queries);
  *width_cache_hit_rate =
      HitRate(text_stats.width_cache_hits, text_stats.width_cache_queries);
  return render_ms;
}

std::optional<int> ParseRunsArg(std::string_view text) {
  static constexpr std::string_view kPrefix = "--runs=";
  if (!text.starts_with(kPrefix)) {
    return std::nullopt;
  }
  const std::optional<int> parsed = microide::util::ParseInt(text.substr(kPrefix.size()));
  if (!parsed.has_value()) {
    return std::nullopt;
  }
  return std::max(1, *parsed);
}

RunMetrics RunBenchmark(const std::filesystem::path& repo_root,
                        const std::filesystem::path& file_path,
                        std::string_view left_ref,
                        std::string_view right_ref,
                        SDL_Renderer* renderer) {
  RunMetrics metrics;

  const Clock::time_point read_start = Clock::now();
  const auto left = microide::project::ReadGitFileAtCommit(repo_root, file_path, std::string(left_ref));
  if (!left.has_value()) {
    throw std::runtime_error("failed to read left side from git");
  }

  std::optional<std::string> right_content;
  if (right_ref == "WORKTREE") {
    right_content = ReadFileText(file_path).value_or("");
  } else {
    const auto right =
        microide::project::ReadGitFileAtCommit(repo_root, file_path, std::string(right_ref));
    if (!right.has_value()) {
      throw std::runtime_error("failed to read right side from git");
    }
    right_content = right->exists ? right->content : "";
  }
  const std::string left_content = left->exists ? left->content : "";
  metrics.read_ms = DurationMs(read_start, Clock::now());

  const auto compare_build =
      microide::compare::BuildCompareModelProfiled(left_content, *right_content);
  metrics.rows = compare_build.model.rows.size();
  metrics.hunks = compare_build.model.hunks.size();
  metrics.diff_total_ms = static_cast<double>(compare_build.profile.total_ns) / 1'000'000.0;
  metrics.split_lines_ms = static_cast<double>(compare_build.profile.split_lines_ns) / 1'000'000.0;
  metrics.line_alignment_ms =
      static_cast<double>(compare_build.profile.line_alignment_ns) / 1'000'000.0;
  metrics.hunk_alignment_ms =
      static_cast<double>(compare_build.profile.hunk_alignment_ns) / 1'000'000.0;
  metrics.intraline_ms = static_cast<double>(compare_build.profile.intraline_ns) / 1'000'000.0;
  metrics.row_assembly_ms =
      static_cast<double>(compare_build.profile.row_assembly_ns) / 1'000'000.0;
  metrics.exact_line_alignment_calls = compare_build.profile.exact_line_alignment_calls;
  metrics.anchored_line_alignment_calls = compare_build.profile.anchored_line_alignment_calls;
  metrics.exact_hunk_alignment_calls = compare_build.profile.exact_hunk_alignment_calls;
  metrics.fallback_hunk_alignment_calls = compare_build.profile.fallback_hunk_alignment_calls;
  metrics.token_intraline_calls = compare_build.profile.token_intraline_calls;
  metrics.codepoint_intraline_calls = compare_build.profile.codepoint_intraline_calls;

  const Clock::time_point syntax_start = Clock::now();
  const CompareSyntaxBenchData syntax =
      TokenizeCompareRows(file_path, compare_build.model);
  metrics.syntax_ms = DurationMs(syntax_start, Clock::now());
  metrics.syntax_highlighted_rows = syntax.highlighted_rows;
  metrics.syntax_reused_rows = syntax.reused_rows;
  metrics.syntax_token_count = syntax.token_count;

  microide::render::TextRenderer compare_text_renderer;
  compare_text_renderer.EnsureInitialized(renderer);
  const microide::render::Theme theme = microide::render::MakeDefaultTheme();
  BuildPreparedCompareRows(compare_build.model, syntax, compare_text_renderer, theme);
  compare_text_renderer.ResetCacheStats();
  const Clock::time_point row_build_start = Clock::now();
  const PreparedCompareRows prepared =
      BuildPreparedCompareRows(compare_build.model, syntax, compare_text_renderer, theme);
  metrics.row_decoration_build_ms = DurationMs(row_build_start, Clock::now());
  const microide::render::TextRendererCacheStats compare_text_stats =
      compare_text_renderer.CacheStats();
  metrics.compare_width_cache_hit_rate =
      HitRate(compare_text_stats.width_cache_hits, compare_text_stats.width_cache_queries);

  metrics.row_paint_ms = MeasurePreparedRowPaint(prepared, compare_text_renderer, renderer);
  metrics.editor_render_ms = MeasureWarmEditorRender(*right_content, file_path, renderer,
                                                     &metrics.editor_visible_layout_cache_hit_rate,
                                                     &metrics.editor_highlight_cache_hit_rate,
                                                     &metrics.editor_width_cache_hit_rate);
  return metrics;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr
        << "usage: microide_diff_bench <repo-root> <file> [left-ref] [right-ref|WORKTREE] [--runs=N]\n";
    return 1;
  }

  std::filesystem::path repo_root = std::filesystem::path(argv[1]).lexically_normal();
  std::filesystem::path file_path = std::filesystem::path(argv[2]);
  std::string left_ref = "HEAD";
  std::string right_ref = "WORKTREE";
  int runs = 1;

  int positional_index = 0;
  for (int index = 3; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (const auto parsed_runs = ParseRunsArg(argument); parsed_runs.has_value()) {
      runs = *parsed_runs;
      continue;
    }
    if (positional_index == 0) {
      left_ref = std::string(argument);
    } else if (positional_index == 1) {
      right_ref = std::string(argument);
    } else {
      std::cerr
          << "usage: microide_diff_bench <repo-root> <file> [left-ref] [right-ref|WORKTREE] [--runs=N]\n";
      return 1;
    }
    ++positional_index;
  }

  if (file_path.is_relative()) {
    file_path = (repo_root / file_path).lexically_normal();
  } else {
    file_path = file_path.lexically_normal();
  }

  if (!EnsureSdlInitialized()) {
    std::cerr << "failed to initialize SDL for software rendering\n";
    return 1;
  }
  const std::optional<SoftwareCanvas> canvas = CreateSoftwareCanvas(1536, 920);
  if (!canvas.has_value()) {
    std::cerr << "failed to create software renderer for benchmark\n";
    SDL_Quit();
    return 1;
  }

  std::vector<RunMetrics> runs_data;
  runs_data.reserve(static_cast<std::size_t>(runs));
  try {
    for (int run = 0; run < runs; ++run) {
      runs_data.push_back(
          RunBenchmark(repo_root, file_path, left_ref, right_ref, canvas->renderer));
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    SDL_Quit();
    return 1;
  }

  const RunMetrics& sample = runs_data.front();
  std::cout << "file: " << file_path << '\n';
  std::cout << "left-ref: " << left_ref << '\n';
  std::cout << "right-ref: " << right_ref << '\n';
  std::cout << "runs: " << runs << '\n';
  std::cout << "rows: " << sample.rows << '\n';
  std::cout << "hunks: " << sample.hunks << '\n';
  std::cout << "syntax-highlighted-rows: " << sample.syntax_highlighted_rows << '\n';
  std::cout << "syntax-reused-rows: " << sample.syntax_reused_rows << '\n';
  std::cout << "syntax-token-count: " << sample.syntax_token_count << '\n';
  std::cout << "exact-line-alignment-calls: " << sample.exact_line_alignment_calls << '\n';
  std::cout << "anchored-line-alignment-calls: " << sample.anchored_line_alignment_calls << '\n';
  std::cout << "exact-hunk-alignment-calls: " << sample.exact_hunk_alignment_calls << '\n';
  std::cout << "fallback-hunk-alignment-calls: " << sample.fallback_hunk_alignment_calls << '\n';
  std::cout << "token-intraline-calls: " << sample.token_intraline_calls << '\n';
  std::cout << "codepoint-intraline-calls: " << sample.codepoint_intraline_calls << '\n';

  const auto collect_metric = [&](auto accessor) {
    std::vector<double> values;
    values.reserve(runs_data.size());
    for (const auto& run : runs_data) {
      values.push_back(accessor(run));
    }
    return values;
  };

  const auto read_values = collect_metric([](const RunMetrics& run) { return run.read_ms; });
  const auto diff_values =
      collect_metric([](const RunMetrics& run) { return run.diff_total_ms; });
  const auto split_values =
      collect_metric([](const RunMetrics& run) { return run.split_lines_ms; });
  const auto line_alignment_values =
      collect_metric([](const RunMetrics& run) { return run.line_alignment_ms; });
  const auto hunk_alignment_values =
      collect_metric([](const RunMetrics& run) { return run.hunk_alignment_ms; });
  const auto intraline_values =
      collect_metric([](const RunMetrics& run) { return run.intraline_ms; });
  const auto row_assembly_values =
      collect_metric([](const RunMetrics& run) { return run.row_assembly_ms; });
  const auto syntax_values =
      collect_metric([](const RunMetrics& run) { return run.syntax_ms; });
  const auto row_build_values =
      collect_metric([](const RunMetrics& run) { return run.row_decoration_build_ms; });
  const auto row_paint_values =
      collect_metric([](const RunMetrics& run) { return run.row_paint_ms; });
  const auto editor_render_values =
      collect_metric([](const RunMetrics& run) { return run.editor_render_ms; });
  const auto compare_width_rate_values = collect_metric([](const RunMetrics& run) {
    return run.compare_width_cache_hit_rate;
  });
  const auto editor_layout_rate_values = collect_metric([](const RunMetrics& run) {
    return run.editor_visible_layout_cache_hit_rate;
  });
  const auto editor_highlight_rate_values = collect_metric([](const RunMetrics& run) {
    return run.editor_highlight_cache_hit_rate;
  });
  const auto editor_width_rate_values = collect_metric([](const RunMetrics& run) {
    return run.editor_width_cache_hit_rate;
  });

  if (runs == 1) {
    PrintMetric("read-ms", read_values.front());
    PrintMetric("diff-total-ms", diff_values.front());
    PrintMetric("split-lines-ms", split_values.front());
    PrintMetric("line-alignment-ms", line_alignment_values.front());
    PrintMetric("hunk-alignment-ms", hunk_alignment_values.front());
    PrintMetric("intraline-ms", intraline_values.front());
    PrintMetric("row-assembly-ms", row_assembly_values.front());
    PrintMetric("syntax-ms", syntax_values.front());
    PrintMetric("row-decoration-build-ms", row_build_values.front());
    PrintMetric("row-paint-ms", row_paint_values.front());
    PrintMetric("editor-render-ms", editor_render_values.front());
    PrintMetric("compare-width-cache-hit-rate", compare_width_rate_values.front());
    PrintMetric("editor-visible-layout-cache-hit-rate", editor_layout_rate_values.front());
    PrintMetric("editor-highlight-cache-hit-rate", editor_highlight_rate_values.front());
    PrintMetric("editor-width-cache-hit-rate", editor_width_rate_values.front());
  } else {
    PrintMetricSummary("read-ms", read_values);
    PrintMetricSummary("diff-total-ms", diff_values);
    PrintMetricSummary("split-lines-ms", split_values);
    PrintMetricSummary("line-alignment-ms", line_alignment_values);
    PrintMetricSummary("hunk-alignment-ms", hunk_alignment_values);
    PrintMetricSummary("intraline-ms", intraline_values);
    PrintMetricSummary("row-assembly-ms", row_assembly_values);
    PrintMetricSummary("syntax-ms", syntax_values);
    PrintMetricSummary("row-decoration-build-ms", row_build_values);
    PrintMetricSummary("row-paint-ms", row_paint_values);
    PrintMetricSummary("editor-render-ms", editor_render_values);
    PrintMetricSummary("compare-width-cache-hit-rate", compare_width_rate_values);
    PrintMetricSummary("editor-visible-layout-cache-hit-rate", editor_layout_rate_values);
    PrintMetricSummary("editor-highlight-cache-hit-rate", editor_highlight_rate_values);
    PrintMetricSummary("editor-width-cache-hit-rate", editor_width_rate_values);
  }

  SDL_Quit();
  return 0;
}
