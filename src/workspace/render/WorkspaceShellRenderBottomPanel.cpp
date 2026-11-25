#include "render/SurfacePrimitives.h"
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
#include "workspace/render/BottomPanelTerminalChromePaint.h"
#include "workspace/render/BottomPanelTerminalGridPaint.h"
#include "workspace/render/TerminalRowPaint.h"

namespace microide::workspace {

using namespace detail;

namespace {

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
  render::DrawTexture(renderer, entry->texture, nullptr, dest);
}

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

}

void PrepareBottomPanelHeaderFrame(PreparedFrameState& frame, const BottomPanelHeaderInputs& inputs) {
  PreparedFrameState::PanelHeaderPaint& header = frame.panel_header;
  header.tabs.clear();
  header.text.clear();
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
    const bool still = !panel.tab_drag.active && !panel.tab_drag.sliding;
    const std::vector<VisibleStripTab>& tabs =
        panel.tabs != nullptr ? *panel.tabs : TabStripService::EmptyVisibleTabs();
    std::size_t text_size = 0;
    for (const VisibleStripTab& tab : tabs) {
      text_size += tab.display_title.size() + tab.badge_text.size() + tab.secondary_title.size();
    }
    header.text.reserve(text_size);
    const auto own = [&header](std::string_view text) {
      const std::size_t at = header.text.size();
      header.text.append(text);
      return std::string_view(header.text).substr(at, text.size());
    };
    for (const VisibleStripTab& tab : tabs) {
      if (panel.tab_drag.active && tab.index == panel.tab_drag.source_index) {
        continue;
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
          .title = own(tab.display_title),
          .badge_text = own(tab.badge_text),
          .badge_color = tab.badge_color,
          .secondary_title = own(tab.secondary_title),
          .remote_state = tab.remote_state,
          .show_badge = tab.show_badge,
          .active = tab.active,
          .hovered = still && inputs.pointer_over(rect),
          .close_hovered = still && inputs.pointer_over(close_rect),
          .dimmed = tab.dimmed,
          .activity_dot = tab.activity_dot,
          .bell_mark = tab.bell_mark,
      });
      units.push_back(DerivedDamage::Unit{.rect = render::SnapToDevicePixelsF(inputs.renderer, rect),
                                          .fingerprint = FingerprintOf(painted, palette_key)});
    }
    header.label = tabs.empty() ? inputs.label : std::string_view{};
    units.push_back(DerivedDamage::Unit{
        .rect = render::SnapToDevicePixelsF(inputs.renderer, header.header),
        .fingerprint = PaintFingerprint().Add(header.header).Add(header.label).Add(palette_key).value()});
    const auto button = [&](PreparedFrameState::PanelButtonPaint& slot, const SDL_FRect& rect, std::size_t hidden) {
      slot = PreparedFrameState::PanelButtonPaint{.rect = rect, .hovered = inputs.pointer_over(rect), .hidden = hidden};
      units.push_back(DerivedDamage::Unit{.rect = render::SnapToDevicePixelsF(inputs.renderer, rect),
                                          .fingerprint = FingerprintOf(slot, palette_key)});
    };
    button(header.overflow_left, panel.tab_overflow.left_button, panel.tab_overflow.hidden_left);
    button(header.overflow_right, panel.tab_overflow.right_button, panel.tab_overflow.hidden_right);
    button(header.new_terminal, inputs.new_terminal_rect, 0);
    header.maximized = inputs.layout->panel_maximized;
    button(header.maximize, inputs.maximize_rect, header.maximized ? 1 : 0);
    if (panel.tab_drag.active) {
      header.drag_tabs_storage = tabs;
      header.drag_tabs = &header.drag_tabs_storage;
      header.dragging = true;
      header.drag_source = panel.tab_drag.source_index;
      header.drag_pointer_x = panel.tab_drag.pointer_x;
      header.drag_grab_offset_x = panel.tab_drag.grab_offset_x;
    }
    header.divider = MakeRect(header.header.x, header.header.y, header.header.w, kWorkspaceDividerThickness);
    header.divider_active = inputs.divider_dragged;
    units.push_back(DerivedDamage::Unit{
        .rect = render::SnapToDevicePixelsF(inputs.renderer, header.divider),
        .fingerprint = PaintFingerprint().Add(header.divider).Add(header.divider_active).Add(palette_key).value()});
    units.push_back(DerivedDamage::Unit{.rect = render::SnapToDevicePixelsF(inputs.renderer, header.header),
                                        .fingerprint = PaintFingerprint()
                                                           .Add(header.dragging)
                                                           .Add(header.drag_source)
                                                           .Add(header.drag_pointer_x)
                                                           .Add(header.drag_grab_offset_x)
                                                           .value()});
    const SDL_FRect body = BottomPanelContentRect(*inputs.layout);
    units.push_back(DerivedDamage::Unit{
        .rect = render::SnapToDevicePixelsF(inputs.renderer, body),
        .fingerprint =
            PaintFingerprint().Add(body).Add(static_cast<int>(panel.content)).Add(theme.surface_background).value()});
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
    DrawStripTabCloseGlyph(renderer, tab.rect, tab.close_rect, tab.close_hovered,
                           tab.active ? palette.active_glyph : palette.inactive_glyph, palette.active_text);
    if (tab.activity_dot || tab.bell_mark) {
      DrawStripTabActivityMark(renderer, tab.rect, tab.close_rect, tab.bell_mark,
                               tab.bell_mark ? theme.diagnostic_warning : theme.accent);
    }
  }
  if (!header.label.empty()) {
    const render::PaintElement element(renderer, header.header, "panel-label");
    DrawVCenteredTextOn(text_renderer, renderer, header.header, 12.0f, theme.chrome_text, theme.chrome_background,
                        header.label);
  }
  DrawTabStripOverflowButton(text_renderer, renderer, theme, header.overflow_left.rect, false,
                             header.overflow_left.hidden, header.overflow_left.hovered);
  DrawTabStripOverflowButton(text_renderer, renderer, theme, header.overflow_right.rect, true,
                             header.overflow_right.hidden, header.overflow_right.hovered);
  if (header.dragging && header.drag_tabs != nullptr) {
    DrawTabDragFeedback(text_renderer, renderer, theme, header.header, *header.drag_tabs, header.header,
                        header.drag_source, header.drag_pointer_x, header.drag_grab_offset_x, kPanelTabStyle,
                        palette);
  }
  {
    const PreparedFrameState::PanelButtonPaint& button = header.new_terminal;
    const render::PaintElement element(renderer, button.rect, "panel-new-terminal");
    DrawButtonCentered(text_renderer, renderer, theme, button.rect, "", ButtonTone::Neutral,
                       ButtonVisualState{.enabled = true, .hovered = button.hovered, .active = false});
    DrawPlusGlyph(renderer, button.rect, button.hovered ? theme.text_primary : theme.text_secondary);
  }
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
  {
    const render::PaintElement element(renderer, header.divider, "panel-divider");
    FillRect(renderer, header.divider, header.divider_active ? theme.accent : theme.border);
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
    const float content_bottom = inputs.content.y + inputs.content.h;
    for (int row = 0; row < inputs.visible_rows; ++row) {
      const int index = inputs.first_row + row;
      if (index < 0 || static_cast<std::size_t>(index) >= count ||
          inputs.text_y + static_cast<float>(row + 1) * inputs.line_height > content_bottom) {
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
        static const std::filesystem::path kNoRoot;
        const std::filesystem::path& resolved = inputs.channels->ResolvedReferencePath(
            inputs.channel_id, entry, inputs.project_root != nullptr ? *inputs.project_root : kNoRoot);
        painted.snippet = true;
        painted.prefix = parsed->prefix;
        painted.code = parsed->code;
        painted.highlighted = inputs.channels->HighlightedContextSnippet(inputs.channel_id, entry, resolved);
        painted.highlight_path = std::hash<std::string_view>{}(resolved.native()) | 1u;
      }
      units.push_back(DerivedDamage::Unit{.rect = render::SnapToDevicePixelsF(inputs.renderer, painted.rect),
                                          .fingerprint = FingerprintOf(painted, row_style)});
    }
    if (count == 0 && !inputs.empty_label.empty() && inputs.text_y + inputs.line_height <= content_bottom) {
      body.hint = inputs.empty_label;
      body.hint_rect = MakeRect(inputs.text_x, inputs.text_y, inputs.text_width,
                                std::max(0.0f, inputs.content.y + inputs.content.h - inputs.text_y));
    }
    units.push_back(DerivedDamage::Unit{
        .rect = render::SnapToDevicePixelsF(inputs.renderer, body.hint_rect),
        .fingerprint = PaintFingerprint().Add(body.hint_rect).Add(body.hint).Add(row_style).value()});
    if (inputs.scrollbar.has_value()) {
      body.scrollbar = PreparedFrameState::PanelScrollbarPaint{
          .track = inputs.scrollbar->track, .thumb = inputs.scrollbar->thumb, .active = inputs.scrollbar_active};
      const std::uint64_t bar_style =
          PaintFingerprint().Add(theme.surface_raised).Add(theme.text_muted).Add(theme.accent).value();
      units.push_back(DerivedDamage::Unit{.rect = render::SnapToDevicePixelsF(inputs.renderer, body.scrollbar->track),
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
    DrawWrappedPlaceholderIn(text_renderer, renderer, body.hint_rect, theme.text_muted, theme.surface_background,
                             body.hint);
  }
  if (body.scrollbar.has_value()) {
    const render::PaintElement element(renderer, body.scrollbar->track, "output-scrollbar");
    DrawScrollbar(renderer, theme, body.scrollbar->track, body.scrollbar->thumb, body.scrollbar->active);
  }
}

void PrepareTerminalChromeFrame(PreparedFrameState& frame, const TerminalChromeInputs& inputs) {
  PreparedFrameState::TerminalChromePaint& chrome = frame.terminal_chrome;
  chrome = PreparedFrameState::TerminalChromePaint{};
  std::vector<DerivedDamage::Unit>& units = frame.derived_damage.Begin(DerivedSurface::BottomPanelTerminalChrome);
  if (inputs.visible) {
    chrome.visible = true;
    const render::Theme& theme = *inputs.theme;
    const std::uint64_t style = PaintFingerprint()
                                    .Add(theme.text_muted)
                                    .Add(theme.surface_background)
                                    .Add(theme.surface_raised)
                                    .Add(theme.accent)
                                    .Add(theme.border)
                                    .Add(inputs.text_metrics)
                                    .value();
    const auto unit = [&](const SDL_FRect& rect, std::uint64_t fingerprint) {
      units.push_back(
          DerivedDamage::Unit{.rect = render::SnapToDevicePixelsF(inputs.renderer, rect), .fingerprint = fingerprint});
    };
    if (inputs.no_session_box.has_value() && !inputs.empty_label.empty()) {
      chrome.no_session = {.rect = *inputs.no_session_box, .text = inputs.empty_label};
    }
    unit(chrome.no_session.rect, FingerprintOf(chrome.no_session, style));
    for (const TerminalPaneChromeInputs& pane : inputs.panes) {
      PreparedFrameState::TerminalHintPaint& hint = chrome.hints.emplace_back();
      if (pane.shows_hint && !inputs.empty_label.empty()) {
        hint = {.rect = pane.text_box, .text = inputs.empty_label};
      }
      unit(hint.rect, FingerprintOf(hint, style));
      std::optional<PreparedFrameState::PanelScrollbarPaint>& bar = chrome.scrollbars.emplace_back();
      if (pane.scrollbar.has_value()) {
        bar = PreparedFrameState::PanelScrollbarPaint{
            .track = pane.scrollbar->track, .thumb = pane.scrollbar->thumb, .active = pane.scrollbar_active};
        unit(bar->track, FingerprintOf(*bar, style));
      } else {
        unit(SDL_FRect{}, 0);
      }
    }
    for (const PreparedFrameState::TerminalDividerPaint& divider : inputs.dividers) {
      chrome.dividers.push_back(divider);
      unit(divider.rect, FingerprintOf(divider, style));
    }
  }
  frame.derived_damage.Commit(DerivedSurface::BottomPanelTerminalChrome, inputs.damage);
}

namespace {

void PaintTerminalHint(SDL_Renderer* renderer, const PreparedFrameState::TerminalHintPaint& hint,
                       const render::Theme& theme, const render::TextRenderer& text_renderer) {
  if (hint.text.empty()) {
    return;
  }
  const render::PaintElement element(renderer, hint.rect, "terminal-hint");
  DrawWrappedPlaceholderIn(text_renderer, renderer, hint.rect, theme.text_muted, theme.surface_background,
                           hint.text);
}

}

void PaintTerminalNoSessionHint(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme,
                                const render::TextRenderer& text_renderer) {
  PaintTerminalHint(renderer, frame.terminal_chrome.no_session, theme, text_renderer);
}

void PaintTerminalPaneHint(SDL_Renderer* renderer, const PreparedFrameState& frame, std::size_t pane,
                           const render::Theme& theme, const render::TextRenderer& text_renderer) {
  if (pane < frame.terminal_chrome.hints.size()) {
    PaintTerminalHint(renderer, frame.terminal_chrome.hints[pane], theme, text_renderer);
  }
}

void PaintTerminalChrome(SDL_Renderer* renderer, const PreparedFrameState& frame, const render::Theme& theme) {
  const PreparedFrameState::TerminalChromePaint& chrome = frame.terminal_chrome;
  if (!chrome.visible) {
    return;
  }
  for (const std::optional<PreparedFrameState::PanelScrollbarPaint>& bar : chrome.scrollbars) {
    if (bar.has_value()) {
      const render::PaintElement element(renderer, bar->track, "terminal-scrollbar");
      DrawScrollbar(renderer, theme, bar->track, bar->thumb, bar->active);
    }
  }
  for (const PreparedFrameState::TerminalDividerPaint& divider : chrome.dividers) {
    const render::PaintElement element(renderer, divider.rect, "terminal-divider");
    FillRect(renderer, divider.rect, divider.active ? theme.accent : theme.border);
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

  PaintBottomPanelHeader(renderer, frame_state_, theme_, text_renderer_);

  if (panel_vm.content == PanelContentKind::PluginSurface) {
    const SDL_FRect body = BottomPanelContentRect(layout);
    FillRect(renderer, body, theme_.surface_background);
    RenderPluginSurfaceInto(renderer, body, panel_vm.plugin_surface,
                            panel_vm.plugin_surface_scroll_y);
    if (panel_vm.plugin_surface != nullptr && panel_vm.plugin_surface->has_body()) {
      if (const auto geometry = MakeVerticalScrollbarGeometry(
              body, PluginSurfacePreviewContentHeight(*panel_vm.plugin_surface), body.h,
              panel_vm.plugin_surface_scroll_y);
          geometry.has_value()) {
        DrawScrollbar(renderer, theme_, geometry->track, geometry->thumb,
                      context_.interaction_state.drag_target == DragTarget::BottomPanelScrollbar);
      }
    }
    if (panel_vm.focus == FocusTarget::Panel) {
      DrawSurfaceFocusRing(renderer, layout.bottom_panel);
    }
    return;
  }

  const auto render_terminal_panes = [&]() {
    TerminalTabState* tab = panel_vm.terminal_tab;
    if (tab == nullptr || tab->panes.empty()) {
      PaintTerminalNoSessionHint(renderer, frame_state_, theme_, text_renderer_);
      return;
    }
    const TerminalPaneRectsLayout& rects = panel_vm.terminal_panes;
    for (std::size_t pane_index = 0; pane_index < tab->panes.size() && pane_index < rects.panes.size();
         ++pane_index) {
      if (tab->panes[pane_index] == nullptr) {
        continue;
      }
      PaintTerminalGridPane(renderer, frame_state_, pane_index, theme_, terminal_text_renderer_,
                            terminal_row_plan_);
      PaintTerminalPaneHint(renderer, frame_state_, pane_index, theme_, text_renderer_);
      PaintTerminalGridCaret(renderer, frame_state_, pane_index, theme_, terminal_text_renderer_);
    }
    PaintTerminalChrome(renderer, frame_state_, theme_);
  };

  if (terminal_panel) {
    render_terminal_panes();
    if (panel_vm.find_visible) {
      RenderFindWidget(renderer, panel_vm.find);
    }
    if (panel_vm.focus == FocusTarget::Panel) {
      DrawSurfaceFocusRing(renderer, layout.bottom_panel);
    }
    return;
  }

  PaintBottomPanelOutputBody(renderer, frame_state_, theme_, text_renderer_);
  if (panel_vm.focus == FocusTarget::Panel) {
    DrawSurfaceFocusRing(renderer, layout.bottom_panel);
  }
}

void WorkspaceShell::RenderPluginSurfaceInto(SDL_Renderer* renderer, const SDL_FRect& rect,
                                             const editor::SurfaceContent* content,
                                             float scroll_y) {
  if (renderer == nullptr || rect.w <= 0.0f || rect.h <= 0.0f) {
    return;
  }
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

}
