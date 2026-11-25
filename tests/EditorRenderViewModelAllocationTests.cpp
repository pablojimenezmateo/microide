#include "TestSupport.h"

#include "support/SoftwareCanvas.h"

#include "editor/EditorViewRenderer.h"
#include "editor/FoldingModel.h"
#include "editor/TextViewport.h"
#include "perf/AllocationCounter.h"
#include "render/TextRenderer.h"
#include "render/Theme.h"
#include "workspace/render/RenderViewModelBuilder.h"
#include "workspace/WorkspaceContext.h"

#include <SDL3/SDL.h>

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace microide::tests {

namespace {

std::string ReadFixturePrefix(std::string_view relative_under_tests, std::size_t max_bytes) {
  std::ostringstream path;
  path << MICROIDE_TEST_SOURCE_DIR << "/" << relative_under_tests;
  std::ifstream in(path.str(), std::ios::binary);
  if (!in) {
    return {};
  }
  std::string out;
  out.resize(max_bytes);
  in.read(out.data(), static_cast<std::streamsize>(max_bytes));
  out.resize(static_cast<std::size_t>(in.gcount()));
  return out;
}

microide::editor::FoldingModel::ComputeOptions DefaultFoldOptions() {
  microide::editor::FoldingModel::ComputeOptions options;
  options.bracket_pairs = {{'{', '}'}};
  options.use_indent_source = true;
  options.tab_size = 4;
  return options;
}

void TestEditorViewModelBuilderReadsLinesWithoutMaterializingThem() {
  microide::workspace::WorkspaceContext ctx;
  microide::workspace::RenderViewModelBuilder builder(ctx);
  microide::workspace::RenderViewModelBuilder::ResetOccurrenceCachesForTesting();

  std::string content;
  for (int i = 0; i < 400; ++i) {
    content += "\tint value_" + std::to_string(i) + " = compute( " + std::to_string(i) + " );\n";
  }
  microide::editor::TextViewport viewport;
  viewport.LoadContent(content, "/tmp/view-model-zero-copy.cpp");
  viewport.SetTabSize(4);
  viewport.SetIndentWidth(4);
  viewport.SetViewportSize(20, 80);

  microide::editor::EditorViewModel vm;
  for (std::size_t top = 0; top < 300; top += 20) {
    viewport.SetScrollLine(top);
    builder.BuildEditorViewModelInto(vm, viewport, 20, nullptr,
                                     true,
                                     false,
                                     true,
                                     3,
                                     true);
  }

  Expect(!vm.whitespace_glyph_runs.empty(),
         "the fixture must actually produce whitespace runs, else the count below "
         "proves nothing");
  const std::size_t materialized = viewport.lines().materialized_line_count();
  Expect(materialized == 0,
         "the view-model builder must read lines through LineView, not the copying "
         "operator[] accessor (materialized " +
             std::to_string(materialized) + " lines)");
}

void TestWhitespaceRowOffsetsIndexFlatGlyphRuns() {
  microide::workspace::WorkspaceContext ctx;
  microide::workspace::RenderViewModelBuilder builder(ctx);
  microide::workspace::RenderViewModelBuilder::ResetOccurrenceCachesForTesting();

  microide::editor::TextViewport viewport;
  viewport.LoadContent("    indent\n\tspaced\n  hi\nplain\n", "/tmp/ws-row-offsets.txt");
  viewport.SetTabSize(4);
  viewport.SetIndentWidth(4);
  viewport.SetViewportSize(4, 80);
  viewport.SetScrollLine(0);

  microide::editor::EditorViewModel vm;
  builder.BuildEditorViewModelInto(vm, viewport, 4, nullptr,
                                    false,
                                    false,
                                    false,
                                    3,
                                    true);

  Expect(vm.whitespace_row_offsets.size() == 5,
         "whitespace_row_offsets must have visible_rows+1 entries (CSR layout)");
  Expect(vm.whitespace_row_offsets.front() == 0,
         "whitespace_row_offsets starts at zero");
  Expect(vm.whitespace_row_offsets.back() == vm.whitespace_glyph_runs.size(),
         "whitespace_row_offsets last entry must equal the flat run count");
  for (std::size_t r = 0; r + 1 < vm.whitespace_row_offsets.size(); ++r) {
    Expect(vm.whitespace_row_offsets[r] <= vm.whitespace_row_offsets[r + 1],
           "whitespace_row_offsets must be monotonic non-decreasing");
  }
  Expect(vm.whitespace_row_offsets[3] == vm.whitespace_row_offsets[4],
         "row with no whitespace produces an empty CSR slice");
  Expect(vm.whitespace_row_offsets[1] > vm.whitespace_row_offsets[0],
         "row with leading spaces should produce whitespace glyph runs");
}

void TestEditorViewModelIntoPreservesVectorCapacitiesAcrossStableFrames() {
  microide::workspace::WorkspaceContext ctx;
  microide::workspace::RenderViewModelBuilder builder(ctx);
  microide::workspace::RenderViewModelBuilder::ResetOccurrenceCachesForTesting();
  microide::workspace::RenderViewModelBuilder::ResetStickyScrollCacheForTesting();

  microide::editor::FoldingModel model;
  microide::editor::TextViewport viewport;
  std::string body = ReadFixturePrefix("perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp",
                                         256 * 1024);
  if (body.empty()) {
    std::string fallback;
    fallback.reserve(12000);
    for (int i = 0; i < 200; ++i) {
      fallback += "namespace n" + std::to_string(i) + " {\n";
    }
    for (int i = 199; i >= 0; --i) {
      fallback += "}  // n" + std::to_string(i) + "\n";
    }
    body = std::move(fallback);
  }
  viewport.LoadContent(body, "/tmp/editor-capacity-discipline.cpp");
  viewport.SetTabSize(4);
  viewport.SetIndentWidth(4);
  viewport.SetScrollLine(40);
  viewport.SetViewportSize(24, 160);
  viewport.MoveCursorTo(50, 2, false);
  Expect(model.Compute(viewport.lines().Snapshot(), DefaultFoldOptions()),
         "fold model should compute for capacity-discipline fixture");

  microide::editor::EditorViewModel vm;
  builder.BuildEditorViewModelInto(
      vm, viewport, 24, &model,
      true,
      false,
      true,
      3,
      true);

  const std::size_t cap_fold = vm.fold_gutter_marks.capacity();
  const std::size_t cap_ws = vm.whitespace_glyph_runs.capacity();
  const std::size_t* sticky_data_first = vm.sticky_lines.data();
  const microide::editor::OccurrenceRange* occ_data_first = vm.occurrence_ranges.data();

  Expect(!model.resolved_ranges().empty(), "capacity test needs a non-empty fold model");
  Expect(!vm.sticky_lines.empty(), "warm-up build should emit sticky lines for nested fixture");

  builder.BuildEditorViewModelInto(
      vm, viewport, 24, &model,
      true,
      false,
      true,
      3,
      true);

  Expect(vm.fold_gutter_marks.capacity() == cap_fold,
         "fold_gutter_marks should retain capacity on a stable second frame");
  Expect(vm.sticky_lines.data() == sticky_data_first,
         "sticky_lines span should point at the same cache storage on a stable second frame");
  Expect(vm.occurrence_ranges.data() == occ_data_first,
         "occurrence_ranges span should point at the same cache storage on a stable second frame");
  Expect(vm.whitespace_glyph_runs.capacity() == cap_ws,
         "whitespace_glyph_runs should retain capacity on a stable second frame");

#if MICROIDE_PERF_HARNESS_BUILD
  const microide::tests::perf::AllocationSnapshot before = microide::tests::perf::Allocations::Snapshot();
  builder.BuildEditorViewModelInto(
      vm, viewport, 24, &model,
      true,
      false,
      true,
      3,
      true);
  const microide::tests::perf::AllocationDelta delta =
      microide::tests::perf::Allocations::DeltaSince(before);
  Expect(delta.allocations == 0 && delta.bytes_allocated == 0,
         "stable editor view-model rebuild should not allocate on the heap");
#endif
}

void TestPerFrameCacheInvalidationKeysModelAndBuilder() {
  {
    microide::editor::FoldingModel model;
    microide::editor::FoldingModel::Fingerprint fp_in{};
    fp_in.layout_revision = 1;
    fp_in.tab_size = 4;
    fp_in.language_id = "cpp";
    model.SetFingerprint(fp_in);
    const std::vector<std::string> one_line = {"x;"};
    Expect(model.Compute(one_line, DefaultFoldOptions()), "fold model should accept trivial buffer");
    model.SetFingerprint(fp_in);
    Expect(model.IsFresh(fp_in), "model should match the fingerprint it was refreshed under");
    microide::editor::FoldingModel::Fingerprint fp_layout = fp_in;
    fp_layout.layout_revision = 2;
    Expect(!model.IsFresh(fp_layout),
           "fold model IsFresh should fail when layout_revision fingerprint changes");
    microide::editor::FoldingModel::Fingerprint fp_tab = fp_in;
    fp_tab.tab_size = 2;
    Expect(!model.IsFresh(fp_tab), "fold model IsFresh should fail when tab_size changes");
    microide::editor::FoldingModel::Fingerprint fp_lang = fp_in;
    fp_lang.language_id = "txt";
    Expect(!model.IsFresh(fp_lang), "fold model IsFresh should fail when language_id changes");
  }

  {
    microide::workspace::WorkspaceContext ctx;
    microide::workspace::RenderViewModelBuilder builder(ctx);
    microide::workspace::RenderViewModelBuilder::ResetOccurrenceCachesForTesting();
    microide::editor::TextViewport viewport;
    viewport.LoadContent("name\n", "/tmp/occ-layout-key.cpp");
    viewport.MoveCursorTo(0, 0, false);
    viewport.SetViewportSize(4, 80);
    (void)builder.BuildEditorViewModel(viewport, 4, nullptr, true, false);
    viewport.InvalidateSyntaxHighlighting();
    (void)builder.BuildEditorViewModel(viewport, 4, nullptr, true, false);
    Expect(microide::workspace::RenderViewModelBuilder::OccurrenceSeedCacheMissesForTesting() == 1,
           "SyntaxConfig bump must not invalidate the content-keyed occurrence seed cache");
    viewport.InsertCharacter('x');
    (void)builder.BuildEditorViewModel(viewport, 4, nullptr, true, false);
    Expect(microide::workspace::RenderViewModelBuilder::OccurrenceSeedCacheMissesForTesting() == 1,
           "an active text edit must suppress occurrence seed work");
    viewport.MoveCursorTo(0, 0, false);
    (void)builder.BuildEditorViewModel(viewport, 4, nullptr, true, false);
    Expect(microide::workspace::RenderViewModelBuilder::OccurrenceSeedCacheMissesForTesting() == 2,
           "navigation after an edit must invalidate the content-keyed occurrence seed cache");
  }

  {
    microide::workspace::WorkspaceContext ctx;
    microide::workspace::RenderViewModelBuilder builder(ctx);
    microide::editor::FoldingModel model;
    microide::editor::FoldingModel::ComputeOptions options;
    options.bracket_pairs = {{'{', '}'}};
    options.use_indent_source = false;
    options.tab_size = 4;
    const std::vector<std::string> lines = {
        "void a() {",
        "  void b() {",
        "    body();",
        "  }",
        "}",
    };
    Expect(model.Compute(lines, options), "fold model should compute nested fixture");
    microide::editor::TextViewport viewport;
    viewport.LoadContent(
        "void a() {\n"
        "  void b() {\n"
        "    body();\n"
        "  }\n"
        "}\n",
        "/tmp/sticky-fold-rev.cpp");
    viewport.SetViewportSize(3, 120);
    viewport.SetScrollLine(2);
    microide::workspace::RenderViewModelBuilder::ResetStickyScrollCacheForTesting();
    (void)builder.BuildEditorViewModel(viewport, 8, &model, false, false, true, 3);
    Expect(model.ToggleFold(0), "nested fixture should toggle outer fold");
    (void)builder.BuildEditorViewModel(viewport, 8, &model, false, false, true, 3);
    Expect(microide::workspace::RenderViewModelBuilder::StickyScrollCacheMissesForTesting() == 2,
           "fold revision change should miss sticky-scroll cache (scroll line unchanged)");
  }
}

#if MICROIDE_HAS_SDL3_TTF
void EnsureDummySdlVideo() {
  static ScopedEnvVar video_driver("SDL_VIDEODRIVER", "dummy");
  static const bool initialized = SDL_Init(SDL_INIT_VIDEO);
  Expect(initialized, "SDL should initialize with the dummy video driver");
}


void TestIndentGuideRunsPreserveCapacityAcrossStableRenders() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(240, 140);
  microide::render::TextRenderer text_renderer;
  microide::render::Theme theme = microide::render::MakeDefaultTheme();
  microide::editor::TextViewport viewport;
  viewport.LoadContent("if (x) {\n    a();\n        b();\n    }\n}\n", "/tmp/indent-capacity.cpp");
  viewport.SetTabSize(4);
  viewport.SetIndentWidth(4);
  viewport.MoveCursorTo(2, 8);

  microide::editor::FoldingModel model;
  Expect(model.Compute(viewport.lines().Snapshot(), DefaultFoldOptions()), "fold model should compute");
  viewport.SetFoldingModel(&model);

  const SDL_FRect rect{0.0f, 0.0f, 240.0f, 140.0f};
  microide::editor::EditorViewRenderer renderer;

  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "",
                  std::nullopt, std::nullopt, {}, nullptr, false,
                  true, false, &model);
  const std::size_t cap = renderer.last_indent_guide_runs().capacity();
  Expect(!renderer.last_indent_guide_runs().empty(),
         "first frame should populate indent guide runs");
  renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "",
                  std::nullopt, std::nullopt, {}, nullptr, false,
                  true, false, &model);
  Expect(renderer.indent_guides_cache_hits() == 1,
         "second unchanged frame should hit the indent-guides cache");
  Expect(renderer.last_indent_guide_runs().capacity() == cap,
         "indent guide runs vector should preserve capacity across cache hits");
}

