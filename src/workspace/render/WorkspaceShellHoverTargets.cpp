#include "workspace/shell/WorkspaceShell.h"

#include "workspace/render/CodeLensLayout.h"
#include "workspace/render/EditorInteractionGrid.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

#include "editor/DiagnosticsRender.h"
#include "editor/DiagnosticsStore.h"
#include "editor/EolDecorationLayout.h"
#include "editor/InlayHintColumns.h"
#include "editor/TextLayout.h"
#include "util/DebugTrace.h"
#include "util/PerformanceTrace.h"
#include "workspace/git/CompareTabReview.h"
#include "workspace/render/RenderViewModelBuilder.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

namespace {

constexpr std::size_t kMaxHoverResolveLineBytes = 1u << 20;

constexpr Uint64 kNoHoverRetryMs = 1000;

void LogHoverGate(const char* reason) {
  static const char* last = nullptr;
  if (!util::DebugTrace::Enabled() || reason == last) {
    return;
  }
  last = reason;
  util::DebugTrace::Note("hover-gate", reason);
}

void LogHoverGate(const char* reason, std::string_view expr) {
  static const char* last_reason = nullptr;
  if (!util::DebugTrace::Enabled() || reason == last_reason) {
    return;
  }
  last_reason = reason;
  util::DebugTrace::Note("hover-gate", reason, expr);
}


struct CompareRightGridHit {
  TextGridInteractionLayout interaction;
  std::size_t visual_row = 0;
  std::size_t model_row = 0;
  std::size_t visible_columns = 0;
};

std::optional<CompareRightGridHit> HitTestCompareRightGrid(const render::TextRenderer& text_renderer,
                                                           const workspace::CompareSurfaceLayout& surface,
                                                           const CompareTabState& compare_tab, float x, float y) {
  CompareRightGridHit hit;
  hit.interaction = ComputeTextGridInteractionLayout(
      MakeRect(surface.right_x, surface.rows_y, surface.gutter_width + surface.right_width,
               static_cast<float>(surface.visible_rows) * surface.line_height),
      surface.right_x + surface.gutter_width, surface.rows_y, surface.line_height,
      text_renderer.CharWidth(), static_cast<std::size_t>(std::max(0, compare_tab.scroll_row)),
      CompareTabVisualRowCount(compare_tab), compare_tab.horizontal_scroll,
      static_cast<std::size_t>(surface.visible_rows), surface.right_visible_columns);
  hit.visible_columns = surface.right_visible_columns;
  if (!Contains(hit.interaction.rect, x, y)) {
    return std::nullopt;
  }
  const std::optional<std::size_t> visual_row = VisibleTextGridLineAtY(hit.interaction, y);
  if (!visual_row.has_value()) {
    return std::nullopt;
  }
  const DiffWrapRow wrap_row = compare_tab.wrap_layout.RowAt(*visual_row);
  if (!wrap_row.right_present) {
    return std::nullopt;
  }
  const compare::ComparePresentationRow* presentation_row =
      CompareTabPresentationRowAt(compare_tab, wrap_row.unit);
  const std::size_t model_row =
      presentation_row != nullptr &&
              presentation_row->kind == compare::ComparePresentationRowKind::Model
          ? presentation_row->model_row_index
          : (compare_tab.presentation.rows.empty() ? wrap_row.unit
                                                   : compare_tab.model.rows.size());
  if (model_row >= compare_tab.model.rows.size()) {
    return std::nullopt;
  }
  const compare::CompareRow& row = compare_tab.model.rows[model_row];
  if (row.right_line <= 0 ||
      static_cast<std::size_t>(row.right_line) > compare_tab.right_viewport.lines().size()) {
    return std::nullopt;
  }
  if (compare_tab.wrap_layout.active()) {
    hit.interaction.text_x += static_cast<float>(wrap_row.right_indent) * text_renderer.CharWidth();
    hit.interaction.horizontal_scroll = wrap_row.right_start;
    hit.visible_columns = wrap_row.right_end > wrap_row.right_start
                              ? wrap_row.right_end - wrap_row.right_start
                              : 0;
  }
  hit.visual_row = *visual_row;
  hit.model_row = model_row;
  return hit;
}

std::optional<editor::CodeLensDecoration> CodeLensForViewport(
    const render::TextRenderer& text_renderer, const editor::TextViewport& viewport,
    const TextGridInteractionLayout& interaction, const editor::FileDecorations* decorations,
    float x, float y) {
  if (decorations == nullptr || viewport.path().empty() || viewport.dirty() ||
      !Contains(interaction.rect, x, y)) {
    return std::nullopt;
  }

  const std::optional<std::size_t> line_index = VisibleTextGridLineAtY(interaction, y);
  if (!line_index.has_value() || *line_index >= viewport.lines().size()) {
    return std::nullopt;
  }

  std::vector<editor::EolDecorationSegment> segments;
  std::optional<editor::CodeLensDecoration> hit;
  ForEachLineCodeLens(text_renderer, viewport, interaction, *decorations, *line_index, segments,
                      [&](const editor::CodeLensDecoration& lens, const SDL_FRect& rect) {
                        if (!hit.has_value() && Contains(rect, x, y)) {
                          hit = lens;
                        }
                      });
  return hit;
}

}

