#include "workspace/shell/WorkspaceShell.h"

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

// Plugin- and debug-hover resolution runs per mouse-move frame and walks the
// hovered line to map screen-x -> text column. Past this length the line is
// pathological (a minified bundle / a single-line multi-hundred-MB file), the
// same class of input the syntax highlighter already refuses to tokenize. Skip
// hover resolution rather than copy the whole line and walk it every frame; a
// hover popup on such a line is useless anyway. Guarding here also removes the
// former per-hover full-line LineVisualColumnMap allocation (~16 bytes/char),
// which for a 512 MB line was an ~8 GB alloc on a single mouse-move -> OOM.
constexpr std::size_t kMaxHoverResolveLineBytes = 1u << 20;  // 1 MiB

// The debug-hover path runs on every mouse-move frame, so an unguarded trace
// would flood the log. These dedupe on the reason's string-literal identity and
// only emit when the furthest-reached gate changes, keeping the log readable
// while still showing exactly where (and why) a debug hover stops short of a
// popup. No string materialization here keeps the render-string lint satisfied.
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

TextGridInteractionLayout BuildEditorInteractionLayout(
    const render::TextRenderer& text_renderer,
    const editor::TextViewport& viewport,
    const SDL_FRect& rect,
    bool show_line_numbers) {
  const editor::EditorViewMetrics metrics =
      editor::EditorViewRenderer::ComputeMetrics(text_renderer, viewport, rect, 0,
                                                 show_line_numbers);
  return ComputeTextGridInteractionLayout(
      rect, metrics.text_x, metrics.first_line_y, metrics.line_height, text_renderer.CharWidth(),
      viewport.scroll_line(), viewport.line_count(), viewport.horizontal_scroll(),
      metrics.visible_rows, metrics.visible_columns);
}

// Resolve the command bound to a clickable end-of-line code lens under (x,y) for
// a single viewport. Pure given its inputs: lays the line's EOL decorations out
// with the same helper the renderer uses, so the hit rect matches the painted
// lens exactly. Returns nullopt when nothing actionable is hit.
// Result of hit-testing a point against the compare view's RIGHT text grid.
struct CompareRightGridHit {
  // Adjusted for the hit row: `text_x` carries the row's hanging indent and
  // `horizontal_scroll` its first painted visual column, so everything that
  // resolves an x into a column off this layout is wrap-correct without knowing
  // about wrapping.
  TextGridInteractionLayout interaction;
  std::size_t visual_row = 0;  // ON-SCREEN row -- the interaction layout's space
  std::size_t model_row = 0;   // index into compare_tab.model.rows
  std::size_t visible_columns = 0;
};