void TestPerFrameCacheInvalidationKeysRenderPaths() {
  {
    EnsureDummySdlVideo();
    SoftwareCanvas canvas(200, 120);
    microide::render::TextRenderer text_renderer;
    microide::render::Theme theme = microide::render::MakeDefaultTheme();
    microide::editor::TextViewport viewport;
    viewport.LoadContent("if (a) {\n  return 1;\n}\n", "/tmp/bracket-key.cpp");
    viewport.MoveCursorTo(0, 7);
    const SDL_FRect rect{0.0f, 0.0f, 200.0f, 120.0f};
    microide::editor::EditorViewRenderer renderer;
    renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "",
                    std::nullopt, std::nullopt, {}, nullptr,
                    true);
    Expect(renderer.bracket_match_cache_misses() == 1,
           "bracket-match cache should miss on first frame");
    viewport.InvalidateSyntaxHighlighting();
    renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "",
                    std::nullopt, std::nullopt, {}, nullptr,
                    true);
    Expect(renderer.bracket_match_cache_misses() == 1,
           "SyntaxConfig bump must not invalidate the content-keyed bracket-match cache");
    viewport.InsertCharacter('z');
    renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "",
                    std::nullopt, std::nullopt, {}, nullptr,
                    true);
    Expect(renderer.bracket_match_cache_misses() == 2,
           "ContentEdit must invalidate the bracket-match cache");
  }

  {
    EnsureDummySdlVideo();
    SoftwareCanvas canvas(220, 140);
    microide::render::TextRenderer text_renderer;
    microide::render::Theme theme = microide::render::MakeDefaultTheme();
    microide::editor::TextViewport viewport;
    viewport.LoadContent("void f() {\n  body();\n}\n", "/tmp/indent-fold-rev.cpp");
    viewport.SetTabSize(4);
    viewport.SetIndentWidth(4);
    viewport.MoveCursorTo(1, 2);
    microide::editor::FoldingModel model;
    Expect(model.Compute(viewport.lines().Snapshot(), DefaultFoldOptions()), "fold model should compute");
    viewport.SetFoldingModel(&model);
    const SDL_FRect rect{0.0f, 0.0f, 220.0f, 140.0f};
    microide::editor::EditorViewRenderer renderer;
    renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "",
                    std::nullopt, std::nullopt, {}, nullptr, false, true, false, &model);
    Expect(renderer.indent_guides_cache_misses() == 1,
           "first indent-guides frame should miss the cache");
    Expect(model.ToggleFold(0), "fixture should allow toggling the outer fold");
    renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "",
                    std::nullopt, std::nullopt, {}, nullptr, false, true, false, &model);
    Expect(renderer.indent_guides_cache_misses() == 2,
           "fold model revision change should invalidate indent-guides cache");
  }
}

