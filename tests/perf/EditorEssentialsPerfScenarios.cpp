#include "editor/ColumnSelection.h"
#include "perf/PerfHarness.h"

#include "editor/IndentDetect.h"
#include "editor/ShapingActions.h"
#include "editor/TextViewport.h"
#include "WorkspaceShellEventHelpers.h"
#include "workspace/shell/WorkspaceShellTestAccess.h"

#include <cstdio>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace microide::tests::perf {
namespace {

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
  if (!RequireFixture(context, cpp_50k, "editor_occurrences_scan")) {
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
  if (!RequireFixture(context, cpp_50k, "editor_add_cursor_next_match")) {
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

void RunEditorColumnSelectionBurst(ScenarioContext& context) {
  const std::filesystem::path cpp_50k =
      "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";
  if (!RequireFixture(context, cpp_50k, "editor_column_selection_burst")) {
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.OpenTab(cpp_50k);
  auto& vp = context.ActiveViewport();
  vp.ClearSecondaryCarets();
  vp.ClearColumnSelection();
  vp.MoveCursorTo(5000, 8, false);
  context.PumpFrames(1);

  context.Measure("column_selection.extend_down", [&] {
    editor::ColumnSelectionState state = vp.column_selection();
    for (int i = 0; i < 400; ++i) {
      const std::size_t lo = state.active ? std::min(state.anchor.line, state.cursor.line) : 5000;
      const std::size_t hi = state.active ? std::max(state.anchor.line, state.cursor.line) : 5000;
      state = editor::StepColumnSelection(state, editor::ColumnSelectDirection::Down,
                                          editor::TextPosition{vp.cursor_line(), vp.cursor_visual_column()},
                                          vp.line_count(), vp.MaxVisualWidthInSpan(lo, hi));
      vp.SetColumnSelection(state);
      vp.SetBoxSelectionVisual(state.anchor.line, state.anchor.column, state.cursor.line,
                               state.cursor.column);
    }
  });

  context.Measure("column_selection.extend_right", [&] {
    editor::ColumnSelectionState state = vp.column_selection();
    for (int i = 0; i < 64; ++i) {
      const std::size_t lo = std::min(state.anchor.line, state.cursor.line);
      const std::size_t hi = std::max(state.anchor.line, state.cursor.line);
      state = editor::StepColumnSelection(state, editor::ColumnSelectDirection::Right,
                                          editor::TextPosition{vp.cursor_line(), vp.cursor_visual_column()},
                                          vp.line_count(), vp.MaxVisualWidthInSpan(lo, hi));
      vp.SetColumnSelection(state);
      vp.SetBoxSelectionVisual(state.anchor.line, state.anchor.column, state.cursor.line,
                               state.cursor.column);
    }
  });
}

void RunEditorMultiCaretMotionBurst(ScenarioContext& context) {
  const std::filesystem::path cpp_50k =
      "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";
  if (!RequireFixture(context, cpp_50k, "editor_multi_caret_motion_burst")) {
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.OpenTab(cpp_50k);
  auto& vp = context.ActiveViewport();
  vp.ClearSecondaryCarets();
  vp.ClearColumnSelection();
  vp.MoveCursorTo(5000, 4, false);
  context.PumpFrames(1);
  const auto place_box = [&] {
    vp.ClearSecondaryCarets();
    vp.SetBoxSelection(editor::TextPosition{4400, 4}, editor::TextPosition{5600, 12});
    const std::size_t carets = vp.secondary_caret_range_view().size() + 1;
    if (carets < 1000) {
      throw std::runtime_error(
          "editor_multi_caret_motion_burst: the box produced only " + std::to_string(carets) +
          " carets, so this measures a small set rather than a large one");
    }
  };
  const auto caret_count = [&] { return vp.secondary_caret_range_view().size() + 1; };
  const auto require_set_survived = [&](const char* phase) {
    if (caret_count() < 1000) {
      throw std::runtime_error(std::string("editor_multi_caret_motion_burst: ") + phase +
                               " collapsed the caret set to " + std::to_string(caret_count()) +
                               ", so its number describes a smaller set than the others");
    }
  };

  place_box();
  context.Measure("multi_caret_motion.horizontal_burst", [&] {
    for (int i = 0; i < 32; ++i) {
      vp.MoveCursorHorizontal(i % 2 == 0 ? 1 : -1);
    }
  });
  require_set_survived("horizontal_burst");

  place_box();
  context.Measure("multi_caret_motion.vertical_burst", [&] {
    for (int i = 0; i < 32; ++i) {
      vp.MoveCursorVertical(i % 2 == 0 ? 1 : -1);
    }
  });
  require_set_survived("vertical_burst");

  place_box();
  context.Measure("multi_caret_motion.line_edge_burst", [&] {
    for (int i = 0; i < 16; ++i) {
      vp.MoveCursorLineEnd();
      vp.MoveCursorLineStart();
    }
  });
  require_set_survived("line_edge_burst");

  place_box();
  context.Measure("multi_caret_motion.word_burst", [&] {
    for (int i = 0; i < 16; ++i) {
      vp.MoveCursorWord(i % 2 == 0 ? 1 : -1);
    }
  });
  require_set_survived("word_burst");

  place_box();
  context.Measure("multi_caret_motion.merge_cascade", [&] {
    vp.MoveCursorVertical(1, true);
  });
  if (caret_count() != 1) {
    throw std::runtime_error(
        "editor_multi_caret_motion_burst: the extending step left " +
        std::to_string(caret_count()) +
        " cursors, so the merge cascade this phase exists to time did not happen");
  }
}

void RunEditorMultiCaretEditBurst(ScenarioContext& context) {
  const std::filesystem::path cpp_50k =
      "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";
  if (!RequireFixture(context, cpp_50k, "editor_multi_caret_edit_burst")) {
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.SetSetting("editor.shaping.toggle_comment.enabled", "true");
  context.SetSetting("editor.shaping.line_ops.enabled", "true");
  context.OpenTab(cpp_50k);
  auto& vp = context.ActiveViewport();
  if (vp.lines().size() <= 6000) {
    throw std::runtime_error("editor_multi_caret_edit_burst: file too short");
  }
  vp.ClearSecondaryCarets();
  vp.ClearColumnSelection();
  vp.MoveCursorTo(4400, 4, false);
  context.PumpFrames(1);

  const auto caret_count = [&] { return vp.secondary_caret_range_view().size() + 1; };
  const auto place_box = [&] {
    vp.ClearSecondaryCarets();
    vp.SetBoxSelection(editor::TextPosition{4400, 4}, editor::TextPosition{5600, 4});
    if (caret_count() < 1000) {
      throw std::runtime_error(
          "editor_multi_caret_edit_burst: the box produced only " +
          std::to_string(caret_count()) + " carets, so this measures a small set");
    }
  };
  const std::string before = vp.SerializeDocumentText(vp.line_ending());
  const auto restore = [&](const char* phase) {
    if (vp.SerializeDocumentText(vp.line_ending()) == before) {
      throw std::runtime_error(std::string("editor_multi_caret_edit_burst: ") + phase +
                               " did not change the buffer, so its number describes a no-op");
    }
    while (vp.SerializeDocumentText(vp.line_ending()) != before) {
      if (!vp.Undo()) {
        throw std::runtime_error(std::string("editor_multi_caret_edit_burst: ") + phase +
                                 " could not be undone, so the next iteration starts elsewhere");
      }
    }
  };

  place_box();
  context.Measure("multi_caret_edit.soft_tab", [&] { vp.InsertTab(); });
  restore("soft_tab");

  place_box();
  context.Measure("multi_caret_edit.toggle_block_comment",
                  [&] { (void)editor::ToggleBlockComment(vp, "/*", "*/"); });
  restore("toggle_block_comment");

  place_box();
  context.Measure("multi_caret_edit.join_lines", [&] { (void)editor::JoinLinesAtCarets(vp); });
  restore("join_lines");

  place_box();
  const std::size_t carets_before_growth = caret_count();
  context.Measure("multi_caret_edit.add_cursor_below", [&] {
    for (int i = 0; i < 32; ++i) {
      (void)vp.AddCaretVertical(1);
    }
  });
  if (caret_count() <= carets_before_growth) {
    throw std::runtime_error(
        "editor_multi_caret_edit_burst: add_cursor_below left " + std::to_string(caret_count()) +
        " cursors against " + std::to_string(carets_before_growth) +
        " before it, so the growth this phase times did not happen");
  }
  vp.ClearSecondaryCarets();
}

void RunEditorShapingMultiCaret(ScenarioContext& context) {
  const std::filesystem::path cpp_50k =
      "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";
  if (!RequireFixture(context, cpp_50k, "editor_shaping_multi_caret")) {
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
  if (!RequireFixture(context, cpp_50k, "editor_toggle_comment_large_selection")) {
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
  if (!RequireFixture(context, cpp_50k, "editor_mouse_selection_drag")) {
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

void RunEditorCjkScrollPaint(ScenarioContext& context) {
  const std::filesystem::path cjk =
      "tests/perf/fixtures/editor_essentials_cjk/synthetic_cjk.py";
  if (!RequireFixture(context, cjk, "editor_cjk_scroll_paint")) {
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.OpenTab(cjk);
  if (context.ActiveViewport().lines().size() <= 4000) {
    throw std::runtime_error("editor_cjk_scroll_paint: fixture too short");
  }
  context.PumpFrames(8);

  editor::TextViewport& viewport = context.ActiveViewport();
  viewport.MoveCursorTo(0, 0, false);
  viewport.SetScrollLine(0);
  context.PumpFrames(1);

  context.Measure("cjk_scroll_paint.page_down_sweep", [&] {
    for (int i = 0; i < 160; ++i) {
      context.KeyDown(SDLK_PAGEDOWN);
      context.PumpFrames(1);
    }
  });
  if (context.ActiveViewport().scroll_line() < 3000) {
    throw std::runtime_error(
        "editor_cjk_scroll_paint: the page-down sweep did not scroll through fresh rows");
  }
  context.PumpFrames(2);
}

void RunEditorTypingPaint(ScenarioContext& context) {
  const std::filesystem::path cpp_50k =
      "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";
  if (!RequireFixture(context, cpp_50k, "editor_typing_paint")) {
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.OpenTab(cpp_50k);
  auto& vp = context.ActiveViewport();
  if (vp.lines().size() <= 10000) {
    throw std::runtime_error("editor_typing_paint: file too short");
  }
  vp.MoveCursorTo(10000, 0, false);
  context.PumpFrames(2);
  context.PumpPartialFrames(1);

  context.Measure("typing_paint.160_keystrokes", [&] {
    for (int i = 0; i < 160; ++i) {
      context.PumpPartialFrame(context.Type("x"));
    }
  });
  context.PumpFrames(1);
}

void RunMenuHoverPaint(ScenarioContext& context) {
  context.ResizeWindow(1280, 720);
  context.PumpFrames(1);

  const auto file_rect =
      workspace::WorkspaceShell::TestAccess::MenuBarItemRect(context.Shell(), "File");
  const auto edit_rect =
      workspace::WorkspaceShell::TestAccess::MenuBarItemRect(context.Shell(), "Edit");
  if (!file_rect.has_value() || !edit_rect.has_value()) {
    throw std::runtime_error("menu_hover_paint: missing File/Edit menu bar items");
  }
  const float file_x = file_rect->x + file_rect->w * 0.5f;
  const float file_y = file_rect->y + file_rect->h * 0.5f;
  if (!workspace::WorkspaceShell::TestAccess::FileMenuOpen(context.Shell())) {
    if (!SendMouseDown(context.Shell(), file_x, file_y, SDL_BUTTON_LEFT)) {
      throw std::runtime_error("menu_hover_paint: failed to open File menu");
    }
  }
  if (!workspace::WorkspaceShell::TestAccess::FileMenuOpen(context.Shell())) {
    throw std::runtime_error("menu_hover_paint: File menu did not stay open");
  }
  context.PumpPartialFrames(1);

  context.Measure("menu_hover_paint.160_hover_frames", [&] {
    for (int i = 0; i < 160; ++i) {
      const SDL_FRect& target = (i & 1) == 0 ? *edit_rect : *file_rect;
      const float x = target.x + target.w * 0.5f;
      const float y = target.y + target.h * 0.5f;
      const auto result = context.MouseMoveHover(x, y);
      if (!result.handled) {
        throw std::runtime_error("menu_hover_paint: hover motion was not handled");
      }
      context.PumpPartialFrame(result.redraw);
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
  if (!RequireFixture(context, cpp_50k, "editor_sort_lines_large")) {
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
  if (!RequireFixture(context, seed, "editor_snippet_expand")) {
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
  if (!RequireFixture(context, seed, "editor_snippet_placeholder_edit")) {
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
  const std::filesystem::path fixture = "tests/perf/fixtures/editor_essentials_1mb/mixed_content.txt";
  if (!RequireFixture(context, fixture, "editor_save_normalization")) {
    return;
  }
  std::error_code ec;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path(ec) / "microide_perf_save_normalization";
  std::filesystem::create_directories(dir, ec);
  const std::filesystem::path file = dir / fixture.filename();
  std::filesystem::copy_file(fixture, file, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    throw std::runtime_error("editor_save_normalization: could not copy the fixture: " + ec.message());
  }
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

void RunSettingsChangeManyTabs(ScenarioContext& context) {
  const std::filesystem::path project = "tests/perf/fixtures/settings_tabs_project";
  if (!RequireFixture(context, project, "settings_change_many_tabs")) {
    return;
  }
  (void)context.Open(project);
  context.PumpFrames(2);

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
    context.SkipScenario("settings_change_many_tabs: no fixture files opened under " +
                         (project / "src").string());
    return;
  }
  context.PumpFrames(2);

  constexpr int kPasses = 24;

  bool trim = false;
  context.Measure("settings.apply_cheap_family_all_tabs", [&] {
    for (int pass = 0; pass < kPasses; ++pass) {
      trim = !trim;
      context.SetSetting("editor.save.trim_trailing_whitespace", trim ? "true" : "false");
    }
  });

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
  if (!RequireFixture(context, file, "editor_indent_detect_open")) {
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.SetSetting("editor.indent.detect_on_open", "true");
  context.Measure("open_tab.with_indent_detect", [&] { context.OpenTab(file); });
  const editor::IndentDetection det = editor::DetectIndent(context.ActiveViewport().lines());
  if (det.non_blank_lines_inspected > 1024) {
    throw std::runtime_error("editor_indent_detect_open: DetectIndent exceeded non-blank budget");
  }
  context.PumpFrames(1);
}

void RunLargeFileRestoreDeepScrollFirstPaint(ScenarioContext& context) {
  const std::filesystem::path cpp_50k =
      "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";
  if (!RequireFixture(context, cpp_50k, "large_file_restore_deep_scroll_first_paint")) {
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.OpenTab(cpp_50k);
  auto& vp = context.ActiveViewport();
  constexpr std::size_t kDeepLine = 45000;
  if (vp.lines().size() <= kDeepLine) {
    throw std::runtime_error("large_file_restore_deep_scroll_first_paint: file too short");
  }
  vp.MoveCursorTo(0, 0, false);
  context.PumpFrames(1);
  context.Measure("deep_restore.jump_and_first_paint", [&] {
    vp.InvalidateSyntaxHighlighting();
    vp.MoveCursorTo(kDeepLine, 0, false);
    context.PumpFrames(2);
  });
}

void RunMidFileEditLatencyLargeFile(ScenarioContext& context) {
  const std::filesystem::path cpp_50k =
      "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";
  if (!RequireFixture(context, cpp_50k, "mid_file_edit_latency_large_file")) {
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
  context.PumpFrames(2);
  context.Measure("mid_file_edit.enter_backspace_burst", [&] {
    for (int i = 0; i < 24; ++i) {
      context.KeyDown(SDLK_RETURN);
      context.KeyDown(SDLK_BACKSPACE);
      context.PumpFrames(1);
    }
  });
}

void RunEditorTypingMinifiedLine(ScenarioContext& context) {
  const std::filesystem::path minified =
      "tests/perf/fixtures/editor_essentials_minified/bundle.min.js";
  if (!RequireFixture(context, minified, "editor_typing_minified_line")) {
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.OpenTab(minified);
  auto& vp = context.ActiveViewport();
  if (vp.lines().size() > 2) {
    throw std::runtime_error(
        "editor_typing_minified_line: fixture is not a single line (regenerate it)");
  }
  vp.MoveCursorTo(0, vp.lines().front().size() / 2, false);
  context.PumpFrames(2);
  context.Measure("minified_line.type_burst", [&] {
    for (int i = 0; i < 16; ++i) {
      context.Type("x");
      context.PumpFrames(1);
    }
  });
  context.Measure("minified_line.backspace_burst", [&] {
    for (int i = 0; i < 16; ++i) {
      context.KeyDown(SDLK_BACKSPACE);
      context.PumpFrames(1);
    }
  });
}

editor::TextViewport* OpenMinifiedFixtureOrSkip(ScenarioContext& context,
                                                const char* scenario_name) {
  const std::filesystem::path minified =
      "tests/perf/fixtures/editor_essentials_minified/bundle.min.js";
  if (!RequireFixture(context, minified, scenario_name)) {
    return nullptr;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.OpenTab(minified);
  editor::TextViewport& viewport = context.ActiveViewport();
  if (viewport.lines().size() > 2) {
    throw std::runtime_error(std::string(scenario_name) +
                             ": fixture is not a single line (regenerate it)");
  }
  return &viewport;
}

void RunEditorLongLineHorizontalScroll(ScenarioContext& context) {
  editor::TextViewport* viewport =
      OpenMinifiedFixtureOrSkip(context, "editor_long_line_horizontal_scroll");
  if (viewport == nullptr) {
    return;
  }
  viewport->MoveCursorTo(0, 0, false);
  context.PumpFrames(2);

  context.Measure("long_line_scroll.end_home_jumps", [&] {
    for (int i = 0; i < 8; ++i) {
      context.KeyDown(SDLK_END);
      context.PumpFrames(1);
      context.KeyDown(SDLK_HOME);
      context.PumpFrames(1);
    }
  });

  context.KeyDown(SDLK_END);
  context.PumpFrames(2);
  context.Measure("long_line_scroll.word_motion_burst", [&] {
    for (int i = 0; i < 24; ++i) {
      context.KeyDown(SDLK_LEFT, SDL_KMOD_CTRL);
      context.PumpFrames(1);
    }
  });
}

void RunEditorLongLineBufferSearch(ScenarioContext& context) {
  editor::TextViewport* viewport =
      OpenMinifiedFixtureOrSkip(context, "editor_long_line_buffer_search");
  if (viewport == nullptr) {
    return;
  }
  viewport->MoveCursorTo(0, 0, false);
  context.PumpFrames(2);

  context.Measure("long_line_search.rare_query", [&] {
    if (!context.ExecuteCommand("search item_12000")) {
      throw std::runtime_error("editor_long_line_buffer_search: search command failed");
    }
    context.PumpFrames(2);
  });

  context.Measure("long_line_search.common_query", [&] {
    if (!context.ExecuteCommand("search \"name\"")) {
      throw std::runtime_error("editor_long_line_buffer_search: search command failed");
    }
    context.PumpFrames(2);
  });

  context.Measure("long_line_search.next_match_burst", [&] {
    for (int i = 0; i < 16; ++i) {
      context.KeyDown(SDLK_RETURN);
      context.PumpFrames(1);
    }
  });
  context.KeyDown(SDLK_ESCAPE);
  context.PumpFrames(1);
}

void RunEditorSoftWrapLongLineScroll(ScenarioContext& context) {
  editor::TextViewport* viewport =
      OpenMinifiedFixtureOrSkip(context, "editor_soft_wrap_long_line_scroll");
  if (viewport == nullptr) {
    return;
  }
  context.SetSetting("editor.wrap", "word");
  viewport->MoveCursorTo(0, 0, false);
  context.PumpFrames(4);
  if (!context.ActiveViewport().soft_wrap()) {
    throw std::runtime_error(
        "editor_soft_wrap_long_line_scroll: editor.wrap=word did not reach the viewport");
  }
  if (context.ActiveViewport().visual_line_count() < 100) {
    throw std::runtime_error(
        "editor_soft_wrap_long_line_scroll: the fixture line did not wrap into rows");
  }

  const std::uint64_t measure_before = microide::util::ReadPerformanceCounter(
      microide::util::PerfCounterId::EditorLineWidthMeasureBytes);
  const std::uint64_t rebuilds_before = microide::util::ReadPerformanceCounter(
      microide::util::PerfCounterId::EditorEnsureWrappedRowLayoutsRebuilds);
  context.Measure("soft_wrap_long_line.scroll_frames", [&] {
    for (int i = 0; i < 64; ++i) {
      context.Scroll(-3);
      context.PumpFrames(1);
    }
  });
  const std::uint64_t measure_after = microide::util::ReadPerformanceCounter(
      microide::util::PerfCounterId::EditorLineWidthMeasureBytes);
  const std::uint64_t rebuilds_after = microide::util::ReadPerformanceCounter(
      microide::util::PerfCounterId::EditorEnsureWrappedRowLayoutsRebuilds);
  if (measure_after != measure_before) {
    throw std::runtime_error(
        "editor_soft_wrap_long_line_scroll: scrolling re-measured whole lines to build "
        "rows (bytes=" +
        std::to_string(measure_after - measure_before) + ")");
  }
  if (rebuilds_after != rebuilds_before) {
    throw std::runtime_error(
        "editor_soft_wrap_long_line_scroll: scrolling rebuilt the wrapped-row table (" +
        std::to_string(rebuilds_after - rebuilds_before) + " times)");
  }

  context.Measure("soft_wrap_long_line.down_arrow_burst", [&] {
    for (int i = 0; i < 64; ++i) {
      context.KeyDown(SDLK_DOWN);
      context.PumpFrames(1);
    }
  });
  context.Measure("soft_wrap_long_line.up_arrow_burst", [&] {
    for (int i = 0; i < 64; ++i) {
      context.KeyDown(SDLK_UP);
      context.PumpFrames(1);
    }
  });
}

void RunEditorSoftWrapLongLineTyping(ScenarioContext& context) {
  editor::TextViewport* viewport =
      OpenMinifiedFixtureOrSkip(context, "editor_soft_wrap_long_line_typing");
  if (viewport == nullptr) {
    return;
  }
  context.SetSetting("editor.wrap", "word");
  viewport->MoveCursorTo(0, viewport->lines().front().size() / 2, false);
  context.PumpFrames(4);
  if (!context.ActiveViewport().soft_wrap()) {
    throw std::runtime_error(
        "editor_soft_wrap_long_line_typing: editor.wrap=word did not reach the viewport");
  }
  context.Measure("soft_wrap_long_line.type_burst", [&] {
    for (int i = 0; i < 16; ++i) {
      context.Type("x");
      context.PumpFrames(1);
    }
  });
  context.Measure("soft_wrap_long_line.backspace_burst", [&] {
    for (int i = 0; i < 16; ++i) {
      context.KeyDown(SDLK_BACKSPACE);
      context.PumpFrames(1);
    }
  });
}

void RunEditorSoftWrapWhitespacePaint(ScenarioContext& context) {
  editor::TextViewport* viewport =
      OpenMinifiedFixtureOrSkip(context, "editor_soft_wrap_whitespace_paint");
  if (viewport == nullptr) {
    return;
  }
  context.SetSetting("editor.wrap", "word");
  context.SetSetting("editor.view.render_whitespace", "true");
  viewport->MoveCursorTo(0, 0, false);
  context.PumpFrames(4);
  if (!context.ActiveViewport().soft_wrap()) {
    throw std::runtime_error(
        "editor_soft_wrap_whitespace_paint: editor.wrap=word did not reach the viewport");
  }
  const std::size_t total_rows = context.ActiveViewport().visual_line_count();
  if (total_rows < 1000) {
    throw std::runtime_error(
        "editor_soft_wrap_whitespace_paint: the fixture line did not wrap into rows");
  }

  for (int i = 0; i < 200; ++i) {
    context.Scroll(-3);
  }
  context.PumpFrames(2);
  const std::size_t deep_scroll_row = context.ActiveViewport().scroll_line();
  if (deep_scroll_row < 400) {
    throw std::runtime_error(
        "editor_soft_wrap_whitespace_paint: the scroll did not reach a deep row (" +
        std::to_string(deep_scroll_row) + ")");
  }

  const std::size_t visible_rows = context.ActiveViewport().visible_lines();
  constexpr int kFrames = 48;
  const std::uint64_t walk_before = microide::util::ReadPerformanceCounter(
      microide::util::PerfCounterId::EditorWhitespaceMarkerWalkBytes);
  const std::uint64_t prefix_before = microide::util::ReadPerformanceCounter(
      microide::util::PerfCounterId::EditorWhitespaceMarkerPrefixBytesScanned);
  context.Measure("soft_wrap_whitespace.deep_scroll_frames", [&] {
    for (int i = 0; i < kFrames; ++i) {
      context.Scroll((i % 2) == 0 ? -1 : 1);
      context.PumpFrames(1);
    }
  });
  const std::uint64_t walked = microide::util::ReadPerformanceCounter(
                                   microide::util::PerfCounterId::EditorWhitespaceMarkerWalkBytes) -
                               walk_before;
  const std::uint64_t probed =
      microide::util::ReadPerformanceCounter(
          microide::util::PerfCounterId::EditorWhitespaceMarkerPrefixBytesScanned) -
      prefix_before;

  const std::uint64_t per_row_budget =
      8ull * 4ull * static_cast<std::uint64_t>(context.ActiveViewport().visible_columns() + 1);
  const std::uint64_t budget =
      per_row_budget * static_cast<std::uint64_t>(visible_rows + 1) * kFrames;
  if (walked > budget) {
    throw std::runtime_error(
        "editor_soft_wrap_whitespace_paint: the whitespace walk read " + std::to_string(walked) +
        " bytes over " + std::to_string(kFrames) + " frames, past the " + std::to_string(budget) +
        " a visible-window-bounded walk can need — it is deriving each row's start from the "
        "line's start again");
  }
  const std::uint64_t deepest_row_start =
      static_cast<std::uint64_t>(deep_scroll_row + visible_rows + kFrames + 8) *
      static_cast<std::uint64_t>(context.ActiveViewport().visible_columns() + 1);
  const std::uint64_t probe_budget = 4ull * kFrames * deepest_row_start;
  if (probed > probe_budget) {
    throw std::runtime_error(
        "editor_soft_wrap_whitespace_paint: the resume probe read " + std::to_string(probed) +
        " bytes over " + std::to_string(kFrames) + " frames, past " +
        std::to_string(probe_budget) + " — it is being paid per row rather than per line");
  }
}

void RunEditorLongLineSelectAllEdit(ScenarioContext& context) {
  editor::TextViewport* viewport =
      OpenMinifiedFixtureOrSkip(context, "editor_long_line_select_all_edit");
  if (viewport == nullptr) {
    return;
  }
  const std::size_t original_bytes = viewport->lines().front().size();
  viewport->MoveCursorTo(0, 0, false);
  context.PumpFrames(2);

  context.Measure("long_line_edit.select_all", [&] {
    context.KeyDown(SDLK_A, SDL_KMOD_CTRL);
    context.PumpFrames(1);
  });
  context.Measure("long_line_edit.cut", [&] {
    context.KeyDown(SDLK_X, SDL_KMOD_CTRL);
    context.PumpFrames(1);
  });
  context.Measure("long_line_edit.paste", [&] {
    context.KeyDown(SDLK_V, SDL_KMOD_CTRL);
    context.PumpFrames(1);
  });
  context.Measure("long_line_edit.undo", [&] {
    context.KeyDown(SDLK_Z, SDL_KMOD_CTRL);
    context.PumpFrames(1);
  });
  context.Measure("long_line_edit.redo", [&] {
    context.KeyDown(SDLK_Y, SDL_KMOD_CTRL);
    context.PumpFrames(1);
  });

  if (viewport->lines().empty() || viewport->lines().front().size() != original_bytes) {
    throw std::runtime_error(
        "editor_long_line_select_all_edit: buffer corrupted after select-all/cut/paste/undo/redo");
  }

  viewport->MoveCursorTo(0, original_bytes / 2, false);
  context.PumpFrames(1);
  context.Measure("long_line_edit.replace_selection_burst", [&] {
    for (int i = 0; i < 8; ++i) {
      context.KeyDown(SDLK_LEFT, SDL_KMOD_SHIFT);
      context.Type("z");
      context.PumpFrames(1);
    }
  });
}

void RunFirstLineEditLatencyLargeFile(ScenarioContext& context) {
  const std::filesystem::path cpp_50k =
      "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";
  if (!RequireFixture(context, cpp_50k, "first_line_edit_latency_large_file")) {
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");
  context.OpenTab(cpp_50k);
  auto& vp = context.ActiveViewport();
  if (vp.lines().size() < 2) {
    throw std::runtime_error("first_line_edit_latency_large_file: file too short");
  }
  vp.MoveCursorTo(0, 0, false);
  context.PumpFrames(2);
  context.Measure("first_line_edit.enter_backspace_burst", [&] {
    for (int i = 0; i < 24; ++i) {
      context.KeyDown(SDLK_RETURN);
      context.KeyDown(SDLK_BACKSPACE);
      context.PumpFrames(1);
    }
  });
}

void RunMobyDickWorkout(ScenarioContext& context) {
  const std::filesystem::path moby =
      "tests/perf/fixtures/editor_essentials_moby_dick/moby-dick.txt";
  if (!RequireFixture(context, moby, "editor_moby_dick_workout")) {
    return;
  }
  (void)context.Open("tests/perf/fixtures/small_project");

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

  vp.MoveCursorTo(mid_line, 0, false);
  context.PumpFrames(2);
  const std::size_t scroll_before = vp.scroll_line();
  context.Measure("moby.mid_edit_burst", [&] {
    context.Type("call me Ishmael ");
    for (int i = 0; i < 12; ++i) {
      context.KeyDown(SDLK_RETURN);
      context.KeyDown(SDLK_BACKSPACE);
    }
    context.PumpFrames(1);
  });
  if (vp.scroll_line() + 100 < scroll_before) {
    throw std::runtime_error(
        "editor_moby_dick_workout: mid-document edit scroll-jumped toward the top");
  }
}

const ScenarioRegistration g_perf_editor_occurrences_scan({Scenario{
    .name = "editor_occurrences_scan",
    .smoke = false,
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
    .warmup_iterations = 12,
    .run = RunEditorShapingMultiCaret,
}});
const ScenarioRegistration g_perf_editor_toggle_comment_large({Scenario{
    .name = "editor_toggle_comment_large_selection",
    .smoke = false,
    .warmup_iterations = 12,
    .run = RunEditorToggleCommentLargeSelection,
}});
const ScenarioRegistration g_perf_editor_mouse_selection_drag({Scenario{
    .name = "editor_mouse_selection_drag",
    .smoke = false,
    .warmup_iterations = 1,
    .run = RunEditorMouseSelectionDrag,
}});
const ScenarioRegistration g_perf_menu_hover_switch({Scenario{
    .name = "menu_hover_switch",
    .smoke = false,
    .warmup_iterations = 1,
    .run = RunMenuHoverSwitch,
}});
const ScenarioRegistration g_perf_editor_cjk_scroll_paint({Scenario{
    .name = "editor_cjk_scroll_paint",
    .smoke = false,
    .warmup_iterations = 1,
    .run = RunEditorCjkScrollPaint,
}});
const ScenarioRegistration g_perf_editor_typing_paint({Scenario{
    .name = "editor_typing_paint",
    .smoke = false,
    .warmup_iterations = 1,
    .run = RunEditorTypingPaint,
}});
const ScenarioRegistration g_perf_menu_hover_paint({Scenario{
    .name = "menu_hover_paint",
    .smoke = false,
    .warmup_iterations = 1,
    .run = RunMenuHoverPaint,
}});
const ScenarioRegistration g_perf_menu_popup_hover_rows({Scenario{
    .name = "menu_popup_hover_rows",
    .smoke = false,
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
    .warmup_iterations = 1,
    .run = RunEditorSnippetExpand,
}});
const ScenarioRegistration g_perf_editor_snippet_placeholder_edit({Scenario{
    .name = "editor_snippet_placeholder_edit",
    .smoke = false,
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
    .warmup_iterations = 1,
    .tolerance_alloc_p50_percent = 1.0,
    .gate_net_heap_metrics = false,
    .run = RunSettingsChangeManyTabs,
}});
const ScenarioRegistration g_perf_editor_multi_caret_motion_burst({Scenario{
    .name = "editor_multi_caret_motion_burst",
    .smoke = false,
    .baseline_gated = true,
    .warmup_iterations = 1,
    .run = RunEditorMultiCaretMotionBurst,
}});
const ScenarioRegistration g_perf_editor_multi_caret_edit_burst({Scenario{
    .name = "editor_multi_caret_edit_burst",
    .smoke = false,
    .baseline_gated = true,
    .warmup_iterations = 1,
    .run = RunEditorMultiCaretEditBurst,
}});
const ScenarioRegistration g_perf_editor_column_selection_burst({Scenario{
    .name = "editor_column_selection_burst",
    .smoke = false,
    .baseline_gated = true,
    .run = RunEditorColumnSelectionBurst,
}});
const ScenarioRegistration g_perf_editor_typing_minified_line({Scenario{
    .name = "editor_typing_minified_line",
    .smoke = false,
    .baseline_gated = true,
    .warmup_iterations = 1,
    .tolerance_alloc_p50_percent = 1.0,
    .run = RunEditorTypingMinifiedLine,
}});
const ScenarioRegistration g_perf_editor_long_line_horizontal_scroll({Scenario{
    .name = "editor_long_line_horizontal_scroll",
    .smoke = false,
    .baseline_gated = true,
    .warmup_iterations = 1,
    .tolerance_alloc_p50_percent = 1.0,
    .run = RunEditorLongLineHorizontalScroll,
}});
const ScenarioRegistration g_perf_editor_long_line_buffer_search({Scenario{
    .name = "editor_long_line_buffer_search",
    .smoke = false,
    .baseline_gated = true,
    .warmup_iterations = 1,
    .tolerance_alloc_p50_percent = 1.0,
    .run = RunEditorLongLineBufferSearch,
}});
const ScenarioRegistration g_perf_editor_long_line_select_all_edit({Scenario{
    .name = "editor_long_line_select_all_edit",
    .smoke = false,
    .baseline_gated = true,
    .warmup_iterations = 1,
    .tolerance_alloc_p50_percent = 1.0,
    .run = RunEditorLongLineSelectAllEdit,
}});
const ScenarioRegistration g_perf_editor_soft_wrap_long_line_scroll({Scenario{
    .name = "editor_soft_wrap_long_line_scroll",
    .smoke = true,
    .baseline_gated = true,
    .warmup_iterations = 1,
    .run = RunEditorSoftWrapLongLineScroll,
}});
const ScenarioRegistration g_perf_editor_soft_wrap_long_line_typing({Scenario{
    .name = "editor_soft_wrap_long_line_typing",
    .smoke = true,
    .baseline_gated = true,
    .warmup_iterations = 1,
    .run = RunEditorSoftWrapLongLineTyping,
}});
const ScenarioRegistration g_perf_editor_soft_wrap_whitespace_paint({Scenario{
    .name = "editor_soft_wrap_whitespace_paint",
    .smoke = true,
    .warmup_iterations = 1,
    .run = RunEditorSoftWrapWhitespacePaint,
}});
const ScenarioRegistration g_perf_editor_moby_dick_workout({Scenario{
    .name = "editor_moby_dick_workout",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = false,
    .run = RunMobyDickWorkout,
}});

}
}