std::optional<EditorHoverTarget> WorkspaceShell::DiagnosticHoverTargetForViewport(
    const editor::TextViewport& viewport,
    const TextGridInteractionLayout& interaction,
    std::span<const editor::PublishedDiagnostic> diagnostics,
    float x,
    float y) const {
  if (viewport.path().empty() || diagnostics.empty() ||
      !Contains(interaction.rect, x, y)) {
    return std::nullopt;
  }

  const std::optional<std::size_t> line_index = VisibleTextGridLineAtY(interaction, y);
  if (!line_index.has_value() || *line_index >= viewport.lines().size()) {
    return std::nullopt;
  }

  const float line_y =
      interaction.first_line_y +
      static_cast<float>(*line_index - interaction.scroll_line) * interaction.line_height;
  const std::string_view line = viewport.lines().LineView(*line_index);
  for (const editor::PublishedDiagnostic& diagnostic : diagnostics) {
    const auto rect = editor::DiagnosticUnderlineRect(
        editor_text_renderer_, interaction.text_x, line_y, interaction.line_height, line, *line_index,
        interaction.horizontal_scroll, interaction.visible_columns, viewport.tab_size(),
        diagnostic);
    if (!rect.has_value()) {
      continue;
    }
    const SDL_FRect hit_rect = MakeRect(rect->x, line_y, rect->w, interaction.line_height);
    if (Contains(hit_rect, x, y)) {
      return EditorHoverTarget{
          .kind = EditorHoverTarget::Kind::Diagnostic,
          .anchor_rect = hit_rect,
          .blame_line_index = 0,
          .diagnostic = diagnostic,
          .plugin_hover = std::nullopt,
      };
    }
  }

  return std::nullopt;
}

