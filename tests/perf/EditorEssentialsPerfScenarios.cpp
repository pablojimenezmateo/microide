#include "editor/ColumnSelection.h"
#include "perf/PerfHarness.h"

#include "editor/IndentDetect.h"
#include "editor/TextViewport.h"
#include "WorkspaceShellEventHelpers.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <cstdio>
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
  if (!PathExistsNoThrow(cpp_50k)) {
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
  if (!PathExistsNoThrow(cpp_50k)) {
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

// Keyboard column selection materializes one caret per spanned line on every
// keystroke, so a long gesture rebuilds a growing caret set repeatedly -- the one
// shape where a single held-down chord can go quadratic. The 10,000-caret cap in
// SetBoxSelection bounds the size; nothing bounded the rebuild rate, and no
// scenario covered box selection at all (keyboard or mouse).
void RunEditorColumnSelectionBurst(ScenarioContext& context) {
  const std::filesystem::path cpp_50k =
      "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";
  if (!PathExistsNoThrow(cpp_50k)) {
    std::cerr << "editor_column_selection_burst: missing fixture " << cpp_50k << "\n";
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.OpenTab(cpp_50k);
  auto& vp = context.ActiveViewport();
  vp.ClearSecondaryCarets();
  vp.ClearColumnSelection();
  vp.MoveCursorTo(5000, 8, false);

  // 400 Down steps: the caret set grows by one line per step and is rebuilt whole
  // each time, so allocations here scale with the SUM of the span, not its final
  // size. That is the number worth watching.
  context.Measure("column_selection.extend_down", [&] {
    editor::ColumnSelectionState state = vp.column_selection();
    for (int i = 0; i < 400; ++i) {
      const std::size_t lo = state.active ? std::min(state.anchor.line, state.cursor.line) : 5000;
      const std::size_t hi = state.active ? std::max(state.anchor.line, state.cursor.line) : 5000;
      state = editor::StepColumnSelection(state, editor::ColumnSelectDirection::Down,
                                          editor::TextPosition{vp.cursor_line(), vp.cursor_column()},
                                          vp.line_count(), vp.MaxLineLengthInSpan(lo, hi));
      vp.SetColumnSelection(state);
      vp.SetBoxSelection(state.anchor, state.cursor);
    }
  });

  // Horizontal extension re-scans the span for the longest line on every step,
  // which is the cost the keyboard gesture adds over the mouse one.
  context.Measure("column_selection.extend_right", [&] {
    editor::ColumnSelectionState state = vp.column_selection();
    for (int i = 0; i < 64; ++i) {
      const std::size_t lo = std::min(state.anchor.line, state.cursor.line);
      const std::size_t hi = std::max(state.anchor.line, state.cursor.line);
      state = editor::StepColumnSelection(state, editor::ColumnSelectDirection::Right,
                                          editor::TextPosition{vp.cursor_line(), vp.cursor_column()},
                                          vp.line_count(), vp.MaxLineLengthInSpan(lo, hi));
      vp.SetColumnSelection(state);
      vp.SetBoxSelection(state.anchor, state.cursor);
    }
  });
}

void RunEditorShapingMultiCaret(ScenarioContext& context) {
  const std::filesystem::path cpp_50k =
      "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";
  if (!PathExistsNoThrow(cpp_50k)) {
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
  if (!PathExistsNoThrow(cpp_50k)) {
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
  // Repeated so the median is measurable: one toggle resolves in ~1.3 ms, and
  // this runner's CPU modes are tens of percent apart, so the gate flaked at any
  // envelope narrow enough to be worth having. The selection has to be
  // re-established each time -- a toggle collapses it, and without this the
  // repetitions are no-ops that inflate the loop count without doing the work
  // (the allocation count is what says which of the two you have). Toggling an
  // even number of times also leaves the buffer as it started.
  context.Measure("toggle_line_comment.1000_lines", [&] {
    for (int i = 0; i < 16; ++i) {
      vp.MoveCursorTo(first, 0, false);
      vp.MoveCursorTo(last, vp.lines()[last].size(), true);
      if (!context.ExecuteCommand("toggle-line-comment")) {
        throw std::runtime_error("toggle-line-comment failed");
      }
    }
  });
}

void RunEditorMouseSelectionDrag(ScenarioContext& context) {
  const std::filesystem::path cpp_50k =
      "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";
  if (!PathExistsNoThrow(cpp_50k)) {
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

void RunMenuHoverSwitch(ScenarioContext& context) {
  context.ResizeWindow(1280, 720);
  context.PumpFrames(1);

  const auto file_rect =
      workspace::WorkspaceShell::TestAccess::MenuBarItemRect(context.Shell(), "File");
  const auto edit_rect =
      workspace::WorkspaceShell::TestAccess::MenuBarItemRect(context.Shell(), "Edit");
  if (!file_rect.has_value() || !edit_rect.has_value()) {
    throw std::runtime_error("menu_hover_switch: missing File/Edit menu bar items");
  }
  const float file_x = file_rect->x + file_rect->w * 0.5f;
  const float file_y = file_rect->y + file_rect->h * 0.5f;
  if (!workspace::WorkspaceShell::TestAccess::FileMenuOpen(context.Shell())) {
    if (!SendMouseDown(context.Shell(), file_x, file_y, SDL_BUTTON_LEFT)) {
      throw std::runtime_error("menu_hover_switch: failed to open File menu");
    }
  }
  if (!workspace::WorkspaceShell::TestAccess::FileMenuOpen(context.Shell())) {
    throw std::runtime_error("menu_hover_switch: File menu did not stay open");
  }

  context.Measure("menu_hover_switch.160_moves", [&] {
    for (int i = 0; i < 160; ++i) {
      const SDL_FRect& target = (i & 1) == 0 ? *edit_rect : *file_rect;
      const float x = target.x + target.w * 0.5f;
      const float y = target.y + target.h * 0.5f;
      if (!SendMouseMotion(context.Shell(), x, y, 0)) {
        throw std::runtime_error("menu_hover_switch: hover motion was not handled");
      }
    }
  });
  context.PumpFrames(1);
}

void RunMenuPopupHoverRows(ScenarioContext& context) {
  context.ResizeWindow(1280, 720);
  context.PumpFrames(1);

  const auto file_rect =
      workspace::WorkspaceShell::TestAccess::MenuBarItemRect(context.Shell(), "File");
  if (!file_rect.has_value()) {
    throw std::runtime_error("menu_popup_hover_rows: missing File menu bar item");
  }
  const float file_x = file_rect->x + file_rect->w * 0.5f;
  const float file_y = file_rect->y + file_rect->h * 0.5f;
  if (!workspace::WorkspaceShell::TestAccess::FileMenuOpen(context.Shell())) {
    if (!SendMouseDown(context.Shell(), file_x, file_y, SDL_BUTTON_LEFT)) {
      throw std::runtime_error("menu_popup_hover_rows: failed to open File menu");
    }
  }
  if (!workspace::WorkspaceShell::TestAccess::FileMenuOpen(context.Shell())) {
    throw std::runtime_error("menu_popup_hover_rows: File menu did not stay open");
  }

  const auto labels = workspace::WorkspaceShell::TestAccess::VisiblePopupMenuLabels(
      context.Shell(), workspace::WorkspaceShell::MenuId::File);
  if (labels.size() < 2) {
    throw std::runtime_error("menu_popup_hover_rows: File menu exposed fewer than two rows");
  }
  const auto first_item = workspace::WorkspaceShell::TestAccess::PopupMenuItemRect(
      context.Shell(), workspace::WorkspaceShell::MenuId::File, labels.front());
  const auto second_item = workspace::WorkspaceShell::TestAccess::PopupMenuItemRect(
      context.Shell(), workspace::WorkspaceShell::MenuId::File, labels[1]);
  if (!first_item.has_value() || !second_item.has_value()) {
    throw std::runtime_error("menu_popup_hover_rows: missing popup row rects");
  }

  context.Measure("menu_popup_hover_rows.160_moves", [&] {
    for (int i = 0; i < 160; ++i) {
      const SDL_FRect& target = (i & 1) == 0 ? *second_item : *first_item;
      const float x = target.x + target.w * 0.5f;
      const float y = target.y + target.h * 0.5f;
      if (!SendMouseMotion(context.Shell(), x, y, 0)) {
        throw std::runtime_error("menu_popup_hover_rows: hover motion was not handled");
      }
    }
  });
  context.PumpFrames(1);
}

void RunEditorSortLinesLarge(ScenarioContext& context) {
  const std::filesystem::path cpp_50k =
      "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";
  if (!PathExistsNoThrow(cpp_50k)) {
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
  if (!PathExistsNoThrow(seed)) {
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
  if (!PathExistsNoThrow(seed)) {
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
  if (!PathExistsNoThrow(file)) {
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

// Gates `ApplyEditorPreferencesToAllTabs`, which is O(open tabs) and runs on the
// shell thread on every live setting change, project activation, and session
// restore. TD-2026-07-17A-103 split that work into families precisely so a
// project with many restored tabs would not rebuild every tab's language
// contract for a checkbox — but nothing measured it, so the win had no gate and
// anything added to the per-tab path was invisible. The harness even carried an
// unused `ScenarioContext::ApplyEditorPreferencesToAllTabs()` helper for it.
//
// Two measured phases, because the two setting families cost very differently
// and a regression in either is a distinct bug:
//   - cheap family: a save-normalization toggle touches only the per-viewport
//     setters. This phase must NOT scale with contract-rebuild cost.
//   - contract family: an auto-close toggle forces the filetype detect plus
//     language-contract build for every tab. This is the expensive path TD-103
//     bounded, and the one a careless change re-triggers for the cheap family.
//
// A `.editorconfig` sits in the fixture so per-tab EditorConfig resolution is on
// the measured path too. Its memo is what keeps that resolution off this budget,
// so an allocation regression there (e.g. re-materializing the lookup key on a
// memo hit) shows up here as rising per-iteration allocations.
void RunSettingsChangeManyTabs(ScenarioContext& context) {
  // A dedicated fixture rather than large_project: dropping a `.editorconfig`
  // into a shared fixture would silently move multi_tab_cycle,
  // cold_startup_large_project and multi_project_switch off their baselines.
  const std::filesystem::path project = "tests/perf/fixtures/settings_tabs_project";
  if (!PathExistsNoThrow(project)) {
    std::cerr << "settings_change_many_tabs: missing fixture " << project << "\n";
    return;
  }
  (void)context.Open(project);
  context.PumpFrames(2);

  // 40 tabs: enough that per-tab cost dominates the fixed overhead, small enough
  // that the scenario stays well inside the per-iteration budget. `.cpp` so
  // filetype detection resolves a real language contract -- the expensive half.
  constexpr int kTabCount = 40;
  int opened = 0;
  for (int index = 1; index <= kTabCount; ++index) {
    char name[32];
    std::snprintf(name, sizeof(name), "unit_%02d.cpp", index);
    const std::filesystem::path file = project / "src" / name;
    if (!PathExistsNoThrow(file)) {
      continue;
    }
    context.OpenTab(file);
    ++opened;
  }
  if (opened == 0) {
    std::cerr << "settings_change_many_tabs: no fixture files opened\n";
    return;
  }
  context.PumpFrames(2);

  // Pass count is set by what makes the scenario-level allocation gate able to
  // FAIL, not by realism. Opening the tabs costs ~1.1k allocations each, so at a
  // handful of passes the setup is >80% of the iteration and a per-tab
  // regression drowns in it: the 10% default allocation tolerance would sit at
  // ~5.4k, an order of magnitude above the ~440 (one per resolve) that this
  // scenario was written to catch. At 24 passes the measured work is the
  // majority of the iteration and the same regression is ~2.5% -- catchable with
  // the tightened tolerance below. Allocations here are deterministic (the
  // counter is shell-thread only), so a tight envelope is safe; the observed
  // p50/max spread is ~0.5%.
  constexpr int kPasses = 24;

  // Cheap family: per-viewport setters only. Toggled rather than re-set to the
  // same value so the setting genuinely changes each pass.
  bool trim = false;
  context.Measure("settings.apply_cheap_family_all_tabs", [&] {
    for (int pass = 0; pass < kPasses; ++pass) {
      trim = !trim;
      context.SetSetting("editor.save.trim_trailing_whitespace", trim ? "true" : "false");
    }
  });

  // Contract family: the filetype detect + language-contract application per tab.
  // Measured separately so the two costs can never hide inside one number --
  // TD-2026-07-17A-103's whole win was keeping the cheap family off this path,
  // and one merged metric would let that regress invisibly.
  //
  // As of TD-2026-08-03-110 NEITHER family allocates per tab: both memoize, so
  // running this scenario at kTabCount 10 instead of 40 leaves the cheap family
  // byte-identical (10,564) and the contract family within 2 allocations
  // (10,752 vs 10,754). What each phase measures now is the fixed per-settings-
  // change overhead, so a regression that reintroduces per-tab allocation shows
  // up as a large jump here rather than as a few percent -- and re-running at a
  // different kTabCount is the cheap way to confirm that is what happened.
  bool auto_close = false;
  context.Measure("settings.apply_contract_family_all_tabs", [&] {
    for (int pass = 0; pass < kPasses; ++pass) {
      auto_close = !auto_close;
      context.SetSetting("editor.brackets.auto_close.enabled", auto_close ? "true" : "false");
    }
  });

  context.PumpFrames(1);
}

void RunEditorIndentDetectOpen(ScenarioContext& context) {
  const std::filesystem::path file = "tests/perf/fixtures/editor_essentials_1mb/mixed_content.txt";
  if (!PathExistsNoThrow(file)) {
    std::cerr << "editor_indent_detect_open: missing fixture " << file << "\n";
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.SetSetting("editor.indent.detect_on_open", "true");
  context.Measure("open_tab.with_indent_detect", [&] { context.OpenTab(file); });
  // Zero-copy LineSpan path (TD-2026-07-17A-003): detect over the live buffer without
  // a Snapshot() materialization.
  const editor::IndentDetection det = editor::DetectIndent(context.ActiveViewport().lines());
  if (det.non_blank_lines_inspected > 256) {
    throw std::runtime_error("editor_indent_detect_open: DetectIndent exceeded non-blank budget");
  }
  context.PumpFrames(1);
}

// Reproduces the multi-second first-paint freeze recorded in
// dev-docs/performance/performance-findings.md (§4.8): restoring a session whose
// saved cursor/scroll sat deep in a large syntax-highlighted file forces the
// syntax-checkpoint chain to replay from line 0 synchronously on the main thread
// during the first paint at that position. The existing large-file scenarios open
// at the top (replay distance ~0) and so never exercise this. We jump deep with a
// cold highlight cache *inside* the measured region; the driver persists across
// iterations, so InvalidateSyntaxHighlighting() resets the checkpoint cursors to
// keep every iteration cold and the measurement stable.
void RunLargeFileRestoreDeepScrollFirstPaint(ScenarioContext& context) {
  const std::filesystem::path cpp_50k =
      "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";
  if (!PathExistsNoThrow(cpp_50k)) {
    std::cerr << "large_file_restore_deep_scroll_first_paint: missing fixture " << cpp_50k << "\n";
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.OpenTab(cpp_50k);
  auto& vp = context.ActiveViewport();
  constexpr std::size_t kDeepLine = 45000;
  if (vp.lines().size() <= kDeepLine) {
    throw std::runtime_error("large_file_restore_deep_scroll_first_paint: file too short");
  }
  // Settle at the top so each measured jump starts from a comparable scroll state.
  vp.MoveCursorTo(0, 0, false);
  context.PumpFrames(1);
  context.Measure("deep_restore.jump_and_first_paint", [&] {
    vp.InvalidateSyntaxHighlighting();
    vp.MoveCursorTo(kDeepLine, 0, false);
    context.PumpFrames(2);
  });
}

// Single-character line-count churn (Enter then Backspace) in the *middle* of a
// 50k-line file. With the vector<std::string> document model every newline
// insert/delete shifts ~n/2 line entries (O(n)); this scenario is the oracle for
// the piece-tree migration (Phase 3) which makes mid-file edits O(log n). The
// deep region is warmed before the measured burst so this isolates edit-apply
// cost from the syntax-replay first-paint cost measured above.
void RunMidFileEditLatencyLargeFile(ScenarioContext& context) {
  const std::filesystem::path cpp_50k =
      "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";
  if (!PathExistsNoThrow(cpp_50k)) {
    std::cerr << "mid_file_edit_latency_large_file: missing fixture " << cpp_50k << "\n";
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.OpenTab(cpp_50k);
  auto& vp = context.ActiveViewport();
  constexpr std::size_t kMidLine = 25000;
  if (vp.lines().size() <= kMidLine) {
    throw std::runtime_error("mid_file_edit_latency_large_file: file too short");
  }
  vp.MoveCursorTo(kMidLine, 0, false);
  context.PumpFrames(2);  // warm highlights/layout for the visible window
  context.Measure("mid_file_edit.enter_backspace_burst", [&] {
    for (int i = 0; i < 24; ++i) {
      context.KeyDown(SDLK_RETURN);
      context.KeyDown(SDLK_BACKSPACE);
      context.PumpFrames(1);
    }
  });
}

// Same burst as above, on the FIRST line instead of the middle of the file.
//
// This exists because the two were not the same cost and nothing measured the
// difference: a content edit anchored at line 0 took a wholesale-invalidation
// branch that dropped the per-line visual-column table, so the next frame
// rebuilt the width of every line in the buffer -- an O(document) rebuild per
// keystroke that an edit one line lower did not pay. Typing at the top of a
// large file is an ordinary thing to do, and it was the slowest place to type.
void RunFirstLineEditLatencyLargeFile(ScenarioContext& context) {
  const std::filesystem::path cpp_50k =
      "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";
  if (!PathExistsNoThrow(cpp_50k)) {
    std::cerr << "first_line_edit_latency_large_file: missing fixture " << cpp_50k << "\n";
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.OpenTab(cpp_50k);
  auto& vp = context.ActiveViewport();
  if (vp.lines().size() < 2) {
    throw std::runtime_error("first_line_edit_latency_large_file: file too short");
  }
  vp.MoveCursorTo(0, 0, false);
  context.PumpFrames(2);  // warm highlights/layout for the visible window
  context.Measure("first_line_edit.enter_backspace_burst", [&] {
    for (int i = 0; i < 24; ++i) {
      context.KeyDown(SDLK_RETURN);
      context.KeyDown(SDLK_BACKSPACE);
      context.PumpFrames(1);
    }
  });
}

// The "Moby Dick workout" (hogbaysoftware.com/posts/moby-dick-workout): a
// full-novel responsiveness test. Drives the six canonical steps on the real
// ~1.2 MB / ~22k-line Project Gutenberg Moby-Dick body through the same SDL
// input path the app uses -- select-all / cut / paste / undo / redo run as real
// Ctrl-key chords (the keyboard + control-channel editing verbs), and the
// window is resized at the end and the middle. Deterministic: one fixed
// fixture, software renderer, fixed window; the per-phase Measure() calls yield
// p50/p95/max wall + allocation counts.
//
// The fixture is opt-in (a network fetch; see
// generate_editor_essentials_perf_fixtures.py --fixture moby), so this scenario
// is run_by_default=false -- select it explicitly on the reference runner:
//   microide_perf --scenarios=editor_moby_dick_workout
void RunMobyDickWorkout(ScenarioContext& context) {
  const std::filesystem::path moby =
      "tests/perf/fixtures/editor_essentials_moby_dick/moby-dick.txt";
  if (!PathExistsNoThrow(moby)) {
    std::cerr << "editor_moby_dick_workout: missing fixture " << moby
              << " (run: python3 tests/perf/generate_editor_essentials_perf_fixtures.py"
              << " --fixture moby)\n";
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");

  // Step 1: open the novel and reach first stable paint.
  context.Measure("moby.open_first_paint", [&] {
    context.OpenTab(moby);
    context.PumpFrames(2);
  });
  auto& vp = context.ActiveViewport();
  const std::size_t total_lines = vp.lines().size();
  if (total_lines < 20000) {
    throw std::runtime_error("editor_moby_dick_workout: fixture too short");
  }
  const std::size_t last_line = total_lines - 1;
  const std::size_t mid_line = total_lines / 2;

  // Step 2: navigate to the end, then resize the window (shrink + restore).
  context.Measure("moby.scroll_to_end", [&] {
    vp.MoveCursorTo(last_line, 0, false);
    context.PumpFrames(2);
  });
  context.Measure("moby.resize_at_end", [&] {
    context.ResizeWindow(1280, 720);
    context.PumpFrames(1);
    context.ResizeWindow(1920, 1080);
    context.PumpFrames(1);
  });

  // Step 3: jump to the middle, then resize again.
  context.Measure("moby.jump_to_middle", [&] {
    vp.MoveCursorTo(mid_line, 0, false);
    context.PumpFrames(2);
  });
  context.Measure("moby.resize_at_middle", [&] {
    context.ResizeWindow(1280, 720);
    context.PumpFrames(1);
    context.ResizeWindow(1920, 1080);
    context.PumpFrames(1);
  });

  // Step 4: whole-document edit ops -- select all, cut, paste, undo, redo, each
  // a real Ctrl chord. The net effect is the identity transform, so the line
  // count must return to the original regardless of whether the headless
  // clipboard round-trips (if cut/paste are no-ops the buffer never changed);
  // this doubles as a corruption check.
  vp.MoveCursorTo(0, 0, false);
  context.Measure("moby.select_all", [&] {
    context.KeyDown(SDLK_A, SDL_KMOD_CTRL);
    context.PumpFrames(1);
  });
  context.Measure("moby.cut", [&] {
    context.KeyDown(SDLK_X, SDL_KMOD_CTRL);
    context.PumpFrames(1);
  });
  context.Measure("moby.paste", [&] {
    context.KeyDown(SDLK_V, SDL_KMOD_CTRL);
    context.PumpFrames(1);
  });
  context.Measure("moby.undo", [&] {
    context.KeyDown(SDLK_Z, SDL_KMOD_CTRL);
    context.PumpFrames(1);
  });
  context.Measure("moby.redo", [&] {
    context.KeyDown(SDLK_Y, SDL_KMOD_CTRL);
    context.PumpFrames(1);
  });
  const std::size_t after_edit_ops = vp.lines().size();
  if (after_edit_ops == 0 || after_edit_ops + 2 < total_lines ||
      after_edit_ops > total_lines + 2) {
    throw std::runtime_error(
        "editor_moby_dick_workout: buffer corrupted after select-all/cut/paste/undo/redo");
  }

  // Step 5: mid-document editing. Typing must not lag or scroll-jump to the top,
  // so we assert the scroll stays anchored near the middle after the burst.
  vp.MoveCursorTo(mid_line, 0, false);
  context.PumpFrames(2);  // settle scroll/highlights before the measured burst
  const std::size_t scroll_before = vp.scroll_line();
  context.Measure("moby.mid_edit_burst", [&] {
    context.Type("call me Ishmael ");
    for (int i = 0; i < 12; ++i) {
      context.KeyDown(SDLK_RETURN);
      context.KeyDown(SDLK_BACKSPACE);
    }
    context.PumpFrames(1);
  });
  // A regression that resets the viewport to line 0 on a mid-document edit would
  // drop scroll_line from ~mid to ~0; allow a small legitimate drift.
  if (vp.scroll_line() + 100 < scroll_before) {
    throw std::runtime_error(
        "editor_moby_dick_workout: mid-document edit scroll-jumped toward the top");
  }
}

const ScenarioRegistration g_perf_editor_occurrences_scan({Scenario{
    .name = "editor_occurrences_scan",
    .smoke = false,
    // Iteration 0 does one-time cold work every later iteration reuses (font and
    // glyph-atlas fill, the project's initial file-index build, the first session
    // write), and this scenario is short enough that the cold pass dominates the
    // percentiles: measured at 13907 allocations on iteration 0 against a ~4.6k
    // steady state, with every other iteration at or below baseline. Discarding
    // it makes p95/max describe the measured work instead of which iteration
    // index the cold pass landed on.
    .warmup_iterations = 1,
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
    // Iteration 0 does one-time cold work every later iteration reuses (font and
    // glyph-atlas fill, the project's initial file-index build, the first session
    // write), and this scenario is short enough that the cold pass dominates the
    // percentiles: measured at 13065 allocations on iteration 0 against a ~5k
    // steady state, with every other iteration at or below baseline. Discarding
    // it makes p95/max describe the measured work instead of which iteration
    // index the cold pass landed on.
    .warmup_iterations = 1,
    .run = RunEditorMouseSelectionDrag,
}});
const ScenarioRegistration g_perf_menu_hover_switch({Scenario{
    .name = "menu_hover_switch",
    .smoke = false,
    // Iteration 0 does one-time cold work every later iteration reuses (font and
    // glyph-atlas fill, the project's initial file-index build, the first session
    // write), and this scenario is short enough that the cold pass dominates the
    // percentiles: measured at 16648 allocations on iteration 0 against a ~8.9k
    // steady state, with every other iteration at or below baseline. Discarding
    // it makes p95/max describe the measured work instead of which iteration
    // index the cold pass landed on.
    .warmup_iterations = 1,
    .run = RunMenuHoverSwitch,
}});
const ScenarioRegistration g_perf_menu_popup_hover_rows({Scenario{
    .name = "menu_popup_hover_rows",
    .smoke = false,
    // Iteration 0 does one-time cold work every later iteration reuses (font and
    // glyph-atlas fill, the project's initial file-index build, the first session
    // write), and this scenario is short enough that the cold pass dominates the
    // percentiles: measured at 10694 allocations on iteration 0 against a ~3k
    // steady state, with every other iteration at or below baseline. Discarding
    // it makes p95/max describe the measured work instead of which iteration
    // index the cold pass landed on.
    .warmup_iterations = 1,
    .run = RunMenuPopupHoverRows,
}});
const ScenarioRegistration g_perf_editor_sort_lines_large({Scenario{
    .name = "editor_sort_lines_large",
    .smoke = false,
    .run = RunEditorSortLinesLarge,
}});
const ScenarioRegistration g_perf_editor_snippet_expand({Scenario{
    .name = "editor_snippet_expand",
    .smoke = false,
    // Iteration 0 does one-time cold work every later iteration reuses (font and
    // glyph-atlas fill, the project's initial file-index build, the first session
    // write), and this scenario is short enough that the cold pass dominates the
    // percentiles: measured at 9958 allocations on iteration 0 against a ~850
    // steady state, with every other iteration at or below baseline. Discarding
    // it makes p95/max describe the measured work instead of which iteration
    // index the cold pass landed on.
    .warmup_iterations = 1,
    .run = RunEditorSnippetExpand,
}});
const ScenarioRegistration g_perf_editor_snippet_placeholder_edit({Scenario{
    .name = "editor_snippet_placeholder_edit",
    .smoke = false,
    // Iteration 0 does one-time cold work every later iteration reuses (font and
    // glyph-atlas fill, the project's initial file-index build, the first session
    // write), and this scenario is short enough that the cold pass dominates the
    // percentiles: measured at 11411 allocations on iteration 0 against a ~2.3k
    // steady state, with every other iteration at or below baseline. Discarding
    // it makes p95/max describe the measured work instead of which iteration
    // index the cold pass landed on.
    .warmup_iterations = 1,
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
const ScenarioRegistration g_perf_large_file_restore_deep_scroll_first_paint({Scenario{
    .name = "large_file_restore_deep_scroll_first_paint",
    .smoke = false,
    // warmup: the first pass on a fresh driver also pays the project's cold
    // open (background file-index build, initial watch batch, session write),
    // which no later iteration repeats -- 5.6k allocations against a 1.8k
    // steady state. Discarding it keeps p95/max describing the tail of the
    // measured work instead of which iteration index the cold pass landed on.
    .warmup_iterations = 1,
    .run = RunLargeFileRestoreDeepScrollFirstPaint,
}});
const ScenarioRegistration g_perf_mid_file_edit_latency_large_file({Scenario{
    .name = "mid_file_edit_latency_large_file",
    .smoke = false,
    .run = RunMidFileEditLatencyLargeFile,
}});
const ScenarioRegistration g_perf_first_line_edit_latency_large_file({Scenario{
    .name = "first_line_edit_latency_large_file",
    .smoke = false,
    .run = RunFirstLineEditLatencyLargeFile,
}});
const ScenarioRegistration g_perf_settings_change_many_tabs({Scenario{
    .name = "settings_change_many_tabs",
    .smoke = false,
    .baseline_gated = true,
    // warmup: the first pass pays the project's cold open (background file-index
    // build, initial watch batch) plus every tab's first syntax/layout build,
    // which dwarfs the measured re-apply. Left un-warmed it lands in one measured
    // iteration and governs p95/max, exactly as it did for multi_tab_cycle.
    .warmup_iterations = 1,
    // Allocations on this path are deterministic (shell-thread counter only), so
    // the default 10% envelope is far looser than the signal warrants -- it would
    // pass a regression that adds an allocation per tab per settings change,
    // which is exactly what this scenario exists to catch. Observed spread is
    // 0.24% (p50 104,803 / max 105,052), so 1% is 4x the noise while still
    // failing on the ~2k regression this was written to catch (+1.95%) -- which
    // 2% would have let through by a hair. This follows the policy stated on
    // Scenario itself: allocations are the oracle, keep it tight.
    .tolerance_alloc_p50_percent = 1.0,
    .run = RunSettingsChangeManyTabs,
}});
const ScenarioRegistration g_perf_editor_column_selection_burst({Scenario{
    .name = "editor_column_selection_burst",
    .smoke = false,
    .baseline_gated = true,
    .run = RunEditorColumnSelectionBurst,
}});
const ScenarioRegistration g_perf_editor_moby_dick_workout({Scenario{
    .name = "editor_moby_dick_workout",
    .smoke = false,
    .baseline_gated = true,
    // Opt-in: the fixture is a network fetch (--fixture moby), so this does not
    // run in the default/smoke suites; select it explicitly on the reference runner.
    .run_by_default = false,
    .run = RunMobyDickWorkout,
}});

}  // namespace
}  // namespace microide::tests::perf
