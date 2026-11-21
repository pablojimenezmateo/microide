#include "workspace/render/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <limits>
#include <variant>

#include "editor/DecoratedTextGridRenderer.h"
#include "editor/PluginDecorationStore.h"
#include "editor/PluginSurfaceStore.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "render/PluginDisplayList.h"
#include "render/PaintElement.h"
#include "render/PluginDisplayListRenderer.h"
#include "render/ScopedRenderClip.h"
#include "render/SdlConvert.h"
#include "render/SurfaceTextureCache.h"
#include "util/PerformanceTrace.h"
#include "workspace/PluginSurfacePreview.h"
#include "workspace/WorkspaceTextSearch.h"
#include "workspace/render/BottomPanelHeaderPaint.h"
#include "workspace/render/BottomPanelOutputPaint.h"
#include "workspace/render/TerminalRowPaint.h"

namespace microide::workspace {

using namespace detail;

namespace {

// Paint a decoded raster surface (scaled to fit, clipped to `rect`), or a muted
// "Rendering…" placeholder while its decode is still in flight. Split out of
// RenderPluginSurfaceInto so the display-list and raster paths stay independently
// readable; kept a file-local helper so it adds no member to the size-capped shell.
void RenderRasterSurfaceInto(SDL_Renderer* renderer, const SDL_FRect& rect,
                             const editor::RasterHandle& raster, float scroll_y,
                             render::SurfaceTextureCache& cache, render::TextRenderer& text_renderer,
                             const render::Theme& theme) {
  constexpr float kPad = kPluginSurfacePreviewPadding;
  const render::SurfaceTextureCache::Entry* entry = cache.Lookup(raster.content_hash);
  if (entry == nullptr || entry->texture == nullptr) {
    text_renderer.DrawString(renderer, rect.x + kPad, rect.y + kPad, theme.text_muted, "Rendering…");
    return;
  }
  const float avail_w = std::max(1.0f, rect.w - kPad * 2.0f);
  const float scale =
      entry->width > 0 ? std::min(1.0f, avail_w / static_cast<float>(entry->width)) : 1.0f;
  const SDL_FRect dest{rect.x + kPad, rect.y + kPad - scroll_y,
                       static_cast<float>(entry->width) * scale,
                       static_cast<float>(entry->height) * scale};
  const SDL_Rect clip{static_cast<int>(std::floor(rect.x)), static_cast<int>(std::floor(rect.y)),
                      static_cast<int>(std::ceil(rect.w)), static_cast<int>(std::ceil(rect.h))};
  const render::ScopedRenderClip clip_scope(renderer, clip);
  SDL_RenderTexture(renderer, entry->texture, nullptr, &dest);
}

// The panel strip's tab colours: one definition for prep's fingerprint and paint.
StripTabPalette PanelTabPalette(const render::Theme& theme) {
  return StripTabPalette{
      .active_fill = render::BlendColors(theme.chrome_active, theme.surface_background, 0.42f),
      .inactive_fill = render::BlendColors(theme.surface_raised, theme.surface_background, 0.66f),
      .hover_fill = theme.row_highlight,
      .active_text = theme.chrome_active_text,
      .inactive_text = theme.text_secondary,
      .active_glyph = theme.chrome_active_text,
      .inactive_glyph = theme.text_secondary,
  };
}

constexpr StripTabStyle kPanelTabStyle{
    .text_left_padding = 8.0f,
    .close_right_reserve = 40.0f,
    .accent_edge = StripAccentEdge::Top,
};

// A unit's rect: the element's, snapped as its PaintElement clips (clip-discipline D2).
SDL_FRect UnitRect(SDL_Renderer* renderer, const SDL_FRect& rect) {
  const SDL_Rect snapped = render::SnapToDevicePixels(renderer, rect);
  return SDL_FRect{static_cast<float>(snapped.x), static_cast<float>(snapped.y), static_cast<float>(snapped.w),
                   static_cast<float>(snapped.h)};
}

}  // namespace

void PrepareBottomPanelHeaderFrame(PreparedFrameState& frame, const BottomPanelHeaderInputs& inputs) {
  PreparedFrameState::PanelHeaderPaint& header = frame.panel_header;
  header.tabs.clear();
  header.visible = false;
  header.drag_tabs = nullptr;
  header.dragging = false;
  std::vector<DerivedDamage::Unit>& units = frame.derived_damage.Begin(DerivedSurface::BottomPanelHeader);
  const BottomPanelSurfaceViewModel& panel = *inputs.panel;
  const render::Theme& theme = *inputs.theme;
  if (panel.content != PanelContentKind::None && inputs.layout->bottom_panel.h > 0.0f) {
    header.visible = true;
    header.header = MakeRect(inputs.layout->bottom_panel.x, inputs.layout->bottom_panel.y,
                             inputs.layout->bottom_panel.w, kWorkspaceBottomPanelHeaderHeight);
    const StripTabPalette palette = PanelTabPalette(theme);
    const std::uint64_t palette_key = PaintFingerprint()
                                          .Add(palette.active_fill)
                                          .Add(palette.inactive_fill)
                                          .Add(palette.hover_fill)
                                          .Add(palette.active_text)
                                          .Add(palette.inactive_text)
                                          .Add(palette.active_glyph)
                                          .Add(palette.inactive_glyph)
                                          .Add(theme.accent)
                                          .Add(theme.diagnostic_warning)
                                          .Add(theme.chrome_text)
                                          .Add(theme.chrome_text_secondary)
                                          .Add(theme.text_primary)
                                          .Add(theme.text_secondary)
                                          .Add(theme.row_highlight)
                                          .Add(theme.surface_raised)
                                          .Add(theme.border)
                                          .value();
    // No hover while the strip slides (soak 38f44bf79ca8): the tabs are drawn at
    // their slid rects, which overlap the resting ones the hover probe tracks.
    const bool still = !panel.tab_drag.active && !panel.tab_drag.sliding;
    const std::vector<VisibleStripTab>& tabs =
        panel.tabs != nullptr ? *panel.tabs : TabStripService::EmptyVisibleTabs();
    for (const VisibleStripTab& tab : tabs) {
      if (panel.tab_drag.active && tab.index == panel.tab_drag.source_index) {
        continue;  // lifted; drawn as the floating ghost
      }
      const float dx = panel.tab_drag.sliding && tab.index < panel.tab_drag.offsets.size()
                           ? panel.tab_drag.offsets[tab.index]
                           : 0.0f;
      SDL_FRect rect = tab.rect;
      SDL_FRect close_rect = tab.close_rect;
      rect.x += dx;
      close_rect.x += dx;
      const PreparedFrameState::PanelTabPaint& painted = header.tabs.emplace_back(PreparedFrameState::PanelTabPaint{
          .rect = rect,
          .close_rect = close_rect,
          .title = tab.display_title,
          .badge_text = tab.badge_text,
          .badge_color = tab.badge_color,
          .secondary_title = tab.secondary_title,
          .remote_state = tab.remote_state,
          .show_badge = tab.show_badge,
          .active = tab.active,
          .hovered = still && inputs.pointer_over(rect),
          .close_hovered = still && inputs.pointer_over(close_rect),
          .dimmed = tab.dimmed,
          .activity_dot = tab.activity_dot,
          .bell_mark = tab.bell_mark,
      });
      units.push_back(DerivedDamage::Unit{.rect = UnitRect(inputs.renderer, rect),
                                          .fingerprint = FingerprintOf(painted, palette_key)});
    }
    header.label = tabs.empty() ? inputs.label : std::string_view{};
    // The label, when it shows: the header's left part up to the buttons.
    units.push_back(DerivedDamage::Unit{
        .rect = UnitRect(inputs.renderer, header.header),
        .fingerprint = PaintFingerprint().Add(header.header).Add(header.label).Add(palette_key).value()});
    const auto button = [&](PreparedFrameState::PanelButtonPaint& slot, const SDL_FRect& rect, std::size_t hidden) {
      slot = PreparedFrameState::PanelButtonPaint{.rect = rect, .hovered = inputs.pointer_over(rect), .hidden = hidden};
      units.push_back(DerivedDamage::Unit{.rect = UnitRect(inputs.renderer, rect),
                                          .fingerprint = FingerprintOf(slot, palette_key)});
    };
    button(header.overflow_left, panel.tab_overflow.left_button, panel.tab_overflow.hidden_left);
    button(header.overflow_right, panel.tab_overflow.right_button, panel.tab_overflow.hidden_right);
    button(header.new_terminal, inputs.new_terminal_rect, 0);
    header.maximized = inputs.layout->panel_maximized;
    button(header.maximize, inputs.maximize_rect, header.maximized ? 1 : 0);
    if (panel.tab_drag.active) {
      header.drag_tabs = &tabs;
      header.dragging = true;
      header.drag_source = panel.tab_drag.source_index;
      header.drag_pointer_x = panel.tab_drag.pointer_x;
      header.drag_grab_offset_x = panel.tab_drag.grab_offset_x;
    }
    // The drag ghost follows the pointer anywhere over the strip.
    units.push_back(DerivedDamage::Unit{.rect = UnitRect(inputs.renderer, header.header),
                                        .fingerprint = PaintFingerprint()
                                                           .Add(header.dragging)
                                                           .Add(header.drag_source)
                                                           .Add(header.drag_pointer_x)
                                                           .Add(header.drag_grab_offset_x)
                                                           .value()});
  }
  frame.derived_damage.Commit(DerivedSurface::BottomPanelHeader, inputs.damage);
}

void PaintBottomPanelHeader(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                            const render::TextRenderer& text_renderer) {
  const PreparedFrameState::PanelHeaderPaint& header = frame.panel_header;
  if (!header.visible) {
    return;
  }
  const StripTabPalette palette = PanelTabPalette(theme);
  for (const PreparedFrameState::PanelTabPaint& tab : header.tabs) {
    const render::PaintElement element(renderer, tab.rect, "panel-tab");
    DrawStripTab(text_renderer, renderer, theme, tab.rect, tab.title, tab.badge_text, tab.badge_color,
                 tab.show_badge, tab.active, kPanelTabStyle, palette, tab.hovered,
                 StripTabRemoteMark{
                     .secondary_label = tab.secondary_title, .state = tab.remote_state, .dimmed = tab.dimmed});
    DrawHoverableCloseGlyph(renderer, tab.close_rect, tab.close_hovered,
                            tab.active ? palette.active_glyph : palette.inactive_glyph, palette.active_text);
    if (tab.activity_dot || tab.bell_mark) {
      // Unseen output in a background terminal: a small accent square just left of
      // the close glyph, where the eye already goes for tab state. A bell is the
      // same place, larger and in the warning colour.
      const float dot = tab.bell_mark ? 8.0f : 6.0f;
      FillRect(renderer,
               MakeRect(tab.close_rect.x - dot - 6.0f, std::floor(tab.rect.y + (tab.rect.h - dot) * 0.5f), dot,
                        dot),
               tab.bell_mark ? theme.diagnostic_warning : theme.accent);
    }
  }
  if (!header.label.empty()) {
    const render::PaintElement element(renderer, header.header, "panel-label");
    DrawVCenteredTextOn(text_renderer, renderer, header.header, 12.0f, theme.chrome_text, theme.chrome_background,
                        header.label);
  }
  DrawTabStripOverflowButton(text_renderer, renderer, theme, header.overflow_left.rect, /*point_right=*/false,
                             header.overflow_left.hidden, header.overflow_left.hovered);
  DrawTabStripOverflowButton(text_renderer, renderer, theme, header.overflow_right.rect, /*point_right=*/true,
                             header.overflow_right.hidden, header.overflow_right.hovered);
  if (header.dragging && header.drag_tabs != nullptr) {
    DrawTabDragFeedback(text_renderer, renderer, theme, header.header, *header.drag_tabs, header.header,
                        header.drag_source, header.drag_pointer_x, header.drag_grab_offset_x, kPanelTabStyle,
                        palette);
  }
  // New terminal, for every content kind: a click there opens one whether or not
  // a tab exists, and the empty panel's own hint points at it.
  {
    const PreparedFrameState::PanelButtonPaint& button = header.new_terminal;
    const render::PaintElement element(renderer, button.rect, "panel-new-terminal");
    DrawButtonCentered(text_renderer, renderer, theme, button.rect, "", ButtonTone::Neutral,
                       ButtonVisualState{.enabled = true, .hovered = button.hovered, .active = false});
    DrawPlusGlyph(renderer, button.rect, button.hovered ? theme.text_primary : theme.text_secondary);
  }
  // Maximize / restore at the header's right edge: an up chevron offers the
  // immersive panel, a down chevron the way back (VS Code's pair).
  {
    const PreparedFrameState::PanelButtonPaint& button = header.maximize;
    const render::PaintElement element(renderer, button.rect, "panel-maximize");
    DrawButtonCentered(text_renderer, renderer, theme, button.rect, "", ButtonTone::Neutral,
                       ButtonVisualState{.enabled = true, .hovered = button.hovered, .active = false});
    const SDL_Color glyph = button.hovered ? theme.text_primary : theme.text_secondary;
    const float cx = std::floor(button.rect.x + button.rect.w * 0.5f);
    const float cy = std::floor(button.rect.y + button.rect.h * 0.5f);
    const float arm = 3.0f;
    render::SetDrawColor(renderer, glyph);
    if (header.maximized) {
      render::RenderClipStableLine(renderer, cx - arm, cy - 1.0f, cx, cy + arm - 1.0f);
      render::RenderClipStableLine(renderer, cx, cy + arm - 1.0f, cx + arm, cy - 1.0f);
    } else {
      render::RenderClipStableLine(renderer, cx - arm, cy + 1.0f, cx, cy - arm + 1.0f);
      render::RenderClipStableLine(renderer, cx, cy - arm + 1.0f, cx + arm, cy + 1.0f);
    }
  }
}

void PrepareBottomPanelOutputFrame(PreparedFrameState& frame, const BottomPanelOutputInputs& inputs) {
  PreparedFrameState::OutputBodyPaint& body = frame.output_body;
  body.visible = false;
  body.rows.clear();
  body.hint = {};
  body.scrollbar.reset();
  std::vector<DerivedDamage::Unit>& units = frame.derived_damage.Begin(DerivedSurface::BottomPanelOutput);
  const std::vector<std::string>* entries =
      inputs.visible ? inputs.channels->Entries(inputs.channel_id) : nullptr;
  if (inputs.visible) {
    body.visible = true;
    const render::Theme& theme = *inputs.theme;
    PaintFingerprint style;
    style.Add(theme.text_secondary).Add(theme.text_muted).Add(theme.surface_background).Add(inputs.text_metrics);
    for (int kind = 0; kind <= static_cast<int>(editor::SyntaxTokenKind::Operator); ++kind) {
      style.Add(editor::SyntaxTokenColor(theme, static_cast<editor::SyntaxTokenKind>(kind), theme.text_secondary));
    }
    const std::uint64_t row_style = style.value();
    const std::size_t count = entries != nullptr ? entries->size() : 0;
    const std::uint64_t first_serial = inputs.channels->FirstEntrySerial(inputs.channel_id);
    for (int row = 0; row < inputs.visible_rows; ++row) {
      const int index = inputs.first_row + row;
      if (index < 0 || static_cast<std::size_t>(index) >= count) {
        break;
      }
      const auto entry = static_cast<std::size_t>(index);
      PreparedFrameState::OutputRowPaint& painted = body.rows.emplace_back(PreparedFrameState::OutputRowPaint{
          .rect = MakeRect(inputs.content.x, inputs.text_y + static_cast<float>(row) * inputs.line_height,
                           inputs.content.w, inputs.line_height),
          .text_x = inputs.text_x,
          .text_width = inputs.text_width,
          .serial = first_serial + entry,
          .text = (*entries)[entry],
      });
      const auto* parsed = inputs.channels->ParsedEntryAt(inputs.channel_id, entry);
      if (parsed != nullptr && parsed->kind == WorkspaceOutputChannels::ParsedEntry::Kind::ContextSnippet) {
        // Highlighted for the path the snippet resolves to: the channel memoizes
        // both per entry, so a steady panel re-resolves nothing (TD-2026-07-17A-017).
        static const std::filesystem::path kNoRoot;
        const std::filesystem::path& resolved = inputs.channels->ResolvedReferencePath(
            inputs.channel_id, entry, inputs.project_root != nullptr ? *inputs.project_root : kNoRoot);
        painted.snippet = true;
        painted.prefix = parsed->prefix;
        painted.code = parsed->code;
        painted.highlighted = inputs.channels->HighlightedContextSnippet(inputs.channel_id, entry, resolved);
        painted.highlight_path = std::hash<std::string_view>{}(resolved.native()) | 1u;
      }
      units.push_back(DerivedDamage::Unit{.rect = UnitRect(inputs.renderer, painted.rect),
                                          .fingerprint = FingerprintOf(painted, row_style)});
    }
    // The empty channel's hint, word-wrapped because the panel narrows with the
    // sidebar and the right pane: up to DrawWrappedPlaceholder's three lines.
    if (count == 0 && !inputs.empty_label.empty()) {
      body.hint = inputs.empty_label;
      const float bottom = inputs.content.y + inputs.content.h;
      body.hint_rect = MakeRect(inputs.text_x, inputs.text_y, inputs.text_width,
                                std::clamp(bottom - inputs.text_y, 0.0f, 3.0f * inputs.line_height));
    }
    units.push_back(DerivedDamage::Unit{
        .rect = UnitRect(inputs.renderer, body.hint_rect),
        .fingerprint = PaintFingerprint().Add(body.hint_rect).Add(body.hint).Add(row_style).value()});
    if (inputs.scrollbar.has_value()) {
      body.scrollbar = PreparedFrameState::OutputScrollbarPaint{
          .track = inputs.scrollbar->track, .thumb = inputs.scrollbar->thumb, .active = inputs.scrollbar_active};
      const std::uint64_t bar_style =
          PaintFingerprint().Add(theme.surface_raised).Add(theme.text_muted).Add(theme.accent).value();
      units.push_back(DerivedDamage::Unit{.rect = UnitRect(inputs.renderer, body.scrollbar->track),
                                          .fingerprint = FingerprintOf(*body.scrollbar, bar_style)});
    }
  }
  frame.derived_damage.Commit(DerivedSurface::BottomPanelOutput, inputs.damage);
}

void PaintBottomPanelOutputBody(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                                const render::TextRenderer& text_renderer) {
  const PreparedFrameState::OutputBodyPaint& body = frame.output_body;
  if (!body.visible) {
    return;
  }
  for (const PreparedFrameState::OutputRowPaint& row : body.rows) {
    const render::PaintElement element(renderer, row.rect, "output-row");
    const float y = row.rect.y;
    if (!row.snippet) {
      DrawTextOn(text_renderer, renderer, row.text_x, y, theme.text_secondary, theme.surface_background,
                 text_renderer.TruncateToWidthEphemeralView(row.text, row.text_width));
      continue;
    }
    // A context snippet: its `path:line:` prefix, then the code coloured as the
    // file it quotes, when the whole prefix fits.
    const std::string_view visible_prefix = text_renderer.TruncateToWidthEphemeralView(row.prefix, row.text_width);
    DrawTextOn(text_renderer, renderer, row.text_x, y, theme.text_secondary, theme.surface_background,
               visible_prefix);
    if (visible_prefix.size() != row.prefix.size()) {
      continue;
    }
    const float prefix_width = text_renderer.MeasureWidth(row.prefix);
    const float remaining_width = row.text_width - prefix_width;
    if (remaining_width <= 0.0f || row.code.empty()) {
      continue;
    }
    const std::string_view visible_code = text_renderer.TruncateToWidthEphemeralView(row.code, remaining_width);
    if (row.highlighted == nullptr || row.highlighted->tokens.size() < visible_code.size()) {
      DrawTextOn(text_renderer, renderer, row.text_x + prefix_width, y, theme.text_secondary,
                 theme.surface_background, visible_code);
      continue;
    }
    float run_x = row.text_x + prefix_width;
    for (std::size_t start = 0; start < visible_code.size();) {
      const editor::SyntaxTokenKind kind = row.highlighted->tokens[start];
      std::size_t end = start + 1;
      while (end < visible_code.size() && row.highlighted->tokens[end] == kind) {
        ++end;
      }
      const std::string_view segment(visible_code.data() + start, end - start);
      text_renderer.DrawString(renderer, run_x, y,
                               editor::SyntaxTokenColor(theme, kind, theme.text_secondary), segment);
      run_x += text_renderer.MeasureWidth(segment);
      start = end;
    }
  }
  if (!body.hint.empty()) {
    const render::PaintElement element(renderer, body.hint_rect, "output-hint");
    DrawWrappedPlaceholder(text_renderer, renderer, body.hint_rect.x, body.hint_rect.y, body.hint_rect.w,
                           theme.text_muted, theme.surface_background, body.hint);
  }
  if (body.scrollbar.has_value()) {
    const render::PaintElement element(renderer, body.scrollbar->track, "output-scrollbar");
    DrawScrollbar(renderer, theme, body.scrollbar->track, body.scrollbar->thumb, body.scrollbar->active);
  }
}

void WorkspaceShell::RenderBottomPanelSurface(SDL_Renderer* renderer,
                                              const WorkspaceLayout& layout) {
  const BottomPanelSurfaceViewModel& panel_vm = *prepare_cached_bottom_panel_vm_;
  if (panel_vm.content == PanelContentKind::None) {
    return;
  }

  util::PerformanceTrace::Scope bottom_panel_scope("WorkspaceShell::RenderBottomPanel");
  const bool terminal_panel = panel_vm.content == PanelContentKind::Terminal;

  // Each pane sets its DECSCNM flag and selection strength before its rows paint;
  // TerminalRowPaint resolves every cell against this once per row.
  TerminalPaintPalette terminal_palette{
      .foreground = theme_.text_primary,
      .background = theme_.surface_background,
      .selection = theme_.selection_fill,
      .find_match = theme_.search_match,
      .find_current = theme_.search_match_active,
  };
  const auto draw_terminal_line = [&](float x,
                                      float y,
                                      float width,
                                      const terminal::TerminalLine& line,
                                      const TerminalRowEmphasis& emphasis,
                                      std::span<const terminal::TerminalUnderlineColor> underline_colors) {
    const float char_width = std::max(1.0f, terminal_text_renderer_.CharWidth());
    if (width < char_width) {
      return;
    }
    const auto columns = static_cast<std::size_t>(std::floor(width / char_width));
    PlanTerminalRow(line, columns, emphasis, terminal_palette, terminal_row_plan_, underline_colors);
    const float line_height = terminal_text_renderer_.LineHeight();
    const auto column_x = [&](std::size_t column) { return x + static_cast<float>(column) * char_width; };
    const auto span_w = [&](std::size_t begin, std::size_t end) {
      return static_cast<float>(end - begin) * char_width;
    };
    for (const TerminalRowPaintPlan::Fill& fill : terminal_row_plan_.fills) {
      DrawFilledRect(renderer, MakeRect(column_x(fill.begin), y - 1.0f, span_w(fill.begin, fill.end), line_height),
                     fill.color);
    }
    for (const TerminalRowPaintPlan::TextRun& run : terminal_row_plan_.text_runs) {
      // SGR 1 / 3 in the family's bold / italic face (TextRenderer::Face). A face
      // whose advance is not the grid's -- a synthesized bold is wider -- would
      // drift across a run, so its glyphs are placed one cell at a time.
      const render::TextRenderer& face = terminal_text_renderer_.Face(run.face);
      if (run.face == render::FontStyle::Regular || std::fabs(face.CharWidth() - char_width) < 0.01f) {
        face.DrawString(renderer, column_x(run.column), y, run.color, terminal_row_plan_.RunText(run));
        continue;
      }
      for (std::size_t column = run.column; column < run.end && column < line.cells.size(); ++column) {
        const std::string_view glyph = line.cells[column].DisplayText();
        if (!glyph.empty() && glyph != " ") {
          face.DrawString(renderer, column_x(column), y, run.color, glyph);
        }
      }
    }
    const float thickness = std::max(1.0f, std::round(line_height / 14.0f));
    for (const TerminalRowPaintPlan::Decoration& stroke : terminal_row_plan_.decorations) {
      const float run_x = column_x(stroke.begin);
      const float run_w = span_w(stroke.begin, stroke.end);
      if (stroke.bits & terminal::cell_attr::kStrikethrough) {
        DrawFilledRect(renderer, MakeRect(run_x, y + std::round(line_height * 0.45f), run_w, thickness),
                       stroke.color);
      }
      const float underline_y = y + line_height - thickness - 1.0f;
      if (stroke.bits & terminal::cell_attr::kUnderline) {
        switch (stroke.variant) {
          case terminal::TerminalUnderlineVariant::Straight:
            DrawFilledRect(renderer, MakeRect(run_x, underline_y, run_w, thickness), stroke.underline_color);
            break;
          case terminal::TerminalUnderlineVariant::Curly: {
            // A zig-zag two strokes tall, its period a few strokes wide, as the
            // editor's own squiggles read at this size.
            const float amplitude = thickness * 1.5f;
            const float half = std::max(2.0f, thickness * 2.0f);
            const float top = underline_y - amplitude + thickness;
            SDL_SetRenderDrawColor(renderer, stroke.underline_color.r, stroke.underline_color.g,
                                   stroke.underline_color.b, stroke.underline_color.a);
            bool up = true;
            for (float px = run_x; px < run_x + run_w; px += half, up = !up) {
              const float next = std::min(px + half, run_x + run_w);
              for (float row = 0.0f; row < thickness; row += 1.0f) {
                render::RenderClipStableLine(renderer, px, (up ? top + amplitude : top) + row, next,
                               (up ? top : top + amplitude) + row);
              }
            }
            break;
          }
          case terminal::TerminalUnderlineVariant::Dotted:
          case terminal::TerminalUnderlineVariant::Dashed: {
            const bool dotted = stroke.variant == terminal::TerminalUnderlineVariant::Dotted;
            const float mark = dotted ? thickness : std::max(3.0f, char_width * 0.5f);
            const float pitch = dotted ? thickness * 2.0f : mark + std::max(2.0f, char_width * 0.25f);
            for (float px = run_x; px < run_x + run_w; px += pitch) {
              DrawFilledRect(renderer, MakeRect(px, underline_y, std::min(mark, run_x + run_w - px), thickness),
                             stroke.underline_color);
            }
            break;
          }
        }
      }
      if (stroke.bits & terminal::cell_attr::kDoubleUnderline) {
        DrawFilledRect(renderer, MakeRect(run_x, y + line_height - thickness * 3.0f - 1.0f, run_w, thickness),
                       stroke.underline_color);
        DrawFilledRect(renderer, MakeRect(run_x, underline_y, run_w, thickness), stroke.underline_color);
      }
    }
  };

  // The header -- tabs, chevrons, label, buttons -- as prep settled it.
  PaintBottomPanelHeader(renderer, frame_state_, theme_, text_renderer_);

  // Phase E0: a plugin content surface replaces the terminal/output body. The
  // host owns scroll + clipping; the plugin only supplied data (display list or
  // raster handle). The surface content pointer is resolved by the builder.
  if (panel_vm.content == PanelContentKind::PluginSurface) {
    const SDL_FRect body = BottomPanelContentRect(layout);
    FillRect(renderer, body, theme_.surface_background);
    RenderPluginSurfaceInto(renderer, body, panel_vm.plugin_surface,
                            panel_vm.plugin_surface_scroll_y);
    // Pixel-unit scrollbar for an overflowing preview; the mouse coordinator
    // computes the identical geometry for thumb grabs (TD-2026-07-16-60).
    if (panel_vm.plugin_surface != nullptr && panel_vm.plugin_surface->has_body()) {
      if (const auto geometry = MakeVerticalScrollbarGeometry(
              body, PluginSurfacePreviewContentHeight(*panel_vm.plugin_surface), body.h,
              panel_vm.plugin_surface_scroll_y);
          geometry.has_value()) {
        DrawScrollbar(renderer, theme_, geometry->track, geometry->thumb,
                      context_.interaction_state.drag_target == DragTarget::BottomPanelScrollbar);
      }
    }
    // The plugin surface is as focusable as the terminal and the Output channel —
    // it is in the Ctrl+Tab ring and now answers the navigation keys — but this
    // early return used to skip the ring the other two contents draw, so a focused
    // preview was the one surface in the shell with no visible focus.
    if (panel_vm.focus == FocusTarget::Panel) {
      DrawSurfaceFocusRing(renderer, layout.bottom_panel);
    }
    return;
  }

  // Paints every pane of the tab on screen into its own slice of the panel body,
  // then the dividers between them. The active pane carries the selection, the
  // find highlights and the blinking caret; the others paint plainly with a
  // steady outline caret.
  const auto render_terminal_panes = [&]() {
    TerminalTabState* tab = panel_vm.terminal_tab;
    if (tab == nullptr || tab->panes.empty()) {
      // A Terminal panel with no live session: the builder's hint over an empty body.
      const LogSurfaceLayout empty_layout = ComputeBottomPanelLogLayout(layout, 0);
      if (!panel_vm.empty_label.empty()) {
        DrawWrappedPlaceholder(text_renderer_, renderer, empty_layout.text_x, empty_layout.text_y,
                               empty_layout.text_width, theme_.text_muted, theme_.surface_background,
                               panel_vm.empty_label);
      }
      return;
    }
    // Every pane was sampled once for this frame (PrepareTerminalPanesForFrame):
    // its line count, persisted scroll, visible lines and cursor. Nothing below
    // reads the live session or writes pane state.
    const TerminalPaneRectsLayout& rects = panel_vm.terminal_panes;
    const float panel_height = layout.bottom_panel.h;
    for (std::size_t pane_index = 0; pane_index < tab->panes.size() && pane_index < rects.panes.size();
         ++pane_index) {
      TerminalPaneState* pane = tab->panes[pane_index].get();
      if (pane == nullptr) {
        continue;
      }
      const bool is_active = pane_index == tab->active_pane;
      terminal_palette.reverse_video = pane->frame_reverse_video;
      terminal_palette.selection = pane->frame_selection_live
                                       ? theme_.selection_fill
                                       : InactiveTerminalSelection(theme_.selection_fill);
      const SDL_FRect& body = rects.panes[pane_index];
      if (terminal_palette.reverse_video) {
        // The blank the rows do not cover takes the default foreground too.
        FillRect(renderer, body, theme_.text_primary);
      }
      const std::size_t line_count = pane->frame_line_count;
      const LogSurfaceLayout pane_layout =
          ComputeTerminalPaneLogLayout(body, panel_height, *pane, line_count);
      const std::size_t first_row =
          static_cast<std::size_t>(std::max(0, pane_layout.scroll.vertical_scroll));
      const std::vector<terminal::TerminalLine>& terminal_lines = pane->visible_lines_snapshot.lines;

      // Find highlights (active pane only): the match list is (row, column)-ordered,
      // so seek once to the first hit at or after the top visible row and then walk
      // forward with the row loop, instead of searching the whole list per row.
      const std::vector<terminal::TerminalSearchMatch>* find_matches =
          is_active ? panel_vm.find_matches : nullptr;
      std::size_t find_cursor = 0;
      if (find_matches != nullptr && !find_matches->empty()) {
        find_cursor = static_cast<std::size_t>(
            std::lower_bound(find_matches->begin(), find_matches->end(), first_row,
                             [](const terminal::TerminalSearchMatch& match, std::size_t row) {
                               return match.row < row;
                             }) -
            find_matches->begin());
      }

      for (int row = 0; row < pane_layout.scroll.visible_rows; ++row) {
        const int index = pane_layout.scroll.vertical_scroll + row;
        if (index >= static_cast<int>(line_count)) {
          break;
        }
        const float line_y = pane_layout.text_y + static_cast<float>(row) * pane_layout.line_height;
        // The selection (only the active pane has one) and this row's slice of
        // the find matches, walked forward alongside the rows.
        TerminalRowEmphasis emphasis =
            TerminalRowSelectionEmphasis(pane->frame_selection, static_cast<std::size_t>(index));
        if (find_matches != nullptr) {
          const auto absolute_row = static_cast<std::size_t>(index);
          while (find_cursor < find_matches->size() &&
                 (*find_matches)[find_cursor].row < absolute_row) {
            ++find_cursor;
          }
          std::size_t row_end = find_cursor;
          while (row_end < find_matches->size() && (*find_matches)[row_end].row == absolute_row) {
            ++row_end;
          }
          if (row_end > find_cursor) {
            emphasis.matches = find_matches->data() + find_cursor;
            emphasis.match_count = row_end - find_cursor;
            if (panel_vm.find_selected_index >= find_cursor &&
                panel_vm.find_selected_index < row_end) {
              emphasis.current_match = panel_vm.find_selected_index - find_cursor;
            }
          }
        }
        // line_count comes from `session.LineCount()` sampled outside the session
        // mutex, while `terminal_lines` is the locked snapshot. They can disagree
        // when scrollback is trimmed between the two calls — the snapshot is the
        // source of truth for what to draw. Guard against that skew before indexing.
        const std::size_t snapshot_index = static_cast<std::size_t>(index) - first_row;
        if (snapshot_index >= terminal_lines.size()) {
          break;
        }
        draw_terminal_line(pane_layout.text_x, line_y, pane_layout.text_width, terminal_lines[snapshot_index],
                           emphasis, pane->visible_lines_snapshot.underline_colors);
        if (pane->hovered_link.has_value() &&
            pane->hovered_link->row <= static_cast<std::size_t>(index) &&
            static_cast<std::size_t>(index) <= pane->hovered_link->end_row) {
          // The hovered link's underline, in the text's own colour; a link the
          // terminal wrapped is underlined to the end of each row but its last.
          const TerminalLinkSpan& link = *pane->hovered_link;
          const std::size_t row = static_cast<std::size_t>(index);
          const std::size_t begin_column = row == link.row ? link.begin_column : 0;
          const std::size_t end_column =
              row == link.end_row ? link.end_column : terminal_lines[snapshot_index].cells.size();
          const float char_width = std::max(1.0f, terminal_text_renderer_.CharWidth());
          const float thickness = std::max(1.0f, std::round(pane_layout.line_height / 14.0f));
          const float link_x = pane_layout.text_x + static_cast<float>(begin_column) * char_width;
          const float link_w = std::min(
              static_cast<float>(end_column - std::min(begin_column, end_column)) * char_width,
              std::max(0.0f, pane_layout.text_x + pane_layout.text_width - link_x));
          const std::vector<terminal::TerminalCell>& cells = terminal_lines[snapshot_index].cells;
          if (link_w > 0.0f && begin_column < cells.size()) {
            DrawFilledRect(renderer,
                           MakeRect(link_x, line_y + pane_layout.line_height - thickness - 1.0f, link_w, thickness),
                           ResolveTerminalCellColors(cells[begin_column].style, terminal_palette).foreground);
          }
        }
      }

      if (line_count == 0 && is_active && !panel_vm.empty_label.empty()) {
        DrawWrappedPlaceholder(text_renderer_, renderer, pane_layout.text_x, pane_layout.text_y,
                               pane_layout.text_width, theme_.text_muted, theme_.surface_background,
                               panel_vm.empty_label);
      }

      const terminal::TerminalCursorSnapshot& cursor = pane->frame_cursor;
      // Same rect the blink invalidation asks for (TerminalCaretRectIn), so the
      // dirty region and the painted caret cannot disagree.
      const std::optional<SDL_FRect> caret_rect = TerminalCaretRectIn(pane_layout, cursor);
      if (caret_rect.has_value()) {
        // The program's DECSCUSR shape; a steady one does not blink.
        const SDL_FRect shape_rect = TerminalCursorShapeRect(*caret_rect, cursor.shape);
        const bool block = cursor.shape == terminal::TerminalCursorShape::Block;
        if (!is_active || !frame_state_.terminal_owns_caret) {
          // A pane that does not own the keyboard -- an inactive pane, or the active
          // one while focus is elsewhere or on its find bar -- shows where its cursor
          // is with a steady outline, as VS Code's unfocused terminal does. The
          // active pane blinked along with the find bar's caret while only the find
          // bar's caret was repainted (QA monkey, seed 4). A bar or an underline is
          // too thin to outline, so it is drawn steady as it is.
          if (block) {
            DrawRect(renderer, *caret_rect, theme_.cursor);
          } else {
            DrawFilledRect(renderer, shape_rect, theme_.cursor);
          }
        } else if (frame_state_.caret_visible || !cursor.blinking) {
          DrawFilledRect(renderer, shape_rect, theme_.cursor);
          if (block && cursor.row >= first_row && cursor.row - first_row < terminal_lines.size()) {
            const auto& line = terminal_lines[cursor.row - first_row];
            if (cursor.column < line.cells.size()) {
              const auto& cell = line.cells[cursor.column];
              const auto display_text = cell.DisplayText();
              if (!display_text.empty()) {
                const SDL_Color cursor_foreground =
                    ResolveTerminalCellColors(cell.style, terminal_palette).background;
                // The glyph is drawn at the caret's own origin; the rect carries a
                // 1px lift, so undo it here to keep the text on the row baseline.
                terminal_text_renderer_.DrawString(renderer, caret_rect->x, caret_rect->y + 1.0f,
                                                   cursor_foreground, display_text);
              }
            }
          }
        }
      }

      if (pane_layout.scroll.vertical_scrollbar.has_value()) {
        DrawScrollbar(renderer, theme_, pane_layout.scroll.vertical_scrollbar->track,
                      pane_layout.scroll.vertical_scrollbar->thumb,
                      is_active &&
                          context_.interaction_state.drag_target == DragTarget::BottomPanelScrollbar);
      }
    }

    for (const TerminalPaneDividerRect& divider : rects.dividers) {
      const bool divider_active =
          context_.interaction_state.drag_target == DragTarget::TerminalPaneDivider &&
          divider.boundary == context_.interaction_state.drag_terminal_pane_boundary;
      DrawFilledRect(renderer, divider.rect, divider_active ? theme_.accent : theme_.border);
    }
  };

  if (terminal_panel) {
    render_terminal_panes();
    // The find bar floats above the active pane (and above its scrollbar), the
    // same compact card the in-file find widget uses.
    if (panel_vm.find_visible) {
      RenderFindWidget(renderer, panel_vm.find);
    }
    if (panel_vm.focus == FocusTarget::Panel) {
      DrawSurfaceFocusRing(renderer, layout.bottom_panel);
    }
    return;
  }

  // The Output body -- rows, hint, scrollbar -- as prep settled it.
  PaintBottomPanelOutputBody(renderer, frame_state_, theme_, text_renderer_);
  if (panel_vm.focus == FocusTarget::Panel) {
    DrawSurfaceFocusRing(renderer, layout.bottom_panel);
  }
}


// Host-owned painter for a single plugin content surface, shared by the bottom
// preview panel (E0) and inline insets (E1). The surface carries only data (a
// display list or a raster handle); all drawing happens here. `scroll_y` is the
// host-owned vertical scroll within the surface. Reads no project state.
void WorkspaceShell::RenderPluginSurfaceInto(SDL_Renderer* renderer, const SDL_FRect& rect,
                                             const editor::SurfaceContent* content,
                                             float scroll_y) {
  if (renderer == nullptr || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }
  // Make any freshly decoded rasters available before we look them up.
  surface_texture_cache_.Upload(renderer);

  constexpr float kPad = kPluginSurfacePreviewPadding;
  if (content == nullptr || !content->has_body()) {
    return;
  }

  if (const auto* display_list = std::get_if<render::PluginDisplayList>(&content->body)) {
    render::DisplayListReplayParams params;
    params.origin_x = rect.x + kPad;
    params.origin_y = rect.y + kPad - scroll_y;
    params.clip = rect;
    render::ReplayDisplayList(renderer, text_renderer_, surface_texture_cache_, *display_list,
                              params);
    return;
  }

  if (const auto* raster = std::get_if<editor::RasterHandle>(&content->body)) {
    RenderRasterSurfaceInto(renderer, rect, *raster, scroll_y, surface_texture_cache_,
                            text_renderer_, theme_);
  }
}

// Phase E1 (gated): paint inline-surface insets into the inert row-gaps the view
// model carries. Uses the same EditorRowYLayout mapping the text-row loop used,
// so an inset always lands directly below its anchor row. Reads only the view
// model + metrics, never project state.
void WorkspaceShell::DrawEditorInsets(SDL_Renderer* renderer, const SDL_FRect& pane_rect,
                                      const editor::EditorViewMetrics& metrics,
                                      std::size_t scroll_line,
                                      const editor::EditorViewModel& view_model) {
  if (view_model.row_gaps.empty()) {
    return;
  }
  const editor::EditorRowYLayout layout(metrics.first_line_y, metrics.line_height,
                                        static_cast<std::uint32_t>(scroll_line),
                                        view_model.row_gaps);
  for (std::size_t i = 0; i < view_model.row_gaps.size(); ++i) {
    const editor::RowGap& gap = view_model.row_gaps[i];
    if (gap.visual_row < scroll_line) {
      continue;
    }
    const std::size_t row = gap.visual_row - scroll_line;
    const editor::RowGapContent content = i < view_model.row_gap_contents.size()
                                              ? view_model.row_gap_contents[i]
                                              : editor::RowGapContent{};
    if (gap.placement == editor::RowGapPlacement::Above) {
      // Phase E2: above-line code-lens strip sits directly over the row's text.
      const float top = layout.RowTop(row) - gap.height;
      if (content.code_lens != nullptr && !content.code_lens->text.empty()) {
        const float text_x = pane_rect.x + metrics.gutter_width + 4.0f;
        const float text_y = top + std::max(0.0f, (gap.height - metrics.line_height) * 0.5f);
        editor_text_renderer_.DrawString(renderer, text_x, text_y, theme_.accent,
                                         content.code_lens->text);
      }
      continue;
    }
    const float top = layout.RowTop(row) + metrics.line_height;
    if (content.ghost_text != nullptr) {
      // Below-caret ghost-text rows, dimmed and aligned under the code (the first
      // line renders inline at the caret in EditorViewRenderer).
      for (std::size_t j = 0; j < content.ghost_text->below_lines.size(); ++j) {
        editor_text_renderer_.DrawString(renderer, metrics.text_x,
                                         top + static_cast<float>(j) * metrics.line_height,
                                         theme_.text_muted, content.ghost_text->below_lines[j]);
      }
      continue;
    }
    const SDL_FRect inset{pane_rect.x, top, pane_rect.w, gap.height};
    RenderPluginSurfaceInto(renderer, inset, content.surface, 0.0f);
  }
}

}  // namespace microide::workspace