std::optional<EditorHoverTarget> WorkspaceShell::PluginHoverTargetForLine(
    const std::filesystem::path& path,
    std::string_view line,
    std::size_t grid_row,
    std::size_t document_line,
    std::size_t tab_size,
    const TextGridInteractionLayout& interaction,
    float x,
    float y) const {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::PluginHoverTargetForLine");
  if (path.empty() || x < interaction.text_x || !Contains(interaction.rect, x, y)) {
    return std::nullopt;
  }

  const std::optional<std::size_t> hovered_line = VisibleTextGridLineAtY(interaction, y);
  if (!hovered_line.has_value() || *hovered_line != grid_row) {
    return std::nullopt;
  }

  if (line.empty() || line.size() > kMaxHoverResolveLineBytes) {
    return std::nullopt;
  }
  const std::string_view line_text = line;

  const float local_x = std::max(0.0f, x - interaction.text_x);
  const std::size_t visual_column =
      interaction.horizontal_scroll +
      static_cast<std::size_t>(std::floor(local_x / std::max(1.0f, interaction.char_width)));
  const std::size_t text_column =
      editor::TextLayout::TextColumnForVisualColumn(line_text, visual_column, tab_size);
  if (text_column >= line_text.size()) {
    return std::nullopt;
  }

  const std::size_t query_line = document_line + 1;
  const std::size_t query_column = text_column + 1;
  plugin::PluginHost::HoverResult hover;
  const bool retry_no_answer = plugin_hover_cache_.state == PluginHoverCache::State::Failed &&
                               SDL_GetTicks() - plugin_hover_cache_.failed_at_ms >= kNoHoverRetryMs;
  if (plugin_hover_cache_.Matches(path, query_line, query_column) && !retry_no_answer) {
    switch (plugin_hover_cache_.state) {
      case PluginHoverCache::State::Resolved:
        hover = plugin_hover_cache_.result;
        break;
      case PluginHoverCache::State::Kickoff:
      case PluginHoverCache::State::Pending:
      case PluginHoverCache::State::Failed:
      case PluginHoverCache::State::Empty:
        return std::nullopt;
    }
  } else {
    plugin_hover_cache_.path = path;
    plugin_hover_cache_.line = query_line;
    plugin_hover_cache_.column = query_column;
    plugin_hover_cache_.state = PluginHoverCache::State::Kickoff;
    plugin_hover_cache_.result.title.clear();
    plugin_hover_cache_.result.content.clear();
    ++plugin_hover_cache_.generation;
    return std::nullopt;
  }

  const std::size_t start_visual =
      editor::TextLayout::VisualColumnForTextColumn(line_text, text_column, tab_size);
  const std::size_t end_visual =
      text_column < line_text.size()
          ? editor::TextLayout::VisualColumnForTextColumn(
                line_text, editor::TextLayout::NextTextColumn(line_text, text_column), tab_size)
          : start_visual + 1;
  const float line_y = TextGridLineY(interaction, grid_row);
  const SDL_FRect anchor_rect =
      MakeRect(TextGridCursorX(interaction, start_visual), line_y,
               std::max(1.0f, static_cast<float>(std::max<std::size_t>(1, end_visual - start_visual)) *
                                   interaction.char_width),
               interaction.line_height);
  return EditorHoverTarget{
      .kind = EditorHoverTarget::Kind::Plugin,
      .anchor_rect = anchor_rect,
      .blame_line_index = 0,
      .diagnostic = std::nullopt,
      .plugin_hover = std::move(hover),
  };
}

std::optional<EditorHoverTarget> WorkspaceShell::PluginHoverTargetForViewport(
    const editor::TextViewport& viewport,
    const TextGridInteractionLayout& interaction,
    float x,
    float y) const {
  if (viewport.path().empty() || !Contains(interaction.rect, x, y)) {
    return std::nullopt;
  }

  const std::optional<std::size_t> line_index = VisibleTextGridLineAtY(interaction, y);
  if (!line_index.has_value() || *line_index >= viewport.lines().size()) {
    return std::nullopt;
  }

  return PluginHoverTargetForLine(viewport.path(), viewport.lines().LineView(*line_index),
                                  *line_index,
                                  *line_index, viewport.tab_size(), interaction, x, y);
}

