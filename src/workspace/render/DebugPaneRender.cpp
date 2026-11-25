#include "workspace/render/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <vector>

#include "render/DevicePixelSnap.h"
#include "render/PaintElement.h"
#include "util/PerformanceTrace.h"
#include "workspace/debug/DebugPaneRegistry.h"
#include "workspace/render/DebugPanePaint.h"
#include "workspace/render/SingleLineViewMetrics.h"

namespace microide::workspace {

using namespace detail;

namespace {

SDL_Color ValueKindColor(const render::Theme& theme, DebugValueKind kind) {
  switch (kind) {
    case DebugValueKind::Number:
      return theme.syntax_number;
    case DebugValueKind::String:
      return theme.syntax_string;
    case DebugValueKind::Pointer:
      return theme.syntax_constant;
    case DebugValueKind::Boolean:
      return theme.syntax_keyword;
    case DebugValueKind::Aggregate:
    case DebugValueKind::Plain:
    case DebugValueKind::Scope:
    case DebugValueKind::Pending:
    case DebugValueKind::Error:
      return theme.text_secondary;
  }
  return theme.text_secondary;
}

class DebugPaneRowLayout {
 public:
  DebugPaneRowLayout(PreparedFrameState::DebugPaneBodyPaint& body, const DebugPaneInputs& inputs)
      : body_(body), in_(inputs), theme_(*inputs.theme), text_(*inputs.text_renderer) {}

  PreparedFrameState::DebugPaneRowPaint& Row(int row, bool lit, bool hoverable = true) {
    PreparedFrameState::DebugPaneRowPaint& painted = body_.rows.emplace_back();
    painted.band = MakeRect(in_.content.x, DebugPaneRowBandTop(in_.text_y, in_.line_height, row), in_.content.w,
                            in_.line_height);
    painted.text_y = painted.band.y;
    painted.highlighted = lit || (hoverable && in_.pointer_over(painted.band));
    return painted;
  }
  static void Run(PreparedFrameState::DebugPaneRowPaint& row, float x, float width, SDL_Color color,
                  std::string_view text) {
    row.runs.push_back(DebugTextRun{.x = x, .width = width, .color = color, .text = text});
  }
  void TwoColumns(PreparedFrameState::DebugPaneRowPaint& row, float x, float width, std::string_view primary,
                  SDL_Color primary_color, std::string_view secondary, SDL_Color secondary_color) {
    Run(row, x, width, primary_color, primary);
    if (secondary.empty()) {
      return;
    }
    const float secondary_x = x + text_.MeasureWidth(primary) + 12.0f;
    if (const float secondary_w = x + width - secondary_x; secondary_w > 0.0f) {
      Run(row, secondary_x, secondary_w, secondary_color, secondary);
    }
  }

  void CallStackRow(const DebugExecutionView& view, std::size_t index, int row) {
    constexpr float kIndentStep = 16.0f;
    const auto ref = view.PanelRowAt(index);
    if (ref.kind == DebugExecutionView::PanelRowRef::Kind::Session) {
      const DebugSessionView& session = view.sessions[ref.index];
      const bool focused = session.id == view.focused_session_id;
      SDL_Color color = focused ? theme_.text_primary : theme_.text_secondary;
      if (session.attention && !focused) {
        color = theme_.accent;
      }
      Run(Row(row, focused), in_.text_x, in_.text_width, color, session.display);
      return;
    }
    const float thread_indent = view.HasSessionSelector() ? kIndentStep : 0.0f;
    if (ref.kind == DebugExecutionView::PanelRowRef::Kind::Thread) {
      const DebugThreadView& thread = view.threads[ref.index];
      const bool focused = thread.id == view.focused_thread_id;
      Run(Row(row, focused), in_.text_x + thread_indent, in_.text_width - thread_indent,
          focused ? theme_.text_primary : theme_.text_secondary, thread.display);
      return;
    }
    const DebugStackFrameView& stack_frame = view.frames[ref.index];
    const bool focused = ref.index == view.focused_frame_index;
    const float indent = thread_indent + (view.HasThreadSelector() ? kIndentStep : 0.0f);
    TwoColumns(Row(row, focused), in_.text_x + indent, in_.text_width - indent, stack_frame.display_primary,
               focused ? theme_.text_primary : theme_.text_secondary, stack_frame.display_secondary,
               theme_.text_muted);
  }