void TestRendererCachesReuseWarmEntriesAcrossViewportSwitches() {
  EnsureDummySdlVideo();
  SoftwareCanvas canvas(240, 140);
  microide::render::TextRenderer text_renderer;
  microide::render::Theme theme = microide::render::MakeDefaultTheme();
  const SDL_FRect rect{0.0f, 0.0f, 240.0f, 140.0f};
  microide::editor::EditorViewRenderer renderer;

  auto configure_viewport = [](microide::editor::TextViewport& viewport,
                               const std::filesystem::path& path,
                               const std::string& body,
                               microide::editor::FoldingModel& model) {
    viewport.LoadContent(body, path);
    viewport.SetTabSize(4);
    viewport.SetIndentWidth(4);
    viewport.MoveCursorTo(0, 7);
    Expect(model.Compute(viewport.lines().Snapshot(), DefaultFoldOptions()),
           "alternating-cache fixture should compute a fold model");
    viewport.SetFoldingModel(&model);
  };

  microide::editor::TextViewport viewport_a;
  microide::editor::FoldingModel model_a;
  configure_viewport(viewport_a, "/tmp/cache-a.cpp",
                     "if (a) {\n    one();\n        two();\n    }\n}\n", model_a);

  microide::editor::TextViewport viewport_b;
  microide::editor::FoldingModel model_b;
  configure_viewport(viewport_b, "/tmp/cache-b.cpp",
                     "if (b) {\n    three();\n        four();\n    }\n}\n", model_b);

  const auto render_with_features = [&](microide::editor::TextViewport& viewport,
                                        microide::editor::FoldingModel& model) {
    renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect, false, "",
                    std::nullopt, std::nullopt, {}, nullptr,
                    true,
                    true,
                    false, &model);
  };

  render_with_features(viewport_a, model_a);
  render_with_features(viewport_b, model_b);
  render_with_features(viewport_a, model_a);
  render_with_features(viewport_b, model_b);

  Expect(renderer.bracket_match_cache_hits() >= 2,
         "alternating warmed viewports should hit the bracket-match cache after warm-up");
  Expect(renderer.indent_guides_cache_hits() >= 2,
         "alternating warmed viewports should hit the indent-guides cache after warm-up");
}