std::optional<EditorHoverTarget> WorkspaceShell::DiagnosticHoverTargetAtPosition(
    float x,
    float y) const {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::DiagnosticHoverTargetAtPosition");
  const HoverTargetsViewModel hover_targets_vm = RenderViewModelBuilder(context_).BuildHoverTargets();
  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return std::nullopt;
  }
  const WorkspaceLayout layout = *layout_state;

  const editor::DiagnosticSeverity diagnostics_min_severity =
      editor::ParseDiagnosticSeverity(GetSettingValue("diagnostics.min_severity").value_or("hint"));
  static thread_local std::vector<editor::PublishedDiagnostic> tls_hover_filtered_diagnostics;
  const auto visible_diagnostics =
      [&](std::span<const editor::PublishedDiagnostic> in) {
        return editor::FilterDiagnosticsAtLeastSeverity(in, diagnostics_min_severity,
                                                        tls_hover_filtered_diagnostics);
      };

  const bool over_view_surface = Contains(ActiveTabSurface(layout), x, y);
  if (ActiveTabIsCompare() && over_view_surface) {
    const CompareTabState* compare_tab = ActiveCompareTab();
    if (compare_tab == nullptr || !compare_tab->right_editable ||
        compare_tab->right_viewport.path().empty() || compare_tab->right_viewport.dirty()) {
      return std::nullopt;
    }

    const auto* diagnostics_all =
        hover_targets_vm.diagnostics_store->FindByPathKey(compare_tab->right_viewport.path_key());
    if (diagnostics_all == nullptr || diagnostics_all->empty()) {
      return std::nullopt;
    }
    const std::span<const editor::PublishedDiagnostic> diagnostics =
        visible_diagnostics(std::span<const editor::PublishedDiagnostic>(*diagnostics_all));

    const CompareSurfaceLayout surface =
        ComputeCompareSurfaceLayout(ActiveTabSurface(layout), *compare_tab);
    const std::optional<CompareRightGridHit> hit =
        HitTestCompareRightGrid(editor_text_renderer_, surface, *compare_tab, x, y);
    if (!hit.has_value()) {
      return std::nullopt;
    }
    const TextGridInteractionLayout& interaction = hit->interaction;
    const std::size_t model_row_index = hit->model_row;
    const compare::CompareRow& row = compare_tab->model.rows[model_row_index];

    const std::size_t line_index = static_cast<std::size_t>(row.right_line - 1);
    const float line_y = TextGridLineY(interaction, hit->visual_row);
    for (const editor::PublishedDiagnostic& diagnostic : diagnostics) {
      const auto rect = editor::DiagnosticUnderlineRect(
          editor_text_renderer_, interaction.text_x, line_y, surface.line_height, row.right_text,
          line_index, interaction.horizontal_scroll, hit->visible_columns,
          compare_tab->right_viewport.tab_size(), diagnostic);
      if (!rect.has_value()) {
        continue;
      }
      const SDL_FRect hit_rect = MakeRect(rect->x, line_y, rect->w, surface.line_height);
      if (Contains(hit_rect, x, y)) {
        return EditorHoverTarget{
            .kind = EditorHoverTarget::Kind::Diagnostic,
            .anchor_rect = hit_rect,
            .blame_line_index = 0,
            .diagnostic = diagnostic,
            .plugin_hover = std::nullopt,
        };
      }
    }
    return std::nullopt;
  }

  if (ActiveTabIsMerge() && over_view_surface) {
    const MergeTabState* merge_tab = ActiveMergeTab();
    if (merge_tab == nullptr || merge_tab->result_viewport.path().empty() ||
        merge_tab->result_viewport.dirty()) {
      return std::nullopt;
    }

    const auto* diagnostics =
        hover_targets_vm.diagnostics_store->FindByPathKey(merge_tab->result_viewport.path_key());
    if (diagnostics == nullptr || diagnostics->empty()) {
      return std::nullopt;
    }

    const MergeSurfaceLayout surface = ComputeMergeSurfaceLayout(ActiveTabSurface(layout), *merge_tab);
    const SDL_FRect result_rect = ComputeMergeResultViewportRect(
        ActiveTabSurface(layout), surface.center_x, surface.rows_y, surface.gutter_width,
        surface.center_width, surface.show_horizontal);
    return DiagnosticHoverTargetForViewport(
        merge_tab->result_viewport,
        BuildEditorInteractionGrid(editor_text_renderer_, merge_tab->result_viewport, result_rect,
                                     LineNumbersEnabled()),
        visible_diagnostics(std::span<const editor::PublishedDiagnostic>(*diagnostics)), x, y);
  }

  if (!ActiveTabIsEditor() && !ActiveTabIsCompare() && !ActiveTabIsMerge()) {
    return std::nullopt;
  }

  const auto panes = ComputeEditorPaneLayouts(layout);
  const editor::TextViewport* active_viewport = ActiveEditorViewport();
  if (panes.empty() && active_viewport != nullptr && !active_viewport->is_placeholder()) {
    const auto* diagnostics = hover_targets_vm.diagnostics_store->FindByPath(active_viewport->path());
    return diagnostics != nullptr
               ? DiagnosticHoverTargetForViewport(
                     *active_viewport,
                     BuildEditorInteractionGrid(editor_text_renderer_, *active_viewport,
                                                  layout.editor_surface, LineNumbersEnabled()),
                     visible_diagnostics(std::span<const editor::PublishedDiagnostic>(*diagnostics)),
                     x, y)
               : std::nullopt;
  }

  for (const EditorPaneLayout& pane : panes) {
    const editor::TextViewport* viewport = ViewportForPane(pane);
    if (viewport == nullptr || viewport->path().empty()) {
      continue;
    }

    const auto* diagnostics = hover_targets_vm.diagnostics_store->FindByPath(viewport->path());
    if (diagnostics == nullptr || diagnostics->empty()) {
      continue;
    }

    if (const auto target = DiagnosticHoverTargetForViewport(
            *viewport, BuildEditorInteractionGrid(editor_text_renderer_, *viewport, pane.rect, LineNumbersEnabled()),
            visible_diagnostics(std::span<const editor::PublishedDiagnostic>(*diagnostics)), x, y);
        target.has_value()) {
      return target;
    }
  }

  return std::nullopt;
}

