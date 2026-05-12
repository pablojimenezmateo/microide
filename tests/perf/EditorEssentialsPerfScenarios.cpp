#include "perf/PerfHarness.h"

#include "editor/IndentDetect.h"
#include "editor/TextViewport.h"
#include "WorkspaceShellEventHelpers.h"
#include "workspace/WorkspaceShellTestAccess.h"

#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace microide::tests::perf {
namespace {

std::string ReadFileTextOrThrow(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to read file: " + path.string());
  }
  return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

void WriteFileTextOrThrow(const std::filesystem::path& path, std::string_view content) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("failed to write file: " + path.string());
  }
  output << content;
  if (!output.good()) {
    throw std::runtime_error("failed to flush file: " + path.string());
  }
}

class FixtureRestoreGuard {
 public:
  FixtureRestoreGuard(std::filesystem::path path, std::string original)
      : path_(std::move(path)), original_(std::move(original)) {}

  ~FixtureRestoreGuard() {
    try {
      WriteFileTextOrThrow(path_, original_);
    } catch (...) {
    }
  }

 private:
  std::filesystem::path path_;
  std::string original_;
};

std::optional<std::pair<std::size_t, std::size_t>> FindTokenLineColumn(const editor::TextViewport& vp,
                                                                      std::string_view token) {
  for (std::size_t line = 0; line < vp.lines().size(); ++line) {
    const std::string& L = vp.lines()[line];
    const auto pos = L.find(token);
    if (pos != std::string::npos) {
      return {{line, pos}};
    }
  }
  return std::nullopt;
}

std::string MakeSnippet20Placeholders() {
  std::string body;
  body.reserve(320);
  for (int i = 1; i <= 20; ++i) {
    body += "${";
    body += std::to_string(i);
    body += ":p}";
  }
  body += "$0";
  return body;
}

std::string MakeSnippetLinkedTen() {
  std::string body;
  body.reserve(120);
  for (int i = 0; i < 10; ++i) {
    body += "${1:x}";
  }
  body += "$0";
  return body;
}