// Resolve (x, y) to the compare view's right pane, or nullopt when the point is
// outside the grid, past the model, or on a row that has no right-file line (a
// pure deletion). Both compare hover paths — the diagnostic underline hit-test
// and the plugin hover query — resolved this with byte-identical code, so a fix
// to the surface math in one silently missed the other.
//
// The grid's row space is ON-SCREEN rows, which is what `compare_tab.scroll_row`
// counts. It used to be built with a model-row `line_count` against that scroll,
// so the two disagreed as soon as the diff collapsed a run — and, now, as soon as
// a line wrapped.
std::optional<CompareRightGridHit> HitTestCompareRightGrid(
    const render::TextRenderer& text_renderer,
    const WorkspaceShell::CompareSurfaceLayout& surface, const CompareTabState& compare_tab,
    float x, float y) {
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

  const std::span<const editor::CodeLensDecoration> code_lenses =
      decorations->CodeLensesForLine(static_cast<std::uint32_t>(*line_index));
  if (code_lenses.empty()) {
    return std::nullopt;
  }
  const std::span<const editor::InlineTextDecoration> inline_texts =
      decorations->InlineTextsForLine(static_cast<std::uint32_t>(*line_index));

  // Anchor at the line's last glyph and lay the segments out exactly as the
  // renderer does (same helper), so the hit rect matches the painted lens.
  const float line_y =
      interaction.first_line_y +
      static_cast<float>(*line_index - interaction.scroll_line) * interaction.line_height;
  // Push the anchor past any mid-line inlay hints exactly as the renderer does, so
  // the code-lens hit rect stays aligned with the painted lens. Suppressed on
  // soft-wrapped lines to match the render path (which omits hints there in v1).
  const std::size_t inlay_total_cells =
      viewport.soft_wrap()
          ? 0
          : editor::InlayLineTotalCells(inline_texts, text_renderer, text_renderer.CharWidth());
  const float anchor_x =
      interaction.text_x +
      static_cast<float>(viewport.VisibleLineLayout(*line_index).visual_columns + inlay_total_cells) *
          text_renderer.CharWidth();
  const float right_limit = interaction.rect.x + interaction.rect.w - 12.0f;
  std::vector<editor::EolDecorationSegment> segments;
  editor::BuildEolDecorationSegments(text_renderer, inline_texts, code_lenses, anchor_x, line_y,
                                     interaction.line_height, right_limit, segments);
  for (const editor::EolDecorationSegment& segment : segments) {
    if (segment.kind == editor::EolDecorationSegment::Kind::CodeLens &&
        Contains(segment.rect, x, y) && code_lenses[segment.index].activatable()) {
      return code_lenses[segment.index];
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<WorkspaceShell::EditorHoverTarget> WorkspaceShell::DiagnosticHoverTargetForViewport(
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
  // LineView, not lines[i]: `operator[]` copies the line into the buffer's line
  // cache, and this runs per painted frame while the pointer is over the text.
  const std::string_view line = viewport.lines().LineView(*line_index);
  for (const editor::PublishedDiagnostic& diagnostic : diagnostics) {
    const auto rect = editor::DiagnosticUnderlineRect(
        text_renderer_, interaction.text_x, line_y, interaction.line_height, line, *line_index,
        interaction.horizontal_scroll, interaction.visible_columns, viewport.tab_size(),
        diagnostic);
    if (!rect.has_value()) {
      continue;
    }
    const SDL_FRect hit_rect = MakeRect(rect->x, line_y, rect->w, interaction.line_height);
    if (Contains(hit_rect, x, y)) {
      return EditorHoverTarget{
          .kind = EditorHoverTarget::Kind::Diagnostic,
          .anchor_rect = *rect,
          .blame_line_index = 0,
          .diagnostic = diagnostic,
          .plugin_hover = std::nullopt,
      };
    }
  }

  return std::nullopt;
}

std::optional<WorkspaceShell::EditorHoverTarget> WorkspaceShell::PluginHoverTargetForLine(
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
  // TextColumnForVisualColumn walks only up to the hovered visual column and
  // returns line.size() once visual_column reaches/exceeds the line's visual
  // width, so an off-the-end hover is rejected here without materializing a
  // full-line visual-column map just to read its width.
  const std::size_t text_column =
      editor::TextLayout::TextColumnForVisualColumn(line_text, visual_column, tab_size);
  if (text_column >= line_text.size()) {
    return std::nullopt;
  }

  // Serve from the async cache: hit -> popup; pending/failed -> nothing; miss ->
  // record a kickoff for the non-const UpdateEditorHover (no Lua runs on this hot
  // hit-test path). The query itself runs on the plugin worker; its completion
  // re-drives this resolution into a cache hit.
  const std::size_t query_line = document_line + 1;
  const std::size_t query_column = text_column + 1;
  plugin::PluginHost::HoverResult hover;
  if (plugin_hover_cache_.Matches(path, query_line, query_column)) {
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
    // Cache miss for this cell: record the kickoff and show nothing yet.
    //
    // Field-by-field rather than `plugin_hover_cache_ = PluginHoverCache{...}`:
    // the aggregate destroys and rebuilds the path and both result strings, where
    // assignment reuses whatever capacity they already hold. The generation bump
    // is what an in-flight async completion compares against to learn its cell was
    // superseded, so it must happen on every re-point.
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
      MakeRect(TextGridCursorX(interaction, start_visual), line_y + interaction.line_height - 2.0f,
               std::max(1.0f, static_cast<float>(std::max<std::size_t>(1, end_visual - start_visual)) *
                                   interaction.char_width),
               2.0f);
  return EditorHoverTarget{
      .kind = EditorHoverTarget::Kind::Plugin,
      .anchor_rect = anchor_rect,
      .blame_line_index = 0,
      .diagnostic = std::nullopt,
      .plugin_hover = std::move(hover),
  };
}

std::optional<WorkspaceShell::EditorHoverTarget> WorkspaceShell::PluginHoverTargetForViewport(
    const editor::TextViewport& viewport,
    const TextGridInteractionLayout& interaction,
    float x,
    float y) const {
  if (viewport.path().empty() || viewport.dirty() || !Contains(interaction.rect, x, y)) {
    return std::nullopt;
  }

  const std::optional<std::size_t> line_index = VisibleTextGridLineAtY(interaction, y);
  if (!line_index.has_value() || *line_index >= viewport.lines().size()) {
    return std::nullopt;
  }

  // Plain editor: the visible grid row and the document line are the same value.
  return PluginHoverTargetForLine(viewport.path(), viewport.lines().LineView(*line_index),
                                  *line_index,
                                  *line_index, viewport.tab_size(), interaction, x, y);
}

std::optional<WorkspaceShell::EditorHoverTarget> WorkspaceShell::DiagnosticHoverTargetAtPosition(
    float x,
    float y) const {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::DiagnosticHoverTargetAtPosition");
  const HoverTargetsViewModel hover_targets_vm = RenderViewModelBuilder(context_).BuildHoverTargets();
  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return std::nullopt;
  }
  const WorkspaceLayout layout = *layout_state;

  // The editor render filters diagnostics by diagnostics.min_severity before
  // painting (WorkspaceShellRenderFrame). Apply the same filter here so hover
  // targets never exist for a diagnostic the user hid — otherwise an unpainted
  // diagnostic would still pop a card and its invisible span would intercept the
  // pointer. "hint" (the default) shows all, so the filter returns the span
  // untouched (zero cost). One reused scratch is safe: each filtered span is
  // consumed by its hit-test before the next filter call.
  const editor::DiagnosticSeverity diagnostics_min_severity =
      editor::ParseDiagnosticSeverity(GetSettingValue("diagnostics.min_severity").value_or("hint"));
  static thread_local std::vector<editor::PublishedDiagnostic> tls_hover_filtered_diagnostics;
  const auto visible_diagnostics =
      [&](std::span<const editor::PublishedDiagnostic> in) {
        return editor::FilterDiagnosticsAtLeastSeverity(in, diagnostics_min_severity,
                                                        tls_hover_filtered_diagnostics);
      };

  if (ActiveTabIsCompare()) {
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
        ComputeCompareSurfaceLayout(layout.editor_surface, *compare_tab);
    const std::optional<CompareRightGridHit> hit =
        HitTestCompareRightGrid(text_renderer_, surface, *compare_tab, x, y);
    if (!hit.has_value()) {
      return std::nullopt;
    }
    const TextGridInteractionLayout& interaction = hit->interaction;
    const std::size_t model_row_index = hit->model_row;
    const compare::CompareRow& row = compare_tab->model.rows[model_row_index];

    const std::size_t line_index = static_cast<std::size_t>(row.right_line - 1);
    // Use the shared signed helper keyed on the clamped interaction scroll: the
    // hand-rolled `*model_row - scroll_row` used the unclamped compare scroll_row,
    // which size_t-underflows to a huge Y if scroll_row transiently exceeds the
    // clamp (scrolled to bottom, then the window is enlarged before re-clamp).
    const float line_y = TextGridLineY(interaction, hit->visual_row);
    for (const editor::PublishedDiagnostic& diagnostic : diagnostics) {
      const auto rect = editor::DiagnosticUnderlineRect(
          text_renderer_, interaction.text_x, line_y, surface.line_height, row.right_text,
          line_index, interaction.horizontal_scroll, hit->visible_columns,
          compare_tab->right_viewport.tab_size(), diagnostic);
      if (!rect.has_value()) {
        continue;
      }
      const SDL_FRect hit_rect = MakeRect(rect->x, line_y, rect->w, surface.line_height);
      if (Contains(hit_rect, x, y)) {
        return EditorHoverTarget{
            .kind = EditorHoverTarget::Kind::Diagnostic,
            .anchor_rect = *rect,
            .blame_line_index = 0,
            .diagnostic = diagnostic,
            .plugin_hover = std::nullopt,
        };
      }
    }
    return std::nullopt;
  }

  if (ActiveTabIsMerge()) {
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

    const MergeSurfaceLayout surface = ComputeMergeSurfaceLayout(layout.editor_surface, *merge_tab);
    const SDL_FRect result_rect = ComputeMergeResultViewportRect(
        layout.editor_surface, surface.center_x, surface.rows_y, surface.gutter_width,
        surface.center_width, surface.show_horizontal);
    return DiagnosticHoverTargetForViewport(
        merge_tab->result_viewport,
        BuildEditorInteractionLayout(text_renderer_, merge_tab->result_viewport, result_rect,
                                     LineNumbersEnabled()),
        visible_diagnostics(std::span<const editor::PublishedDiagnostic>(*diagnostics)), x, y);
  }

  if (!ActiveTabIsEditor()) {
    return std::nullopt;
  }

  const auto panes = ComputeEditorPaneLayouts(layout);
  const editor::TextViewport* active_viewport = ActiveEditorViewport();
  if (panes.empty() && active_viewport != nullptr && !active_viewport->is_placeholder()) {
    const auto* diagnostics = hover_targets_vm.diagnostics_store->FindByPath(active_viewport->path());
    return diagnostics != nullptr
               ? DiagnosticHoverTargetForViewport(
                     *active_viewport,
                     BuildEditorInteractionLayout(text_renderer_, *active_viewport,
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
            *viewport, BuildEditorInteractionLayout(text_renderer_, *viewport, pane.rect, LineNumbersEnabled()),
            visible_diagnostics(std::span<const editor::PublishedDiagnostic>(*diagnostics)), x, y);
        target.has_value()) {
      return target;
    }
  }

  return std::nullopt;
}

std::optional<WorkspaceShell::EditorHoverTarget> WorkspaceShell::PluginHoverTargetAtPosition(
    float x,
    float y) const {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::PluginHoverTargetAtPosition");
  const auto layout_state = CurrentWorkspaceLayout();
  if (!layout_state.has_value()) {
    return std::nullopt;
  }
  const WorkspaceLayout layout = *layout_state;

  if (ActiveTabIsCompare()) {
    const CompareTabState* compare_tab = ActiveCompareTab();
    if (compare_tab == nullptr || !compare_tab->right_editable ||
        compare_tab->right_viewport.path().empty() || compare_tab->right_viewport.dirty()) {
      return std::nullopt;
    }

    const CompareSurfaceLayout surface =
        ComputeCompareSurfaceLayout(layout.editor_surface, *compare_tab);
    const std::optional<CompareRightGridHit> hit =
        HitTestCompareRightGrid(text_renderer_, surface, *compare_tab, x, y);
    if (!hit.has_value()) {
      return std::nullopt;
    }
    const TextGridInteractionLayout& interaction = hit->interaction;
    const std::size_t model_row_index = hit->model_row;
    const compare::CompareRow& row = compare_tab->model.rows[model_row_index];

    // Compare view: the visible grid row is the ON-SCREEN row (interaction space),
    // while the plugin query line is the right-file line it maps to. Passing the
    // file line for both (the old bug) made the row-identity check and anchor Y
    // wrong whenever any diff hunk sat above the cursor, so hover never appeared.
    const std::size_t document_line = static_cast<std::size_t>(row.right_line - 1);
    return PluginHoverTargetForLine(compare_tab->right_viewport.path(), row.right_text,
                                    hit->visual_row, document_line,
                                    compare_tab->right_viewport.tab_size(), interaction, x, y);
  }

  if (ActiveTabIsMerge()) {
    const MergeTabState* merge_tab = ActiveMergeTab();
    if (merge_tab == nullptr || merge_tab->result_viewport.path().empty() ||
        merge_tab->result_viewport.dirty()) {
      return std::nullopt;
    }

    const MergeSurfaceLayout surface = ComputeMergeSurfaceLayout(layout.editor_surface, *merge_tab);
    const SDL_FRect result_rect = ComputeMergeResultViewportRect(
        layout.editor_surface, surface.center_x, surface.rows_y, surface.gutter_width,
        surface.center_width, surface.show_horizontal);
    return PluginHoverTargetForViewport(
        merge_tab->result_viewport,
        BuildEditorInteractionLayout(text_renderer_, merge_tab->result_viewport, result_rect,
                                     LineNumbersEnabled()), x, y);
  }

  if (!ActiveTabIsEditor()) {
    return std::nullopt;
  }

  const auto panes = ComputeEditorPaneLayouts(layout);
  const editor::TextViewport* active_viewport = ActiveEditorViewport();
  if (panes.empty() && active_viewport != nullptr && !active_viewport->is_placeholder()) {
    return PluginHoverTargetForViewport(
        *active_viewport,
        BuildEditorInteractionLayout(text_renderer_, *active_viewport, layout.editor_surface,
                                     LineNumbersEnabled()), x, y);
  }

  for (const EditorPaneLayout& pane : panes) {
    const editor::TextViewport* viewport = ViewportForPane(pane);
    if (viewport == nullptr || viewport->path().empty() || viewport->dirty()) {
      continue;
    }

    if (const auto target = PluginHoverTargetForViewport(
            *viewport, BuildEditorInteractionLayout(text_renderer_, *viewport, pane.rect, LineNumbersEnabled()), x, y);
        target.has_value()) {
      return target;
    }
  }

  return std::nullopt;
}

std::optional<WorkspaceShell::EditorHoverTarget> WorkspaceShell::DebugValueHoverTargetForViewport(
    const editor::TextViewport& viewport,
    const TextGridInteractionLayout& interaction,
    float x,
    float y) const {
  if (viewport.path().empty() || viewport.dirty() || !Contains(interaction.rect, x, y)) {
    return std::nullopt;
  }

  // Debug execution + hover-eval state arrive through the view model (this is a
  // covered render-surface TU; it must not read project state directly). The
  // pointers are non-null only when the debug-hover gate held.
  const HoverTargetsViewModel hover_targets_vm =
      RenderViewModelBuilder(context_).BuildHoverTargets(/*debug_hover_enabled=*/true);
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
  // Only evaluate in the focused frame's source file: evaluate(frameId) resolves
  // names in that frame's lexical scope, so a token hovered in another file would
  // silently resolve against the wrong scope. Match the same way the execution-line
  // marker does (lexically-normalized generic strings).
  // path_key() is this exact value (editor::NormalizedPathKey), cached on the
  // document, so the hover path compares against it instead of re-normalizing and
  // re-allocating the viewport's path on every resolve.
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

  // Map the screen position to a text byte column (same geometry as the plugin
  // hover path), then resolve the identifier under it. TextColumnForVisualColumn
  // walks only to the hovered column and returns line.size() past the visual end,
  // so no full-line visual-column map is materialized on this per-frame path.
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

  // Serve from the async cache: hit → popup; pending/failed → nothing; miss →
  // record the kickoff for the non-const UpdateEditorHover (no DAP I/O here).
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
      TextGridCursorX(interaction, start_visual), line_y + interaction.line_height - 2.0f,
      std::max(1.0f, static_cast<float>(std::max<std::size_t>(1, end_visual - start_visual)) *
                         interaction.char_width),
      2.0f);
  return EditorHoverTarget{
      .kind = EditorHoverTarget::Kind::DebugValue,
      .anchor_rect = anchor_rect,
      .blame_line_index = 0,
      .diagnostic = std::nullopt,
      .plugin_hover = std::nullopt,
      .debug_value = DebugHoverValue{.value = hover.value, .type = hover.type},
  };
}

std::optional<WorkspaceShell::EditorHoverTarget> WorkspaceShell::DebugValueHoverTargetAtPosition(
    float x,
    float y) const {
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::DebugValueHoverTargetAtPosition");
  // Cheap session-level gates first: only paused, debugger-enabled sessions, and
  // only over editor tabs. We intentionally do NOT require the adapter to
  // advertise supportsEvaluateForHovers — RequestEvaluate falls back to the "repl"
  // context for adapters (e.g. GDB) that omit the capability but still evaluate.
  // (The focused-frame and per-viewport checks live in
  // DebugValueHoverTargetForViewport, which reads gated debug state via the view
  // model.)
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
        BuildEditorInteractionLayout(text_renderer_, *active_viewport, layout.editor_surface,
                                     LineNumbersEnabled()), x, y);
  }
  for (const EditorPaneLayout& pane : panes) {
    const editor::TextViewport* viewport = ViewportForPane(pane);
    if (viewport == nullptr || viewport->path().empty() || viewport->dirty()) {
      continue;
    }
    if (const auto target = DebugValueHoverTargetForViewport(
            *viewport, BuildEditorInteractionLayout(text_renderer_, *viewport, pane.rect, LineNumbersEnabled()), x, y);
        target.has_value()) {
      return target;
    }
  }
  return std::nullopt;
}

std::optional<WorkspaceShell::EditorHoverTarget> WorkspaceShell::EditorHoverTargetAtPosition(
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

  if (const auto diagnostic = DiagnosticHoverTargetAtPosition(x, y); diagnostic.has_value()) {
    return diagnostic;
  }

  // Debug values outrank LSP/plugin hover while a session is paused.
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
        text_renderer_, *active_viewport,
        BuildEditorInteractionLayout(text_renderer_, *active_viewport, layout.editor_surface,
                                     LineNumbersEnabled()),
        decorations_for(*active_viewport), x, y);
  }

  for (const EditorPaneLayout& pane : panes) {
    const editor::TextViewport* viewport = ViewportForPane(pane);
    if (viewport == nullptr || viewport->path().empty() || viewport->dirty()) {
      continue;
    }
    if (auto lens = CodeLensForViewport(
            text_renderer_, *viewport,
            BuildEditorInteractionLayout(text_renderer_, *viewport, pane.rect,
                                         LineNumbersEnabled()),
            decorations_for(*viewport), x, y);
        lens.has_value()) {
      return lens;
    }
  }
  return std::nullopt;
}

}  // namespace microide::workspace