std::optional<EditorHoverTarget> WorkspaceShell::PluginHoverTargetAtPosition(
    float x,
    float y) const {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::PluginHoverTargetAtPosition");
  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return std::nullopt;
  }
  const WorkspaceLayout layout = *layout_state;

  const bool over_view_surface = Contains(ActiveTabSurface(layout), x, y);
  if (ActiveTabIsCompare() && over_view_surface) {
    const CompareTabState* compare_tab = ActiveCompareTab();
    if (compare_tab == nullptr || !compare_tab->right_editable ||
        compare_tab->right_viewport.path().empty()) {
      return std::nullopt;
    }

    const CompareSurfaceLayout surface =
        ComputeCompareSurfaceLayout(ActiveTabSurface(layout), *compare_tab);
    const std::optional<CompareRightGridHit> hit =
        HitTestCompareRightGrid(editor_text_renderer_, surface, *compare_tab, x, y);
    if (!hit.has_value()) {
      return std::nullopt;
    }
    const TextGridInteractionLayout& interaction = hit->interaction;
    const std::size_t model_row_index = hit->model_row;
    const compare::CompareRow& row = compare_tab->model.rows[model_row_index];

    const std::size_t document_line = static_cast<std::size_t>(row.right_line - 1);
    return PluginHoverTargetForLine(compare_tab->right_viewport.path(), row.right_text,
                                    hit->visual_row, document_line,
                                    compare_tab->right_viewport.tab_size(), interaction, x, y);
  }

  if (ActiveTabIsMerge() && over_view_surface) {
    const MergeTabState* merge_tab = ActiveMergeTab();
    if (merge_tab == nullptr || merge_tab->result_viewport.path().empty()) {
      return std::nullopt;
    }

    const MergeSurfaceLayout surface = ComputeMergeSurfaceLayout(ActiveTabSurface(layout), *merge_tab);
    const SDL_FRect result_rect = ComputeMergeResultViewportRect(
        ActiveTabSurface(layout), surface.center_x, surface.rows_y, surface.gutter_width,
        surface.center_width, surface.show_horizontal);
    return PluginHoverTargetForViewport(
        merge_tab->result_viewport,
        BuildEditorInteractionGrid(editor_text_renderer_, merge_tab->result_viewport, result_rect,
                                     LineNumbersEnabled()), x, y);
  }

  if (!ActiveTabIsEditor() && !ActiveTabIsCompare() && !ActiveTabIsMerge()) {
    return std::nullopt;
  }

  const auto panes = ComputeEditorPaneLayouts(layout);
  const editor::TextViewport* active_viewport = ActiveEditorViewport();
  if (panes.empty() && active_viewport != nullptr && !active_viewport->is_placeholder()) {
    return PluginHoverTargetForViewport(
        *active_viewport,
        BuildEditorInteractionGrid(editor_text_renderer_, *active_viewport, layout.editor_surface,
                                     LineNumbersEnabled()), x, y);
  }

  for (const EditorPaneLayout& pane : panes) {
    const editor::TextViewport* viewport = ViewportForPane(pane);
    if (viewport == nullptr || viewport->path().empty()) {
      continue;
    }

    if (const auto target = PluginHoverTargetForViewport(
            *viewport, BuildEditorInteractionGrid(editor_text_renderer_, *viewport, pane.rect, LineNumbersEnabled()), x, y);
        target.has_value()) {
      return target;
    }
  }

  return std::nullopt;
}