  template <typename Model>
  void ValueTreeRow(const Model& model, std::size_t index, int row) {
    const std::vector<DebugVariableRowView>& rows = model.Rows();
    if (index >= rows.size()) {
      return;
    }
    const DebugVariableRowView& var = rows[index];
    PreparedFrameState::DebugPaneRowPaint& painted = Row(row, index == model.SelectedRow());
    const float row_x = in_.text_x + static_cast<float>(var.depth) * kDebugPaneTreeIndentStep;
    const float name_x = row_x + kDebugPaneTreeDisclosureSlot;
    const float content_right = in_.text_x + in_.text_width;
    const float name_avail = content_right - name_x;
    if (name_avail <= 0.0f) {
      return;
    }
    if (var.is_placeholder || var.is_show_more || var.kind == DebugValueKind::Scope) {
      const SDL_Color color = var.is_placeholder ? theme_.text_muted : theme_.accent;
      if (!var.is_placeholder && !var.is_show_more && var.has_children) {
        Disclose(painted, row_x, var.expanded);
      }
      Run(painted, name_x, name_avail, color, var.display_name);
      return;
    }
    if (var.has_children) {
      Disclose(painted, row_x, var.expanded);
    }
    const float name_w = text_.MeasureWidth(var.display_name);
    const bool editing =
        model.IsEditing() && model.EditingNodeId().has_value() && *model.EditingNodeId() == var.node_id;
    Run(painted, name_x, name_avail, theme_.text_primary, var.display_name);
    if (editing) {
      Edit(painted, model.EditBuffer(), name_x + std::min(name_w, name_avail) + 12.0f);
      return;
    }
    float values_right = content_right;
    if (!var.display_type.empty()) {
      const float type_w = text_.MeasureWidth(var.display_type);
      const float type_x = content_right - type_w;
      constexpr float kMinValueRoom = 48.0f;
      if (type_x > name_x + name_w + 12.0f + kMinValueRoom) {
        Run(painted, type_x, type_w, theme_.text_muted, var.display_type);
        values_right = type_x - 8.0f;
      }
    }
    if (var.display_value.empty()) {
      return;
    }
    const float value_x =
        std::max(in_.text_x + std::max(120.0f, in_.text_width * 0.40f), name_x + name_w + 12.0f);
    if (const float value_w = values_right - value_x; value_w > 0.0f) {
      Run(painted, value_x, value_w, ValueKindColor(theme_, var.kind), var.display_value);
    }
  }

  void BreakpointRow(const DebugBreakpointsModel& model, std::size_t index, int row, int selected_row) {
    if (index >= model.Rows().size()) {
      return;
    }
    const DebugBreakpointRowView& bp = model.Rows()[index];
    if (bp.kind == DebugBreakpointRowView::Kind::Header) {
      Run(Row(row, false, false), in_.text_x, in_.text_width, theme_.text_muted, bp.display);
      return;
    }
    PreparedFrameState::DebugPaneRowPaint& painted = Row(row, selected_row == static_cast<int>(index));
    const std::string_view checkbox = bp.enabled ? "[x] " : "[ ] ";
    const float box_w = text_.MeasureWidth(checkbox);
    if (bp.kind == DebugBreakpointRowView::Kind::ExceptionFilter) {
      const SDL_Color color = bp.enabled ? theme_.text_primary : theme_.text_secondary;
      Run(painted, in_.text_x, box_w, color, checkbox);
      Run(painted, in_.text_x + box_w, in_.text_width - box_w, color, bp.display);
      return;
    }
    const float x = in_.text_x + kDebugPaneBreakpointIndent;
    const SDL_Color primary = bp.failed    ? theme_.diagnostic_warning
                              : bp.enabled ? theme_.text_secondary
                                           : theme_.text_disabled;
    Run(painted, x, box_w, primary, checkbox);
    TwoColumns(painted, x + box_w, in_.text_width - kDebugPaneBreakpointIndent - box_w, bp.display, primary,
               bp.secondary, theme_.text_muted);
  }