void RunEditorOccurrencesScan(ScenarioContext& context) {
  const std::filesystem::path cpp_50k =
      "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";
  if (!std::filesystem::exists(cpp_50k)) {
    std::cerr << "editor_occurrences_scan: missing fixture " << cpp_50k << "\n";
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.SetSetting("editor.occurrences.enabled", "true");
  context.OpenTab(cpp_50k);
  const auto pos = FindTokenLineColumn(context.ActiveViewport(), "perfocc");
  if (!pos.has_value()) {
    throw std::runtime_error("editor_occurrences_scan: perfocc token not found (regenerate 50k cpp?)");
  }
  context.ActiveViewport().MoveCursorTo(pos->first, pos->second, false);
  context.Measure("occurrences.pump_frames", [&] {
    for (int i = 0; i < 12; ++i) {
      context.PumpFrames(1);
    }
  });
}

void RunEditorAddCursorNextMatch(ScenarioContext& context) {
  const std::filesystem::path cpp_50k =
      "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";
  if (!std::filesystem::exists(cpp_50k)) {
    std::cerr << "editor_add_cursor_next_match: missing fixture " << cpp_50k << "\n";
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.SetSetting("editor.multicursor.add_at_match.enabled", "true");
  context.OpenTab(cpp_50k);
  const auto pos = FindTokenLineColumn(context.ActiveViewport(), "seedhit");
  if (!pos.has_value()) {
    throw std::runtime_error("editor_add_cursor_next_match: seedhit token not found");
  }
  context.ActiveViewport().MoveCursorTo(pos->first, pos->second, false);
  context.Measure("add_cursor_next_match.repeat", [&] {
    for (int i = 0; i < 96; ++i) {
      if (!context.ExecuteCommand("add-cursor-next-match")) {
        throw std::runtime_error("add-cursor-next-match command failed");
      }
    }
  });
}

void RunEditorShapingMultiCaret(ScenarioContext& context) {
  const std::filesystem::path cpp_50k =
      "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";
  if (!std::filesystem::exists(cpp_50k)) {
    std::cerr << "editor_shaping_multi_caret: missing fixture " << cpp_50k << "\n";
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.SetSetting("editor.shaping.line_ops.enabled", "true");
  context.OpenTab(cpp_50k);
  auto& vp = context.ActiveViewport();
  vp.ClearSecondaryCarets();
  vp.MoveCursorTo(5000, 0, false);
  for (unsigned i = 1; i < 32; ++i) {
    vp.AddSecondaryCaret(5000 + i * 25u, 0);
  }
  context.Measure("move_line_down.multi_caret_burst", [&] {
    for (int k = 0; k < 12; ++k) {
      if (!context.ExecuteCommand("move-line-down")) {
        throw std::runtime_error("move-line-down failed");
      }
    }
  });
}

void RunEditorToggleCommentLargeSelection(ScenarioContext& context) {
  const std::filesystem::path cpp_50k =
      "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";
  if (!std::filesystem::exists(cpp_50k)) {
    std::cerr << "editor_toggle_comment_large_selection: missing fixture " << cpp_50k << "\n";
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.SetSetting("editor.shaping.toggle_comment.enabled", "true");
  context.OpenTab(cpp_50k);
  auto& vp = context.ActiveViewport();
  const std::size_t first = 6000;
  const std::size_t last = 6999;
  if (vp.lines().size() <= last) {
    throw std::runtime_error("editor_toggle_comment_large_selection: file too short");
  }
  vp.MoveCursorTo(first, 0, false);
  vp.MoveCursorTo(last, vp.lines()[last].size(), true);
  context.Measure("toggle_line_comment.1000_lines", [&] {
    if (!context.ExecuteCommand("toggle-line-comment")) {
      throw std::runtime_error("toggle-line-comment failed");
    }
  });
}

void RunEditorMouseSelectionDrag(ScenarioContext& context) {
  const std::filesystem::path cpp_50k =
      "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";
  if (!std::filesystem::exists(cpp_50k)) {
    std::cerr << "editor_mouse_selection_drag: missing fixture " << cpp_50k << "\n";
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.OpenTab(cpp_50k);
  auto& vp = context.ActiveViewport();
  if (vp.lines().size() <= 10020) {
    throw std::runtime_error("editor_mouse_selection_drag: file too short");
  }
  vp.MoveCursorTo(10000, 0, false);
  context.PumpFrames(2);

  const auto metrics = workspace::WorkspaceShell::TestAccess::ActiveEditorRenderMetrics(context.Shell());
  const SDL_FRect pane = workspace::WorkspaceShell::TestAccess::ActiveEditorPaneRect(context.Shell());
  const float char_width = workspace::WorkspaceShell::TestAccess::TextCharWidth(context.Shell());
  const float y = metrics.first_line_y + metrics.line_height * 4.0f + metrics.line_height * 0.5f;
  const float start_x = metrics.text_x + char_width * 4.0f;
  const float end_x = std::min(metrics.text_x + char_width * 84.0f, pane.x + pane.w - 20.0f);
  if (!(end_x > start_x) || !SendMouseDown(context.Shell(), start_x, y, SDL_BUTTON_LEFT)) {
    throw std::runtime_error("editor_mouse_selection_drag: failed to start drag selection");
  }

  context.Measure("mouse_selection_drag.160_moves", [&] {
    for (int i = 0; i < 160; ++i) {
      const float t = static_cast<float>(i + 1) / 160.0f;
      const float x = start_x + (end_x - start_x) * t;
      if (!SendMouseMotion(context.Shell(), x, y, SDL_BUTTON_LMASK)) {
        throw std::runtime_error("editor_mouse_selection_drag: motion was not handled");
      }
    }
  });
  if (!SendMouseUp(context.Shell(), end_x, y, SDL_BUTTON_LEFT)) {
    throw std::runtime_error("editor_mouse_selection_drag: failed to end drag selection");
  }
  context.PumpFrames(1);
}

void RunEditorSortLinesLarge(ScenarioContext& context) {
  const std::filesystem::path cpp_50k =
      "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";
  if (!std::filesystem::exists(cpp_50k)) {
    std::cerr << "editor_sort_lines_large: missing fixture " << cpp_50k << "\n";
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.SetSetting("editor.shaping.sort_lines.enabled", "true");
  context.OpenTab(cpp_50k);
  auto& vp = context.ActiveViewport();
  const std::size_t first = 11000;
  const std::size_t last = 20999;
  if (vp.lines().size() <= last) {
    throw std::runtime_error("editor_sort_lines_large: file too short");
  }
  vp.MoveCursorTo(first, 0, false);
  vp.MoveCursorTo(last, vp.lines()[last].size(), true);
  context.Measure("sort_lines_ascending.10000_lines", [&] {
    if (!context.ExecuteCommand("sort-lines-ascending")) {
      throw std::runtime_error("sort-lines-ascending failed");
    }
  });
}

void RunEditorSnippetExpand(ScenarioContext& context) {
  const std::filesystem::path seed =
      "tests/perf/fixtures/editor_essentials_snippet_seed/snippet_expand.cpp";
  if (!std::filesystem::exists(seed)) {
    std::cerr << "editor_snippet_expand: missing fixture " << seed << "\n";
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.SetSetting("editor.snippets.enabled", "true");
  context.OpenTab(seed);
  auto& vp = context.ActiveViewport();
  vp.MoveCursorTo(0, 1, false);
  const std::string body = MakeSnippet20Placeholders();
  context.Measure("snippet.expand_20_placeholders", [&] {
    if (!context.ExpandSnippetAtCaret(body)) {
      throw std::runtime_error("snippet expansion failed");
    }
  });
  context.PumpFrames(1);
}

void RunEditorSnippetPlaceholderEdit(ScenarioContext& context) {
  const std::filesystem::path seed =
      "tests/perf/fixtures/editor_essentials_snippet_seed/snippet_linked.cpp";
  if (!std::filesystem::exists(seed)) {
    std::cerr << "editor_snippet_placeholder_edit: missing fixture " << seed << "\n";
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.SetSetting("editor.snippets.enabled", "true");
  context.OpenTab(seed);
  auto& vp = context.ActiveViewport();
  vp.MoveCursorTo(0, 0, false);
  vp.MoveCursorTo(0, 2, true);
  const std::string body = MakeSnippetLinkedTen();
  if (!context.ExpandSnippetAtCaret(body)) {
    throw std::runtime_error("linked snippet expansion failed");
  }
  context.Measure("snippet.linked_placeholder_typings", [&] {
    for (int i = 0; i < 6; ++i) {
      context.Type("m");
    }
  });
  context.PumpFrames(1);
}

void RunEditorSaveNormalization(ScenarioContext& context) {
  const std::filesystem::path file = "tests/perf/fixtures/editor_essentials_1mb/mixed_content.txt";
  if (!std::filesystem::exists(file)) {
    std::cerr << "editor_save_normalization: missing fixture " << file << "\n";
    return;
  }
  FixtureRestoreGuard guard(file, ReadFileTextOrThrow(file));
  (void)context.Open("tests/perf/fixtures/small_project");
  context.SetSetting("editor.save.trim_trailing_whitespace", "true");
  context.SetSetting("editor.save.ensure_final_newline", "true");
  context.OpenTab(file);
  context.Measure("save.normalize_1mb_buffer", [&] {
    if (!context.SaveActiveTab()) {
      throw std::runtime_error("save tab failed");
    }
  });
  context.PumpFrames(1);
}

void RunEditorIndentDetectOpen(ScenarioContext& context) {
  const std::filesystem::path file = "tests/perf/fixtures/editor_essentials_1mb/mixed_content.txt";
  if (!std::filesystem::exists(file)) {
    std::cerr << "editor_indent_detect_open: missing fixture " << file << "\n";
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.SetSetting("editor.indent.detect_on_open", "true");
  context.Measure("open_tab.with_indent_detect", [&] { context.OpenTab(file); });
  const editor::IndentDetection det = editor::DetectIndent(context.ActiveViewport().lines());
  if (det.non_blank_lines_inspected > 256) {
    throw std::runtime_error("editor_indent_detect_open: DetectIndent exceeded non-blank budget");
  }
  context.PumpFrames(1);
}

const ScenarioRegistration g_perf_editor_occurrences_scan({Scenario{
    .name = "editor_occurrences_scan",
    .smoke = false,
    .run = RunEditorOccurrencesScan,
}});
const ScenarioRegistration g_perf_editor_add_cursor_next_match({Scenario{
    .name = "editor_add_cursor_next_match",
    .smoke = false,
    .run = RunEditorAddCursorNextMatch,
}});
const ScenarioRegistration g_perf_editor_shaping_multi_caret({Scenario{
    .name = "editor_shaping_multi_caret",
    .smoke = false,
    .run = RunEditorShapingMultiCaret,
}});
const ScenarioRegistration g_perf_editor_toggle_comment_large({Scenario{
    .name = "editor_toggle_comment_large_selection",
    .smoke = false,
    .run = RunEditorToggleCommentLargeSelection,
}});
const ScenarioRegistration g_perf_editor_mouse_selection_drag({Scenario{
    .name = "editor_mouse_selection_drag",
    .smoke = false,
    .run = RunEditorMouseSelectionDrag,
}});
const ScenarioRegistration g_perf_editor_sort_lines_large({Scenario{
    .name = "editor_sort_lines_large",
    .smoke = false,
    .run = RunEditorSortLinesLarge,
}});
const ScenarioRegistration g_perf_editor_snippet_expand({Scenario{
    .name = "editor_snippet_expand",
    .smoke = false,
    .run = RunEditorSnippetExpand,
}});
const ScenarioRegistration g_perf_editor_snippet_placeholder_edit({Scenario{
    .name = "editor_snippet_placeholder_edit",
    .smoke = false,
    .run = RunEditorSnippetPlaceholderEdit,
}});
const ScenarioRegistration g_perf_editor_save_normalization({Scenario{
    .name = "editor_save_normalization",
    .smoke = false,
    .run = RunEditorSaveNormalization,
}});
const ScenarioRegistration g_perf_editor_indent_detect_open({Scenario{
    .name = "editor_indent_detect_open",
    .smoke = false,
    .run = RunEditorIndentDetectOpen,
}});

}  // namespace
}  // namespace microide::tests::perf