std::optional<EditorHoverTarget> WorkspaceShell::DebugValueHoverTargetForViewport(
    const editor::TextViewport& viewport,
    const TextGridInteractionLayout& interaction,
    float x,
    float y) const {
  if (viewport.path().empty() || viewport.dirty() || !Contains(interaction.rect, x, y)) {
    return std::nullopt;
  }

  const HoverTargetsViewModel hover_targets_vm =
      RenderViewModelBuilder(context_).BuildHoverTargets(true);
  if (hover_targets_vm.debug_execution == nullptr || hover_targets_vm.debug_hover == nullptr) {
    LogHoverGate("vm-debug-state-null");
    return std::nullopt;
  }
  const DebugExecutionView& execution = *hover_targets_vm.debug_execution;
  const DebugStackFrameView* frame = execution.FocusedFrame();
  if (frame == nullptr) {
    LogHoverGate("no-focused-frame");
    return std::nullopt;
  }
  if (viewport.path_key() != execution.FocusedPathNormalized()) {
    LogHoverGate("file-not-focused-frame");
    return std::nullopt;
  }

  const std::optional<std::size_t> hovered_line = VisibleTextGridLineAtY(interaction, y);
  if (!hovered_line.has_value() || *hovered_line >= viewport.lines().size()) {
    return std::nullopt;
  }
  const std::string_view line_text = viewport.lines().LineView(*hovered_line);
  if (line_text.empty() || line_text.size() > kMaxHoverResolveLineBytes) {
    return std::nullopt;
  }

  const float local_x = std::max(0.0f, x - interaction.text_x);
  const std::size_t visual_column =
      interaction.horizontal_scroll +
      static_cast<std::size_t>(std::floor(local_x / std::max(1.0f, interaction.char_width)));
  const std::size_t text_column =
      editor::TextLayout::TextColumnForVisualColumn(line_text, visual_column, viewport.tab_size());
  if (text_column >= line_text.size()) {
    return std::nullopt;
  }

  const editor::TextLayout::ByteRange range =
      editor::TextLayout::IdentifierRangeAt(line_text, text_column);
  if (range.empty()) {
    return std::nullopt;
  }
  std::string expression(line_text.substr(range.start, range.end - range.start));

  const DebugHoverModel& hover = *hover_targets_vm.debug_hover;
  const int frame_id = frame->id;
  switch (hover.Classify(frame_id, expression)) {
    case DebugHoverModel::Lookup::Hit:
      LogHoverGate("cache-hit -> popup", expression);
      break;
    case DebugHoverModel::Lookup::Pending:
      LogHoverGate("cache-pending", expression);
      return std::nullopt;
    case DebugHoverModel::Lookup::Failed:
      LogHoverGate("cache-failed", expression);
      return std::nullopt;
    case DebugHoverModel::Lookup::Miss:
      LogHoverGate("cache-miss -> kickoff", expression);
      pending_hover_eval_ = PendingHoverEval{frame_id, std::move(expression), true};
      return std::nullopt;
  }

  const std::size_t start_visual =
      editor::TextLayout::VisualColumnForTextColumn(line_text, range.start, viewport.tab_size());
  const std::size_t end_visual =
      editor::TextLayout::VisualColumnForTextColumn(line_text, range.end, viewport.tab_size());
  const float line_y = TextGridLineY(interaction, *hovered_line);
  const SDL_FRect anchor_rect = MakeRect(
      TextGridCursorX(interaction, start_visual), line_y,
      std::max(1.0f, static_cast<float>(std::max<std::size_t>(1, end_visual - start_visual)) *
                         interaction.char_width),
      interaction.line_height);
  return EditorHoverTarget{
      .kind = EditorHoverTarget::Kind::DebugValue,
      .anchor_rect = anchor_rect,
      .blame_line_index = 0,
      .diagnostic = std::nullopt,
      .plugin_hover = std::nullopt,
      .debug_value = DebugHoverValue{.value = hover.value, .type = hover.type},
  };
}