 private:
  void Disclose(PreparedFrameState::DebugPaneRowPaint& row, float x, bool expanded) {
    row.disclosure = true;
    row.expanded = expanded;
    row.disclosure_x = x;
  }
  void Edit(PreparedFrameState::DebugPaneRowPaint& row, const editor::SingleLineEditor& buffer, float value_x) {
    const float value_w = in_.text_x + in_.text_width - value_x;
    if (value_w <= 4.0f) {
      return;
    }
    row.editing = true;
    PreparedFrameState::DebugEditPaint& edit = body_.edit;
    edit.field = MakeRect(value_x - 2.0f, row.band.y, value_w, row.band.h);
    edit.text_x = value_x + 2.0f;
    const SingleLineViewMetrics metrics =
        ComputeSingleLineViewMetrics(text_, buffer, "", std::max(1.0f, value_w - 8.0f));
    edit.text.assign(metrics.displayed_text);
    edit.selection = SDL_FRect{};
    if (metrics.selection_bytes.has_value()) {
      const std::string_view shown = edit.text;
      const auto [begin, end] = *metrics.selection_bytes;
      const float sel_x = edit.text_x + text_.MeasureWidth(shown.substr(0, begin));
      const float sel_w = text_.MeasureWidth(shown.substr(begin, end - begin));
      if (sel_w > 0.0f) {
        edit.selection = MakeRect(sel_x, row.band.y, sel_w, row.band.h);
      }
    }
    edit.caret = in_.edit_caret_shown
                     ? MakeRect(edit.text_x + metrics.cursor_x, row.band.y, 1.5f, std::min(text_.LineHeight(), row.band.h))
                     : SDL_FRect{};
  }