void TestRowMatchFillsSurviveHorizontalScrollNarrowing() {
  EnsureDummySdlVideo();
  microide::render::TextRenderer text_renderer;
  microide::render::Theme theme = microide::render::MakeDefaultTheme();

  constexpr std::size_t kRecordCount = 40;
  const std::string record = "MMMM abcdef ";
  std::string line;
  for (std::size_t i = 0; i < kRecordCount; ++i) {
    line += record;
  }
  std::vector<microide::editor::SelectionRange> all_matches;
  for (std::size_t i = 0; i < kRecordCount; ++i) {
    const std::size_t start = i * record.size();
    all_matches.push_back(microide::editor::SelectionRange{
        .start = {0, start},
        .end = {0, start + 4},
    });
  }
  constexpr std::size_t kScroll = 98;
  const SDL_FRect rect{0.0f, 0.0f, 400.0f, 60.0f};

  const auto count_differing_pixels = [&](const SoftwareCanvas& a, const SoftwareCanvas& b,
                                          int x_lo, int x_hi) {
    std::size_t differing = 0;
    for (int y = 0; y < 24; ++y) {
      for (int x = std::max(0, x_lo); x < std::min(400, x_hi); ++x) {
        const SDL_Color pa = a.PixelAt(x, y);
        const SDL_Color pb = b.PixelAt(x, y);
        if (pa.r != pb.r || pa.g != pb.g || pa.b != pb.b || pa.a != pb.a) {
          ++differing;
        }
      }
    }
    return differing;
  };

  const float char_width = text_renderer.CharWidth();

  {
    SoftwareCanvas scrolled_canvas(400, 60);
    SoftwareCanvas control_canvas(400, 60);
    microide::editor::TextViewport scrolled;
    scrolled.LoadContent(line + "\n", "/tmp/row-fill-scrolled.txt");
    scrolled.SetViewportSize(3, 20);
    scrolled.SetHorizontalScroll(kScroll);

    microide::editor::TextViewport control;
    control.LoadContent(line.substr(kScroll) + "\n", "/tmp/row-fill-control.txt");
    control.SetViewportSize(3, 20);
    std::vector<microide::editor::SelectionRange> control_matches;
    for (const auto& match : all_matches) {
      if (match.end.column <= kScroll) {
        continue;
      }
      control_matches.push_back(microide::editor::SelectionRange{
          .start = {0, match.start.column > kScroll ? match.start.column - kScroll : 0},
          .end = {0, match.end.column - kScroll},
      });
    }
    Expect(control_matches.size() >= 2,
           "the window must contain more than one match for this to test anything");

    microide::editor::EditorViewRenderer scrolled_renderer;
    microide::editor::EditorViewRenderer control_renderer;
    scrolled_renderer.Render(scrolled_canvas.renderer(), text_renderer, theme, scrolled, rect,
                             false, "", std::nullopt, std::nullopt, {}, nullptr,
                             false, false, false, nullptr, nullptr, nullptr,
                             false, all_matches);
    control_renderer.Render(control_canvas.renderer(), text_renderer, theme, control, rect,
                            false, "", std::nullopt, std::nullopt, {}, nullptr,
                            false, false, false, nullptr, nullptr, nullptr,
                            false, control_matches);

    SoftwareCanvas bare_canvas(400, 60);
    microide::editor::EditorViewRenderer bare_renderer;
    bare_renderer.Render(bare_canvas.renderer(), text_renderer, theme, scrolled, rect,
                         false, "", std::nullopt, std::nullopt, {}, nullptr, false,
                         false, false, nullptr, nullptr, nullptr, false, {});

    const microide::editor::EditorViewMetrics metrics =
        microide::editor::EditorViewRenderer::ComputeMetrics(text_renderer, scrolled, rect, 0,
                                                             false);
    std::size_t checked = 0;
    for (const auto& match : all_matches) {
      const std::size_t window_end = kScroll + metrics.visible_columns;
      if (match.end.column <= kScroll || match.start.column >= window_end) {
        continue;
      }
      const std::size_t visible_start = std::max(match.start.column, kScroll);
      const int x_lo = static_cast<int>(metrics.text_x +
                                        static_cast<float>(visible_start - kScroll) * char_width);
      const int x_hi = x_lo + static_cast<int>(char_width);
      Expect(count_differing_pixels(scrolled_canvas, bare_canvas, x_lo, x_hi) > 0,
             "the match at source column " + std::to_string(match.start.column) +
                 " is inside the scrolled window and must paint");
      ++checked;
    }
    Expect(checked >= 3,
           "the window must contain several matches for this to test anything (checked " +
               std::to_string(checked) + ")");

    const std::size_t differing = count_differing_pixels(scrolled_canvas, control_canvas, 0, 400);
    Expect(differing == 0,
           "a horizontally scrolled row must paint the same match highlights as the same text "
           "unscrolled (" +
               std::to_string(differing) + " pixels differ)");
  }

  {
    SoftwareCanvas canvas(400, 60);
    SoftwareCanvas bare_canvas(400, 60);
    microide::editor::TextViewport viewport;
    viewport.LoadContent(line + "\n", "/tmp/row-fill-literal.txt");
    viewport.SetViewportSize(3, 20);
    viewport.SetHorizontalScroll(kScroll);

    microide::editor::EditorViewRenderer renderer;
    microide::editor::EditorViewRenderer bare_renderer;
    renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect,
                    false, "mmmm", std::nullopt, std::nullopt, {}, nullptr, false,
                    false, false, nullptr, nullptr, nullptr, false);
    bare_renderer.Render(bare_canvas.renderer(), text_renderer, theme, viewport, rect,
                         false, "", std::nullopt, std::nullopt, {}, nullptr, false,
                         false, false, nullptr, nullptr, nullptr, false);

    const microide::editor::EditorViewMetrics metrics =
        microide::editor::EditorViewRenderer::ComputeMetrics(text_renderer, viewport, rect, 0,
                                                             false);
    std::size_t checked = 0;
    for (const auto& match : all_matches) {
      const std::size_t window_end = kScroll + metrics.visible_columns;
      if (match.end.column <= kScroll || match.start.column >= window_end) {
        continue;
      }
      const std::size_t visible_start = std::max(match.start.column, kScroll);
      const int x_lo = static_cast<int>(metrics.text_x +
                                        static_cast<float>(visible_start - kScroll) * char_width);
      const int x_hi = x_lo + static_cast<int>(char_width);
      Expect(count_differing_pixels(canvas, bare_canvas, x_lo, x_hi) > 0,
             "the literal-query match at source column " + std::to_string(match.start.column) +
                 " is inside the scrolled window and must paint");
      ++checked;
    }
    Expect(checked >= 3,
           "the window must contain several literal matches for this to test anything (checked " +
               std::to_string(checked) + ")");
  }

  {
    microide::workspace::WorkspaceContext ctx;
    microide::workspace::RenderViewModelBuilder builder(ctx);
    microide::workspace::RenderViewModelBuilder::ResetOccurrenceCachesForTesting();

    SoftwareCanvas canvas(400, 60);
    microide::editor::TextViewport viewport;
    viewport.LoadContent(line + "\n", "/tmp/row-fill-occurrences.txt");
    const microide::editor::EditorViewMetrics build_metrics =
        microide::editor::EditorViewRenderer::ComputeMetrics(text_renderer, viewport, rect, 0,
                                                             false);
    viewport.SetViewportSize(build_metrics.visible_rows, build_metrics.visible_columns);
    viewport.MoveCursorTo(0, 9 * record.size() + 1, false);
    viewport.SetHorizontalScroll(kScroll);

    microide::editor::EditorViewModel vm;
    builder.BuildEditorViewModelInto(vm, viewport, build_metrics.visible_rows,
                                     nullptr,
                                     true,
                                     true,
                                     false,
                                     3,
                                     false);
    Expect(vm.occurrence_ranges.size() >= 3,
           "the fixture must produce several in-window occurrences for this to test anything (got " +
               std::to_string(vm.occurrence_ranges.size()) + ")");
    Expect(vm.occurrence_ranges.size() < kRecordCount,
           "the occurrence scan must stop at the visible window, not scan the whole line");

    microide::editor::EditorViewRenderer renderer;
    renderer.Render(canvas.renderer(), text_renderer, theme, viewport, rect,
                    false, "", std::nullopt, std::nullopt, {}, &vm, false, false,
                    false, nullptr, nullptr, nullptr, false);

    SoftwareCanvas bare_canvas(400, 60);
    microide::editor::EditorViewModel bare_vm;
    microide::editor::EditorViewRenderer bare_renderer;
    bare_renderer.Render(bare_canvas.renderer(), text_renderer, theme, viewport, rect,
                         false, "", std::nullopt, std::nullopt, {}, &bare_vm, false,
                         false, false, nullptr, nullptr, nullptr, false);
    const microide::editor::EditorViewMetrics metrics =
        microide::editor::EditorViewRenderer::ComputeMetrics(text_renderer, viewport, rect, 0,
                                                             false);
    std::size_t checked = 0;
    for (const auto& occurrence : vm.occurrence_ranges) {
      const std::size_t window_end = kScroll + metrics.visible_columns;
      if (occurrence.end_column <= kScroll || occurrence.start_column >= window_end) {
        continue;
      }
      const std::size_t visible_start = std::max<std::size_t>(occurrence.start_column, kScroll);
      const int x_lo = static_cast<int>(metrics.text_x +
                                        static_cast<float>(visible_start - kScroll) * char_width);
      const int x_hi = x_lo + static_cast<int>(char_width);
      Expect(count_differing_pixels(canvas, bare_canvas, x_lo, x_hi) > 0,
             "the occurrence at source column " + std::to_string(occurrence.start_column) +
                 " is inside the scrolled window and must paint");
      ++checked;
    }
    Expect(checked >= 3,
           "the window must contain several occurrences for this to test anything (checked " +
               std::to_string(checked) + ")");
  }
}
#endif

}

void RegisterEditorRenderViewModelAllocationTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorRenderViewModel/BuilderReadsLinesWithoutMaterializingThem",
          TestEditorViewModelBuilderReadsLinesWithoutMaterializingThem);
  AddTest(tests, "EditorRenderViewModel/WhitespaceRowOffsetsIndexFlatGlyphRuns",
          TestWhitespaceRowOffsetsIndexFlatGlyphRuns);
  AddTest(tests, "EditorRenderViewModel/IntoPreservesVectorCapacitiesOnStableFrames",
          TestEditorViewModelIntoPreservesVectorCapacitiesAcrossStableFrames);
  AddTest(tests, "EditorRenderViewModel/PerFrameCacheInvalidationKeysModelAndBuilder",
          TestPerFrameCacheInvalidationKeysModelAndBuilder);
#if MICROIDE_HAS_SDL3_TTF
  AddTest(tests, "EditorRenderViewModel/IndentGuideRunsPreserveCapacityAcrossCacheHits",
          TestIndentGuideRunsPreserveCapacityAcrossStableRenders);
  AddTest(tests, "EditorRenderViewModel/PerFrameCacheInvalidationKeysRenderPaths",
          TestPerFrameCacheInvalidationKeysRenderPaths);
  AddTest(tests, "EditorRenderViewModel/RendererCachesReuseWarmEntriesAcrossViewportSwitches",
          TestRendererCachesReuseWarmEntriesAcrossViewportSwitches);
  AddTest(tests, "EditorRenderViewModel/RowMatchFillsSurviveHorizontalScrollNarrowing",
          TestRowMatchFillsSurviveHorizontalScrollNarrowing);
#endif
}

}