std::optional<EditorHoverTarget> WorkspaceShell::DebugValueHoverTargetAtPosition(
    float x,
    float y) const {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::DebugValueHoverTargetAtPosition");
  if (!DebugEnabled()) {
    LogHoverGate("debug-disabled");
    return std::nullopt;
  }
  if (!IsDebugSessionStopped()) {
    LogHoverGate("not-stopped");
    return std::nullopt;
  }
  if (!ActiveTabIsEditor()) {
    LogHoverGate("not-editor-tab");
    return std::nullopt;
  }
  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return std::nullopt;
  }
  const WorkspaceLayout layout = *layout_state;
  const auto panes = ComputeEditorPaneLayouts(layout);
  const editor::TextViewport* active_viewport = ActiveEditorViewport();
  if (panes.empty() && active_viewport != nullptr && !active_viewport->is_placeholder()) {
    return DebugValueHoverTargetForViewport(
        *active_viewport,
        BuildEditorInteractionGrid(editor_text_renderer_, *active_viewport, layout.editor_surface,
                                     LineNumbersEnabled()), x, y);
  }
  for (const EditorPaneLayout& pane : panes) {
    const editor::TextViewport* viewport = ViewportForPane(pane);
    if (viewport == nullptr || viewport->path().empty() || viewport->dirty()) {
      continue;
    }
    if (const auto target = DebugValueHoverTargetForViewport(
            *viewport, BuildEditorInteractionGrid(editor_text_renderer_, *viewport, pane.rect, LineNumbersEnabled()), x, y);
        target.has_value()) {
      return target;
    }
  }
  return std::nullopt;
}

std::optional<EditorHoverTarget> WorkspaceShell::EditorHoverTargetAtPosition(
    float x,
    float y) const {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::EditorHoverTargetAtPosition");
  const HoverTargetsViewModel hover_targets_vm = RenderViewModelBuilder(context_).BuildHoverTargets();
  if (!hover_targets_vm.hover_enabled) {
    return std::nullopt;
  }

  if (const editor::EditorBlameLine* blame_line = editor_blame_overlay_service_.LineAtPosition(x, y);
      blame_line != nullptr && blame_line->interactive) {
    return EditorHoverTarget{
        .kind = EditorHoverTarget::Kind::Blame,
        .anchor_rect = blame_line->rect,
        .blame_line_index = blame_line->line_index,
        .diagnostic = std::nullopt,
        .plugin_hover = std::nullopt,
    };
  }

  if (auto diagnostic = DiagnosticHoverTargetAtPosition(x, y); diagnostic.has_value()) {
    if (const auto symbol = PluginHoverTargetAtPosition(x, y);
        symbol.has_value() && symbol->plugin_hover.has_value()) {
      diagnostic->plugin_hover = symbol->plugin_hover;
    }
    return diagnostic;
  }

  if (const auto debug_value = DebugValueHoverTargetAtPosition(x, y); debug_value.has_value()) {
    return debug_value;
  }

  return PluginHoverTargetAtPosition(x, y);
}

std::optional<editor::CodeLensDecoration> WorkspaceShell::CodeLensAtPosition(float x,
                                                                             float y) const {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::CodeLensAtPosition");
  if (!ActiveTabIsEditor()) {
    return std::nullopt;
  }
  const HoverTargetsViewModel hover_targets_vm = RenderViewModelBuilder(context_).BuildHoverTargets();
  if (hover_targets_vm.decoration_store == nullptr) {
    return std::nullopt;
  }
  const editor::PluginDecorationStore& store = *hover_targets_vm.decoration_store;
  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return std::nullopt;
  }
  const WorkspaceLayout layout = *layout_state;

  const auto decorations_for = [&store](const editor::TextViewport& viewport)
      -> const editor::FileDecorations* {
    if (viewport.path().empty() || viewport.dirty()) {
      return nullptr;
    }
    return store.FindByPathKey(viewport.path_key());
  };

  const auto panes = ComputeEditorPaneLayouts(layout);
  const editor::TextViewport* active_viewport = ActiveEditorViewport();
  if (panes.empty() && active_viewport != nullptr && !active_viewport->is_placeholder()) {
    return CodeLensForViewport(
        editor_text_renderer_, *active_viewport,
        BuildEditorInteractionGrid(editor_text_renderer_, *active_viewport, layout.editor_surface,
                                     LineNumbersEnabled()),
        decorations_for(*active_viewport), x, y);
  }

  for (const EditorPaneLayout& pane : panes) {
    const editor::TextViewport* viewport = ViewportForPane(pane);
    if (viewport == nullptr || viewport->path().empty() || viewport->dirty()) {
      continue;
    }
    if (auto lens = CodeLensForViewport(
            editor_text_renderer_, *viewport,
            BuildEditorInteractionGrid(editor_text_renderer_, *viewport, pane.rect,
                                         LineNumbersEnabled()),
            decorations_for(*viewport), x, y);
        lens.has_value()) {
      return lens;
    }
  }
  return std::nullopt;
}

}