  PreparedFrameState::DebugPaneBodyPaint& body_;
  const DebugPaneInputs& in_;
  const render::Theme& theme_;
  const render::TextRenderer& text_;
};

void DrawModeGlyph(SDL_Renderer* renderer, DebugPaneMode mode, const SDL_FRect& icon, SDL_Color color) {
  const float cx = icon.x + icon.w * 0.5f;
  const float cy = icon.y + icon.h * 0.5f;
  switch (mode) {
    case DebugPaneMode::CallStack: {
      const float bar_w = std::min(10.0f, icon.w - 4.0f);
      for (int i = -1; i <= 1; ++i) {
        FillRect(renderer, MakeRect(cx - bar_w * 0.5f, cy + static_cast<float>(i) * 3.0f - 0.5f, bar_w, 1.5f),
                 color);
      }
      break;
    }
    case DebugPaneMode::Variables: {
      const float bar_w = std::min(9.0f, icon.w - 4.0f);
      FillRect(renderer, MakeRect(cx - bar_w * 0.5f, cy - 2.5f, bar_w, 1.5f), color);
      FillRect(renderer, MakeRect(cx - bar_w * 0.5f, cy + 1.0f, bar_w, 1.5f), color);
      break;
    }
    case DebugPaneMode::Watch:
      DrawSearchGlyph(renderer, icon, color);
      break;
    case DebugPaneMode::Breakpoints:
      DrawGlyphDot(renderer, cx, cy, color);
      break;
  }
}

void DrawDisclosure(SDL_Renderer* renderer, float x, float band_y, float line_height, bool expanded,
                    SDL_Color color) {
  constexpr int kSize = 7;
  const float cy = band_y + line_height * 0.5f;
  for (int i = 0; i < kSize; ++i) {
    const float extent = static_cast<float>(kSize - i * 2);
    if (extent <= 0.0f) {
      break;
    }
    FillRect(renderer,
             expanded ? MakeRect(x + static_cast<float>(i), cy - 2.0f + static_cast<float>(i), extent, 1.0f)
                      : MakeRect(x + static_cast<float>(i), cy - extent * 0.5f, 1.0f, extent),
             color);
  }
}

}

void PrepareDebugPaneFrame(PreparedFrameState& frame, const DebugPaneInputs& inputs) {
  PreparedFrameState::DebugPaneBodyPaint& body = frame.debug_pane;
  body.visible = false;
  body.tabs.clear();
  body.rows.clear();
  body.hint = TerminalHintPaint{};
  body.scrollbar.reset();
  std::vector<DerivedDamage::Unit>& units = frame.derived_damage.Begin(DerivedSurface::DebugPane);
  if (inputs.visible) {
    const DebugPaneSurfaceViewModel& pane = *inputs.pane;
    const render::Theme& theme = *inputs.theme;
    body.visible = true;
    body.line_height = inputs.line_height;
    const std::uint64_t style = PaintFingerprint()
                                    .Add(theme.surface_background)
                                    .Add(theme.row_highlight)
                                    .Add(theme.text_muted)
                                    .Add(theme.surface_raised)
                                    .Add(theme.accent)
                                    .Add(theme.selection_fill)
                                    .Add(theme.cursor)
                                    .Add(theme.border)
                                    .Add(inputs.text_renderer->MetricsGeneration())
                                    .value();
    const auto unit = [&](const SDL_FRect& rect, std::uint64_t fingerprint) {
      units.push_back(
          DerivedDamage::Unit{.rect = render::SnapToDevicePixelsF(inputs.renderer, rect), .fingerprint = fingerprint});
    };
    for (int i = 0; i < inputs.mode_row.tab_count; ++i) {
      const DebugPaneModeTab& tab = inputs.mode_row.tabs[static_cast<std::size_t>(i)];
      const DebugModeTabPaint& painted = body.tabs.emplace_back(DebugModeTabPaint{
          .rect = tab.rect,
          .mode = tab.mode,
          .label = inputs.mode_row.icon_only ? std::string_view{} : tab.label,
          .hovered = inputs.pointer_over(tab.rect),
          .active = tab.mode == pane.mode,
      });
      unit(painted.rect, FingerprintOf(painted, style));
    }
    DebugPaneRowLayout layout(body, inputs);
    for (int row = 0; row < inputs.visible_rows; ++row) {
      const int index = inputs.first_row + row;
      if (index < 0 || static_cast<std::size_t>(index) >= inputs.line_count) {
        break;
      }
      const auto entry = static_cast<std::size_t>(index);
      if (pane.execution != nullptr) {
        layout.CallStackRow(*pane.execution, entry, row);
      } else if (pane.variables != nullptr) {
        layout.ValueTreeRow(*pane.variables, entry, row);
      } else if (pane.watch != nullptr) {
        layout.ValueTreeRow(*pane.watch, entry, row);
      } else if (pane.breakpoints != nullptr) {
        layout.BreakpointRow(*pane.breakpoints, entry, row, pane.breakpoints_selected_row);
      }
    }
    for (const PreparedFrameState::DebugPaneRowPaint& row : body.rows) {
      unit(row.band, FingerprintOf(row, body.edit, style));
    }
    std::string_view hint;
    if (pane.watch != nullptr && pane.watch->Rows().empty()) {
      hint = "No watch expressions — click or press Insert to add one.";
    } else if (pane.breakpoints != nullptr && pane.breakpoints->RowCount() == 0) {
      hint = "No breakpoints — click the editor gutter to add one.";
    } else if (pane.variables != nullptr && inputs.line_count == 0) {
      hint = "No variables — start a debug session and pause to inspect scope.";
    } else if (pane.execution != nullptr && inputs.line_count == 0) {
      hint = "Not paused — start a debug session and hit a breakpoint.";
    }
    if (!hint.empty()) {
      body.hint = TerminalHintPaint{
          .rect = MakeRect(inputs.text_x, inputs.text_y, inputs.text_width,
                           std::max(0.0f, inputs.content.y + inputs.content.h - inputs.text_y)),
          .text = hint};
    }
    unit(body.hint.rect, FingerprintOf(body.hint, style));
    if (inputs.scrollbar.has_value()) {
      body.scrollbar = PanelScrollbarPaint{
          .track = inputs.scrollbar->track, .thumb = inputs.scrollbar->thumb, .active = inputs.scrollbar_active};
      unit(body.scrollbar->track, FingerprintOf(*body.scrollbar, style));
    } else {
      unit(SDL_FRect{}, 0);
    }
  }
  frame.derived_damage.Commit(DerivedSurface::DebugPane, inputs.damage);
}

void PaintDebugPaneBody(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                        const render::TextRenderer& text_renderer) {
  const PreparedFrameState::DebugPaneBodyPaint& body = frame.debug_pane;
  if (!body.visible) {
    return;
  }
  for (const DebugModeTabPaint& tab : body.tabs) {
    const render::PaintElement element(renderer, tab.rect, "debug-mode-tab");
    DrawModeTab(text_renderer, renderer, theme, tab.rect, tab.hovered, tab.active, tab.label,
                [&](const SDL_FRect& icon, SDL_Color color) { DrawModeGlyph(renderer, tab.mode, icon, color); });
  }
  for (const PreparedFrameState::DebugPaneRowPaint& row : body.rows) {
    const render::PaintElement element(renderer, row.band, "debug-row");
    const SDL_Color background = row.highlighted ? theme.row_highlight : theme.surface_background;
    if (row.highlighted) {
      FillRect(renderer, row.band, background);
    }
    if (row.disclosure) {
      DrawDisclosure(renderer, row.disclosure_x, row.text_y, body.line_height, row.expanded, theme.text_muted);
    }
    for (const DebugTextRun& run : row.runs) {
      DrawTextOn(text_renderer, renderer, run.x, row.text_y, run.color, background,
                 text_renderer.TruncateToWidthEphemeralView(run.text, run.width));
    }
    if (row.editing) {
      const PreparedFrameState::DebugEditPaint& edit = body.edit;
      DrawTextFieldFrame(renderer, theme, edit.field, true);
      if (edit.selection.w > 0.0f) {
        FillRect(renderer, edit.selection, theme.selection_fill);
      }
      text_renderer.DrawString(renderer, edit.text_x, row.text_y, theme.text_primary, edit.text);
      if (edit.caret.w > 0.0f) {
        FillRect(renderer, edit.caret, theme.cursor);
      }
    }
  }
  if (!body.hint.text.empty()) {
    const render::PaintElement element(renderer, body.hint.rect, "debug-hint");
    DrawWrappedPlaceholderIn(text_renderer, renderer, body.hint.rect, theme.text_muted, theme.surface_background,
                             body.hint.text);
  }
  if (body.scrollbar.has_value()) {
    const render::PaintElement element(renderer, body.scrollbar->track, "debug-scrollbar");
    DrawScrollbar(renderer, theme, body.scrollbar->track, body.scrollbar->thumb, body.scrollbar->active);
  }
}

void WorkspaceShell::RenderDebugPaneSurface(SDL_Renderer* renderer,
                                            const WorkspaceLayout& layout) {
  if (!prepare_cached_debug_pane_vm_.has_value()) {
    return;
  }
  const DebugPaneSurfaceViewModel& pane_vm = *prepare_cached_debug_pane_vm_;
  if (!pane_vm.visible || layout.right_pane.w <= 0.0f || layout.right_pane.h <= 0.0f) {
    return;
  }

  util::PerformanceTrace::Scope pane_scope("WorkspaceShell::RenderDebugPane");

  DrawFilledRect(renderer, layout.right_pane, theme_.surface_background);

  PaintDebugPaneBody(renderer, frame_state_, theme_, text_renderer_);

  if (pane_vm.focus == FocusTarget::DebugPane) {
    DrawSurfaceFocusRing(renderer, layout.right_pane);
  }
}

}
