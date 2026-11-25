#include "TestSupport.h"

#include "editor/EditorInsetLayout.h"

#include <vector>

#include "editor/PluginDecorationStore.h"
#include "editor/PluginSurfaceStore.h"
#include "editor/TextViewport.h"

namespace microide::tests {
namespace {

using microide::editor::InsetGapOptions;
using microide::editor::PluginDecorationStore;
using microide::editor::PluginSurfaceStore;
using microide::editor::RasterHandle;
using microide::editor::ResolveInsetClick;
using microide::editor::SurfaceAnchor;
using microide::editor::SurfaceContent;
using microide::editor::TextViewport;

SurfaceContent AnchoredInset(const std::filesystem::path& path, std::uint32_t line, float height) {
  SurfaceContent content;
  content.body = RasterHandle{.content_hash = 7, .width = 32, .height = 16};
  content.intrinsic_width = 32;
  content.intrinsic_height = height;
  content.anchor = SurfaceAnchor{.path = path, .line = line};
  return content;
}

TextViewport MakeViewport(const char* path) {
  TextViewport viewport;
  viewport.LoadContent("l0\nl1\nl2\nl3\nl4\nl5\nl6\nl7\n", path);
  viewport.SetViewportSize(6, 8);
  return viewport;
}

void TestResolveRowIgnoresGapsWhenDisabled() {
  const char* path = "/tmp/inset_disabled.txt";
  TextViewport viewport = MakeViewport(path);
  PluginSurfaceStore surfaces;
  PluginDecorationStore decorations;
  surfaces.ReplaceForOwnerSurface("p", "s", AnchoredInset(path, 2, 20.0f));

  const auto result = ResolveInsetClick(surfaces, decorations, viewport, 0.0f,
                                        10.0f, 6, 52.0f,
                                        InsetGapOptions{.inline_surfaces = false});
  Expect(result.hit.row == 5 && !result.hit.in_gap,
         "with inline surfaces off the click resolves with the legacy y/line_height row");
}

void TestResolveRowAccountsForSurfaceGap() {
  const char* path = "/tmp/inset_enabled.txt";
  TextViewport viewport = MakeViewport(path);
  PluginSurfaceStore surfaces;
  PluginDecorationStore decorations;
  surfaces.ReplaceForOwnerSurface("p", "s", AnchoredInset(path, 2, 20.0f));

  const auto result = ResolveInsetClick(surfaces, decorations, viewport, 0.0f, 10.0f, 6, 52.0f,
                                        InsetGapOptions{.inline_surfaces = true});
  Expect(result.hit.row == 3 && !result.hit.in_gap,
         "with inline surfaces on the click below the gap resolves to the correct row");

  const auto gap = ResolveInsetClick(surfaces, decorations, viewport, 0.0f, 10.0f, 6, 40.0f,
                                     InsetGapOptions{.inline_surfaces = true});
  Expect(gap.hit.row == 2 && gap.hit.in_gap, "a click in the inert inset gap is reported in_gap");
}

void TestAboveLensGapAndClick() {
  const char* path = "/tmp/inset_lens.txt";
  TextViewport viewport = MakeViewport(path);
  PluginSurfaceStore surfaces;
  PluginDecorationStore decorations;
  editor::PluginDecorationData data;
  data.code_lenses.push_back(
      editor::CodeLensDecoration{.line = 2, .text = "2 refs", .command = "refs.show"});
  decorations.ReplaceForOwnerFile("plug", path, std::move(data));

  const InsetGapOptions options{.code_lens_above = true, .code_lens_height = 10.0f};
  const auto in_strip = ResolveInsetClick(surfaces, decorations, viewport, 0.0f, 10.0f, 6, 24.0f,
                                          options);
  Expect(in_strip.hit.row == 2 && in_strip.hit.in_gap,
         "a click in the above-line lens strip is reported in_gap on the lensed row");
  Expect(in_strip.gap_content.code_lens != nullptr &&
             in_strip.gap_content.code_lens->command == "refs.show",
         "the above-lens click resolves the lens command");

  const auto in_text = ResolveInsetClick(surfaces, decorations, viewport, 0.0f, 10.0f, 6, 32.0f,
                                         options);
  Expect(in_text.hit.row == 2 && !in_text.hit.in_gap && in_text.gap_content.code_lens == nullptr,
         "a click on the lensed line's text is not treated as a lens click");
}

}

void RegisterEditorInsetLayoutTests(std::vector<TestCase>& tests) {
  AddTest(tests, "EditorInsetLayout/ResolveRowIgnoresGapsWhenDisabled",
          TestResolveRowIgnoresGapsWhenDisabled);
  AddTest(tests, "EditorInsetLayout/ResolveRowAccountsForSurfaceGap",
          TestResolveRowAccountsForSurfaceGap);
  AddTest(tests, "EditorInsetLayout/AboveLensGapAndClick", TestAboveLensGapAndClick);
}

}
