#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "workspace/WorkspaceShellShared.h"

namespace microide::workspace {

namespace {

constexpr float kDivider = 1.0f;
constexpr float kSidebarHeaderHeight = 30.0f;
constexpr float kBottomPanelHeaderHeight = 28.0f;
constexpr float kSidebarInset = 10.0f;
constexpr float kSidebarRowHeight = 20.0f;
constexpr float kTreeIndentWidth = 14.0f;
constexpr float kTreeChevronSlotWidth = 12.0f;
constexpr float kDirtyPromptWidth = 460.0f;
constexpr float kDirtyPromptHeight = 176.0f;
constexpr float kDirtyPromptButtonWidth = 96.0f;
constexpr float kDirtyPromptButtonHeight = 28.0f;
constexpr float kDirtyPromptButtonGap = 10.0f;
constexpr float kPromptSurfaceWidth = 520.0f;
constexpr float kPromptSurfaceHeight = 188.0f;
constexpr float kPromptSurfaceInputHeight = 24.0f;
constexpr float kPromptSurfaceButtonWidth = 108.0f;
constexpr float kPromptSurfaceButtonHeight = 28.0f;
constexpr float kPromptSurfaceButtonGap = 10.0f;
constexpr float kScrollbarThickness = 10.0f;
constexpr float kScrollbarInset = 2.0f;
constexpr float kBottomPanelCommandTopPadding = 8.0f;
constexpr float kCompareMarkerLaneWidth = 6.0f;
constexpr float kCompareMarkerLaneGap = 3.0f;

SDL_FRect ComputeDirtyPromptRect(const SDL_FRect& full) {
  const float width = std::min(kDirtyPromptWidth, full.w - 32.0f);
  const float height = std::min(kDirtyPromptHeight, full.h - 32.0f);
  return MakeRect(full.x + std::floor((full.w - width) * 0.5f),
                  full.y + std::floor((full.h - height) * 0.5f), width, height);
}

std::array<SDL_FRect, 3> ComputeDirtyPromptButtonRects(const SDL_FRect& dialog) {
  const float total_width =
      kDirtyPromptButtonWidth * 3.0f + kDirtyPromptButtonGap * 2.0f;
  const float start_x = dialog.x + dialog.w - total_width - 16.0f;
  const float y = dialog.y + dialog.h - kDirtyPromptButtonHeight - 16.0f;
  return {
      MakeRect(start_x, y, kDirtyPromptButtonWidth, kDirtyPromptButtonHeight),
      MakeRect(start_x + kDirtyPromptButtonWidth + kDirtyPromptButtonGap, y,
               kDirtyPromptButtonWidth, kDirtyPromptButtonHeight),
      MakeRect(start_x + (kDirtyPromptButtonWidth + kDirtyPromptButtonGap) * 2.0f, y,
               kDirtyPromptButtonWidth, kDirtyPromptButtonHeight),
  };
}

SDL_FRect ComputePromptSurfaceRect(const SDL_FRect& full) {
  const float width = std::min(kPromptSurfaceWidth, full.w - 32.0f);
  const float height = std::min(kPromptSurfaceHeight, full.h - 32.0f);
  return MakeRect(full.x + std::floor((full.w - width) * 0.5f),
                  full.y + std::floor((full.h - height) * 0.5f), width, height);
}

std::array<SDL_FRect, 2> ComputePromptSurfaceButtonRects(const SDL_FRect& dialog) {
  const float total_width =
      kPromptSurfaceButtonWidth * 2.0f + kPromptSurfaceButtonGap;
  const float start_x = dialog.x + dialog.w - total_width - 16.0f;
  const float y = dialog.y + dialog.h - kPromptSurfaceButtonHeight - 16.0f;
  return {
      MakeRect(start_x, y, kPromptSurfaceButtonWidth, kPromptSurfaceButtonHeight),
      MakeRect(start_x + kPromptSurfaceButtonWidth + kPromptSurfaceButtonGap, y,
               kPromptSurfaceButtonWidth, kPromptSurfaceButtonHeight),
  };
}

SDL_FRect ComputePromptSurfaceInputRect(const SDL_FRect& dialog) {
  return MakeRect(dialog.x + 16.0f, dialog.y + 98.0f, dialog.w - 32.0f,
                  kPromptSurfaceInputHeight);
}

void DrawScrollbarTrack(SDL_Renderer* renderer,
                        const render::Theme& theme,
                        const SDL_FRect& track) {
  if (renderer == nullptr || track.w <= 0.0f || track.h <= 0.0f) {
    return;
  }

  SDL_SetRenderDrawColor(renderer, theme.surface_raised.r, theme.surface_raised.g,
                         theme.surface_raised.b, theme.surface_raised.a);
  SDL_RenderFillRect(renderer, &track);
}

void DrawScrollbarThumb(SDL_Renderer* renderer,
                        const render::Theme& theme,
                        const SDL_FRect& thumb,
                        bool active = false) {
  if (renderer == nullptr || thumb.w <= 0.0f || thumb.h <= 0.0f) {
    return;
  }

  const SDL_Color thumb_color = active ? theme.accent : theme.text_disabled;
  SDL_SetRenderDrawColor(renderer, thumb_color.r, thumb_color.g, thumb_color.b, thumb_color.a);
  SDL_RenderFillRect(renderer, &thumb);
}

void DrawScrollbar(SDL_Renderer* renderer,
                   const render::Theme& theme,
                   const SDL_FRect& track,
                   const SDL_FRect& thumb,
                   bool active = false) {
  DrawScrollbarTrack(renderer, theme, track);
  DrawScrollbarThumb(renderer, theme, thumb, active);
}

SDL_Color CompareMarkerColor(const render::Theme& theme, compare::CompareRowKind kind) {
  switch (kind) {
    case compare::CompareRowKind::Added:
      return theme.diff_added;
    case compare::CompareRowKind::Deleted:
      return theme.diff_deleted;
    case compare::CompareRowKind::Modified:
      return theme.diff_modified;
    case compare::CompareRowKind::Unchanged:
    default:
      return theme.text_muted;
  }
}

void DrawCompareScrollbarMarkers(SDL_Renderer* renderer,
                                 const render::Theme& theme,
                                 const SDL_FRect& track,
                                 const compare::CompareModel& model) {
  if (renderer == nullptr) {
    return;
  }

  const auto markers = BuildCompareScrollbarMarkers(track, model);
  for (const CompareScrollbarMarker& marker : markers) {
    const SDL_Color color = CompareMarkerColor(theme, marker.kind);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer, &marker.rect);
  }
}

std::size_t MaxVisualColumns(const editor::TextViewport& viewport) {
  return viewport.max_visual_columns();
}

char GitMarker(project::GitFileStatus status) {
  switch (status) {
    case project::GitFileStatus::Conflicted:
      return '!';
    case project::GitFileStatus::Modified:
      return 'M';
    case project::GitFileStatus::Added:
      return 'A';
    case project::GitFileStatus::Deleted:
      return 'D';
    case project::GitFileStatus::Untracked:
      return 'U';
    case project::GitFileStatus::Clean:
    default:
      return ' ';
  }
}

SDL_Color GitMarkerColor(const render::Theme& theme, project::GitFileStatus status) {
  switch (status) {
    case project::GitFileStatus::Conflicted:
      return theme.accent;
    case project::GitFileStatus::Modified:
      return theme.diff_modified;
    case project::GitFileStatus::Added:
      return theme.diff_added;
    case project::GitFileStatus::Deleted:
      return theme.diff_deleted;
    case project::GitFileStatus::Untracked:
      return theme.accent;
    case project::GitFileStatus::Clean:
    default:
      return theme.text_disabled;
  }
}

void DrawChevron(SDL_Renderer* renderer, float x, float center_y, bool expanded, SDL_Color color) {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  if (expanded) {
    SDL_RenderLine(renderer, x, center_y - 2.0f, x + 4.0f, center_y + 2.0f);
    SDL_RenderLine(renderer, x + 8.0f, center_y - 2.0f, x + 4.0f, center_y + 2.0f);
    return;
  }

  SDL_RenderLine(renderer, x + 2.0f, center_y - 4.0f, x + 6.0f, center_y);
  SDL_RenderLine(renderer, x + 2.0f, center_y + 4.0f, x + 6.0f, center_y);
}

void DrawWindowControlGlyph(SDL_Renderer* renderer,
                            const SDL_FRect& rect,
                            microide::workspace::WorkspaceShell::WindowControlButtonId id,
                            SDL_Color color,
                            bool maximized) {
  if (renderer == nullptr) {
    return;
  }

  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  const float left = rect.x + 4.0f;
  const float right = rect.x + rect.w - 4.0f;
  const float top = rect.y + 4.0f;
  const float bottom = rect.y + rect.h - 4.0f;
  const float center_y = rect.y + rect.h * 0.5f;

  switch (id) {
    case microide::workspace::WorkspaceShell::WindowControlButtonId::Minimize:
      SDL_RenderLine(renderer, left, center_y + 2.0f, right, center_y + 2.0f);
      return;
    case microide::workspace::WorkspaceShell::WindowControlButtonId::Maximize:
      if (maximized) {
        const SDL_FRect back = SDL_FRect{left + 1.5f, top + 3.0f, rect.w - 9.0f, rect.h - 9.0f};
        const SDL_FRect front = SDL_FRect{left - 1.0f, top + 1.0f, rect.w - 9.0f, rect.h - 9.0f};
        SDL_RenderRect(renderer, &back);
        SDL_RenderRect(renderer, &front);
      } else {
        const SDL_FRect outline = SDL_FRect{left, top + 1.0f, rect.w - 8.0f, rect.h - 8.0f};
        SDL_RenderRect(renderer, &outline);
      }
      return;
    case microide::workspace::WorkspaceShell::WindowControlButtonId::Close:
      SDL_RenderLine(renderer, left, top, right, bottom);
      SDL_RenderLine(renderer, right, top, left, bottom);
      return;
  }
}

}  // namespace

std::string WorkspaceShell::BottomPanelHeaderLabel() const {
  if (!BottomPanelShowsTerminal()) {
    return "Bottom Panel | Logs";
  }

  const auto* terminal_tab = ActiveTerminalTab();
  if (terminal_tab == nullptr) {
    return "Bottom Panel | Logs";
  }

  const std::string launch_label = terminal_tab->session.LaunchLabel();
  if (launch_label.empty()) {
    return "Bottom Panel | Terminal";
  }
  return "Bottom Panel | Terminal | " + launch_label;
}

void WorkspaceShell::ResizeTerminalToPanel(const SDL_FRect& panel_rect) {
  auto* terminal_tab = ActiveTerminalTab();
  if (!BottomPanelShowsTerminal() || terminal_tab == nullptr) {
    return;
  }

  const int rows = BottomPanelVisibleRows(panel_rect.h);
  const float usable_width =
      std::max(16.0f, panel_rect.w - 24.0f - kScrollbarThickness - 6.0f);
  const int columns = std::max(
      1, static_cast<int>(std::floor(usable_width / std::max(1.0f, text_renderer_.CharWidth()))));
  terminal_tab->session.Resize(static_cast<std::size_t>(rows), static_cast<std::size_t>(columns));
}

void WorkspaceShell::DrawFilledRect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) const {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  SDL_RenderFillRect(renderer, &rect);
}

void WorkspaceShell::DrawRect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_Color color) const {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  SDL_RenderRect(renderer, &rect);
}

std::string WorkspaceShell::TruncateLabel(std::string_view text, float max_width) const {
  return text_renderer_.TruncateToWidth(text, max_width);
}

void WorkspaceShell::Render(SDL_Renderer* renderer, int width, int height) {
  if (renderer == nullptr || width <= 0 || height <= 0) {
    return;
  }

  ConsumeProjectSearchUpdates();
  text_renderer_.EnsureInitialized(renderer);
  last_window_width_ = width;
  last_window_height_ = height;
  sidebar_width_ = ClampSidebarWidth(sidebar_width_, static_cast<float>(width));
  bottom_panel_height_ = ClampBottomPanelHeight(bottom_panel_height_, static_cast<float>(height));

  const WorkspaceLayout layout =
      ComputeLayout(static_cast<float>(width), static_cast<float>(height), sidebar_visible_,
                    bottom_panel_visible_, sidebar_width_, bottom_panel_height_);
  SDL_Window* render_window = SDL_GetRenderWindow(renderer);
  SyncTextInputSurface(render_window);
  if (ActiveTabIsEditor()) {
    SyncActiveEditorTab();
    if (auto* editor_tab = ActiveEditorTab(); editor_tab != nullptr) {
      NormalizeEditorSplitTree(*editor_tab);
    }
  }
  if (bottom_panel_visible_ && BottomPanelShowsTerminal()) {
    ResizeTerminalToPanel(layout.bottom_panel);
  }
  float mouse_x = 0.0f;
  float mouse_y = 0.0f;
  SDL_GetMouseState(&mouse_x, &mouse_y);
  UpdateMouseCursor(mouse_x, mouse_y);
  const std::vector<terminal::TerminalLine> terminal_lines =
      bottom_panel_visible_ && BottomPanelShowsTerminal() && ActiveTerminalTab() != nullptr
          ? ActiveTerminalTab()->session.SnapshotLines()
          : std::vector<terminal::TerminalLine>{};
  std::optional<SDL_FRect> active_editor_pane_rect;
  const bool draw_editor_caret =
      CaretVisibleNow() &&
      !(CurrentTextInputSurface() == TextInputSurface::Editor && !text_composition_.text.empty());

  DrawFilledRect(renderer, layout.full, theme_.window_background);
  DrawFilledRect(renderer, layout.menu_bar, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(layout.menu_bar.x, layout.menu_bar.y + layout.menu_bar.h - kDivider,
                          layout.menu_bar.w, kDivider),
                 theme_.border);
  DrawFilledRect(renderer, layout.project_tab_strip, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(layout.project_tab_strip.x,
                          layout.project_tab_strip.y + layout.project_tab_strip.h - kDivider,
                          layout.project_tab_strip.w, kDivider),
                 theme_.border);
  DrawFilledRect(renderer, layout.tab_strip, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(layout.tab_strip.x, layout.tab_strip.y + layout.tab_strip.h - kDivider,
                          layout.tab_strip.w, kDivider),
                 theme_.border);
  DrawFilledRect(renderer, layout.breadcrumb, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(layout.breadcrumb.x, layout.breadcrumb.y + layout.breadcrumb.h - kDivider,
                          layout.breadcrumb.w, kDivider),
                 theme_.border);
  DrawFilledRect(renderer, layout.status_bar, theme_.chrome_background);
  DrawFilledRect(renderer,
                 MakeRect(layout.status_bar.x, layout.status_bar.y, layout.status_bar.w, kDivider),
                 theme_.border);

  if (sidebar_visible_) {
    DrawFilledRect(renderer, layout.sidebar, theme_.surface_background);
    DrawFilledRect(renderer,
                   MakeRect(layout.sidebar.x + layout.sidebar.w, layout.sidebar.y, kDivider,
                            layout.sidebar.h),
                   drag_target_ == DragTarget::SidebarDivider ? theme_.accent : theme_.border);
    const SDL_FRect sidebar_header =
        MakeRect(layout.sidebar.x, layout.sidebar.y, layout.sidebar.w, kSidebarHeaderHeight);
    DrawFilledRect(renderer, sidebar_header, theme_.chrome_background);
    DrawFilledRect(renderer,
                   MakeRect(sidebar_header.x, sidebar_header.y + sidebar_header.h - kDivider,
                            sidebar_header.w, kDivider),
                   theme_.border);
  }

  if (bottom_panel_visible_) {
    DrawFilledRect(renderer, layout.bottom_panel, theme_.surface_background);
    DrawFilledRect(renderer,
                   MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                            kDivider),
                   drag_target_ == DragTarget::BottomPanelDivider ? theme_.accent : theme_.border);
    const SDL_FRect panel_header = MakeRect(layout.bottom_panel.x, layout.bottom_panel.y,
                                            layout.bottom_panel.w, kBottomPanelHeaderHeight);
    DrawFilledRect(renderer, panel_header, theme_.chrome_background);
    DrawFilledRect(renderer,
                   MakeRect(panel_header.x, panel_header.y + panel_header.h - kDivider,
                            panel_header.w, kDivider),
                   theme_.border);
  }

  if (ActiveTabIsCompare()) {
    RenderCompareSurface(renderer, layout.editor_surface);
  } else if (ActiveTabIsMerge()) {
    RenderMergeSurface(renderer, layout.editor_surface);
  } else {
    const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
    if (panes.empty() && text_viewport_.is_placeholder()) {
      active_editor_pane_rect = layout.editor_surface;
      editor_view_renderer_.Render(renderer, text_renderer_, theme_, text_viewport_,
                                   layout.editor_surface, draw_editor_caret, "", std::nullopt);
    }
    auto* editor_tab = ActiveEditorTab();
    for (const EditorPaneLayout& pane : panes) {
      editor::TextViewport* viewport =
          pane.active ? &text_viewport_
                      : (editor_tab != nullptr ? FindEditorView(*editor_tab, pane.leaf_id)
                                               : nullptr);
      if (viewport == nullptr) {
        continue;
      }
      if (pane.active) {
        active_editor_pane_rect = pane.rect;
      }
      editor_view_renderer_.Render(renderer, text_renderer_, theme_, *viewport, pane.rect,
                                   pane.active && draw_editor_caret,
                                   pane.active && (overlay_mode_ == OverlayMode::BufferSearch ||
                                                   overlay_mode_ == OverlayMode::BufferReplace)
                                       ? buffer_search_query_
                                       : "",
                                   pane.active ? ActiveBufferSearchMatch() : std::nullopt);
    }
  }

  const auto draw_text_on =
      [&](float x, float y, SDL_Color foreground, SDL_Color background, std::string_view text) {
        text_renderer_.DrawStringOn(renderer, x, y, foreground, background, text);
      };
  struct TextInputVisual {
    TextInputSurface surface = TextInputSurface::None;
    SDL_FRect area{};
    float text_x = 0.0f;
    float text_y = 0.0f;
    float cursor_x = 0.0f;
    SDL_Color foreground{};
    SDL_Color background{};
  };
  const auto active_text_input_visual = [&]() -> std::optional<TextInputVisual> {
    const TextInputSurface surface = CurrentTextInputSurface();
    const float line_height = text_renderer_.LineHeight();
    const float char_width = std::max(1.0f, text_renderer_.CharWidth());

    switch (surface) {
      case TextInputSurface::Editor: {
        if (!active_editor_pane_rect.has_value()) {
          return std::nullopt;
        }
        const editor::EditorViewMetrics metrics =
            editor::EditorViewRenderer::ComputeMetrics(text_renderer_, text_viewport_,
                                                       *active_editor_pane_rect);
        const float cursor_x =
            metrics.text_x +
            static_cast<float>(text_viewport_.cursor_visual_column() - text_viewport_.horizontal_scroll()) *
                char_width;
        const float cursor_y =
            metrics.first_line_y +
            static_cast<float>(text_viewport_.cursor_line() - text_viewport_.scroll_line()) *
                metrics.line_height;
        return TextInputVisual{
            .surface = surface,
            .area = MakeRect(cursor_x, cursor_y - 1.0f, char_width, metrics.line_height),
            .text_x = cursor_x,
            .text_y = cursor_y,
            .cursor_x = cursor_x,
            .foreground = theme_.text_primary,
            .background = theme_.row_highlight,
        };
      }
      case TextInputSurface::Command: {
        const SDL_FRect prompt_rect = BottomPanelCommandPromptRect(layout);
        const float text_x = prompt_rect.x + 6.0f;
        const float text_y = prompt_rect.y + 4.0f;
        const float cursor_x = text_x + text_renderer_.MeasureWidth("> " + command_input_);
        return TextInputVisual{
            .surface = surface,
            .area = MakeRect(text_x, text_y, std::max(1.0f, prompt_rect.w - 12.0f), line_height),
            .text_x = text_x,
            .text_y = text_y,
            .cursor_x = cursor_x,
            .foreground = theme_.text_primary,
            .background = theme_.chrome_active,
        };
      }
      case TextInputSurface::PromptInput: {
        const SDL_FRect dialog = ComputePromptSurfaceRect(layout.full);
        const SDL_FRect input_rect = ComputePromptSurfaceInputRect(dialog);
        const float text_x = input_rect.x + 6.0f;
        const float text_y = input_rect.y + 4.0f;
        const float cursor_x = text_x + text_renderer_.MeasureWidth(prompt_surface_state_.input);
        return TextInputVisual{
            .surface = surface,
            .area = MakeRect(text_x, text_y, std::max(1.0f, input_rect.w - 12.0f), line_height),
            .text_x = text_x,
            .text_y = text_y,
            .cursor_x = cursor_x,
            .foreground = theme_.text_primary,
            .background = theme_.surface_background,
        };
      }
      case TextInputSurface::FileFinder:
      case TextInputSurface::BufferSearch:
      case TextInputSurface::BufferReplaceSearch:
      case TextInputSurface::BufferReplaceReplace:
      case TextInputSurface::ProjectSearchOverlay:
      case TextInputSurface::CommitPicker: {
        if (!overlay_visible_) {
          return std::nullopt;
        }
        const SDL_FRect overlay = ComputeOverlayRect(layout.editor_area);
        const float inset = 18.0f;
        float text_x = overlay.x + inset;
        float text_y = overlay.y + 44.0f;
        std::string prefix = "> ";
        switch (surface) {
          case TextInputSurface::BufferSearch:
            prefix += buffer_search_query_;
            break;
          case TextInputSurface::BufferReplaceSearch:
            prefix = "find: " + buffer_search_query_;
            break;
          case TextInputSurface::BufferReplaceReplace:
            text_y = overlay.y + 62.0f;
            prefix = "replace: " + buffer_replace_text_;
            break;
          case TextInputSurface::ProjectSearchOverlay:
            prefix += project_search_query_;
            break;
          case TextInputSurface::CommitPicker:
            text_y = overlay.y + 62.0f;
            prefix += compare_picker_query_;
            break;
          case TextInputSurface::FileFinder:
          default:
            prefix += file_finder_.query();
            break;
        }
        const float cursor_x = text_x + text_renderer_.MeasureWidth(prefix);
        return TextInputVisual{
            .surface = surface,
            .area = MakeRect(text_x, text_y, std::max(1.0f, overlay.w - inset * 2.0f), line_height),
            .text_x = text_x,
            .text_y = text_y,
            .cursor_x = cursor_x,
            .foreground = theme_.text_secondary,
            .background = theme_.overlay_background,
        };
      }
      case TextInputSurface::SidebarSearchQuery:
      case TextInputSurface::SidebarSearchReplace: {
        if (!sidebar_visible_ || sidebar_mode_ != SidebarMode::Search || !project_search_editing_) {
          return std::nullopt;
        }
        const float text_x = layout.sidebar.x + kSidebarInset;
        const float text_y =
            layout.sidebar.y + (surface == TextInputSurface::SidebarSearchQuery ? 38.0f : 54.0f);
        const std::string prefix =
            surface == TextInputSurface::SidebarSearchQuery
                ? "search> " + project_search_edit_buffer_
                : "replace> " + project_search_edit_buffer_;
        const float cursor_x = text_x + text_renderer_.MeasureWidth(prefix);
        return TextInputVisual{
            .surface = surface,
            .area = MakeRect(text_x, text_y, std::max(1.0f, layout.sidebar.w - kSidebarInset * 2.0f),
                             line_height),
            .text_x = text_x,
            .text_y = text_y,
            .cursor_x = cursor_x,
            .foreground = theme_.text_primary,
            .background = theme_.surface_background,
        };
      }
      case TextInputSurface::Terminal:
      case TextInputSurface::None:
      default:
        return std::nullopt;
    }
  }();
  const auto update_text_input_area = [&](const std::optional<TextInputVisual>& visual) {
    if (render_window == nullptr) {
      return;
    }

    if (!visual.has_value()) {
      SDL_SetTextInputArea(render_window, nullptr, 0);
      return;
    }

    const SDL_Rect area = SDL_Rect{
        static_cast<int>(std::floor(visual->area.x)),
        static_cast<int>(std::floor(visual->area.y)),
        std::max(1, static_cast<int>(std::ceil(visual->area.w))),
        std::max(1, static_cast<int>(std::ceil(visual->area.h))),
    };
    const int cursor =
        std::max(0, static_cast<int>(std::round(visual->cursor_x - visual->area.x)));
    SDL_SetTextInputArea(render_window, &area, cursor);
  };
  const auto render_text_composition = [&](const std::optional<TextInputVisual>& visual) {
    if (!visual.has_value() || text_composition_.text.empty() ||
        text_composition_.surface != visual->surface) {
      return;
    }

    const std::string_view composition = text_composition_.text;
    const std::size_t total_codepoints = Utf8CodepointCount(composition);
    const std::size_t selection_start_codepoints =
        text_composition_.start < 0
            ? total_codepoints
            : std::min<std::size_t>(static_cast<std::size_t>(text_composition_.start),
                                    total_codepoints);
    const std::size_t selection_end_codepoints =
        text_composition_.length <= 0
            ? selection_start_codepoints
            : std::min(total_codepoints,
                       selection_start_codepoints +
                           static_cast<std::size_t>(text_composition_.length));
    const std::size_t selection_start =
        Utf8ByteOffsetForCodepointCount(composition, selection_start_codepoints);
    const std::size_t selection_end =
        Utf8ByteOffsetForCodepointCount(composition, selection_end_codepoints);
    const std::string_view prefix = composition.substr(0, selection_start);
    const std::string_view selected =
        composition.substr(selection_start, selection_end - selection_start);
    const std::string_view suffix = composition.substr(selection_end);
    const float prefix_width = text_renderer_.MeasureWidth(prefix);
    const float selected_width = text_renderer_.MeasureWidth(selected);
    const float total_width = text_renderer_.MeasureWidth(composition);

    if (!selected.empty()) {
      DrawFilledRect(renderer,
                     MakeRect(visual->cursor_x + prefix_width, visual->text_y - 1.0f, selected_width,
                              text_renderer_.LineHeight()),
                     theme_.selection_fill);
    }

    float segment_x = visual->cursor_x;
    if (!prefix.empty()) {
      text_renderer_.DrawStringOn(renderer, segment_x, visual->text_y, theme_.accent,
                                  visual->background, prefix);
      segment_x += prefix_width;
    }
    if (!selected.empty()) {
      text_renderer_.DrawStringOn(renderer, segment_x, visual->text_y, theme_.text_primary,
                                  theme_.selection_fill, selected);
      segment_x += selected_width;
    }
    if (!suffix.empty()) {
      text_renderer_.DrawStringOn(renderer, segment_x, visual->text_y, theme_.accent,
                                  visual->background, suffix);
    }

    DrawFilledRect(renderer,
                   MakeRect(visual->cursor_x, visual->text_y + text_renderer_.LineHeight() - 1.0f,
                            total_width, 1.0f),
                   theme_.accent);
    DrawFilledRect(renderer,
                   MakeRect(visual->cursor_x + prefix_width + selected_width, visual->text_y - 1.0f,
                            1.5f, text_renderer_.LineHeight()),
                   theme_.accent);
  };
  const auto draw_vertical_scrollbar =
      [&](const SDL_FRect& area, float total_units, float visible_units, float scroll_units,
          bool active = false, bool reserve_horizontal = false) {
        if (const auto geometry = MakeVerticalScrollbarGeometry(area, total_units, visible_units,
                                                                scroll_units, reserve_horizontal);
            geometry.has_value()) {
          DrawScrollbar(renderer, theme_, geometry->track, geometry->thumb, active);
        }
      };
  const auto draw_horizontal_scrollbar =
      [&](const SDL_FRect& area, float total_units, float visible_units, float scroll_units,
          bool active = false, bool reserve_vertical = false) {
        if (const auto geometry = MakeHorizontalScrollbarGeometry(area, total_units, visible_units,
                                                                  scroll_units, reserve_vertical);
            geometry.has_value()) {
          DrawScrollbar(renderer, theme_, geometry->track, geometry->thumb, active);
        }
      };
  const auto terminal_styles_equal = [](const terminal::TerminalStyle& lhs,
                                        const terminal::TerminalStyle& rhs) {
    const auto colors_equal = [](const std::optional<SDL_Color>& left,
                                 const std::optional<SDL_Color>& right) {
      return left.has_value() == right.has_value() &&
             (!left.has_value() ||
              (left->r == right->r && left->g == right->g && left->b == right->b &&
               left->a == right->a));
    };
    return colors_equal(lhs.foreground, rhs.foreground) &&
           colors_equal(lhs.background, rhs.background) && lhs.bold == rhs.bold;
  };
  const auto draw_terminal_line =
      [&](float x, float y, float width, const terminal::TerminalLine& line, std::size_t row_index) {
    if (width <= 0.0f || line.cells.empty()) {
      return;
    }

    const float char_width = std::max(1.0f, text_renderer_.CharWidth());
    const std::size_t max_chars =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(width / char_width)));
    std::size_t drawn_chars = 0;
    std::size_t segment_start = 0;
    std::string segment;
    terminal::TerminalStyle segment_style;
    bool has_segment = false;
    bool segment_selected = false;

    const auto flush_segment = [&]() {
      if (!has_segment || segment.empty()) {
        return;
      }
      const SDL_Color foreground =
          segment_selected ? theme_.text_primary
                           : segment_style.foreground.value_or(theme_.text_muted);
      const SDL_Color background =
          segment_selected ? theme_.row_highlight
                           : segment_style.background.value_or(theme_.surface_background);
      const float segment_x = x + static_cast<float>(segment_start) * char_width;
      text_renderer_.DrawStringOn(renderer, segment_x, y, foreground, background, segment);
      segment.clear();
      has_segment = false;
    };

    for (std::size_t column = 0; column < line.cells.size(); ++column) {
      if (drawn_chars >= max_chars) {
        break;
      }
      const auto& cell = line.cells[column];
      const bool selected = TerminalCellSelected(row_index, column);
      if (!has_segment) {
        segment_start = column;
        segment_style = cell.style;
        segment_selected = selected;
        has_segment = true;
      } else if (!terminal_styles_equal(segment_style, cell.style) ||
                 segment_selected != selected) {
        flush_segment();
        segment_start = column;
        segment_style = cell.style;
        segment_selected = selected;
        has_segment = true;
      }
      segment.push_back(cell.character);
      ++drawn_chars;
    }
    flush_segment();
  };
  const auto draw_centered_text_on = [&](const SDL_FRect& rect,
                                         float left_padding,
                                         SDL_Color foreground,
                                         SDL_Color background,
                                         std::string_view text) {
    const float y = rect.y + std::floor(std::max(0.0f, rect.h - text_renderer_.LineHeight()) * 0.5f);
    draw_text_on(rect.x + left_padding, y, foreground, background, text);
  };

  const auto visible_menu_items = ComputeVisibleMenuBarItems(layout.menu_bar);
  const auto window_buttons = ComputeVisibleWindowControlButtons(layout.menu_bar);
  for (const VisibleMenuBarItem& item : visible_menu_items) {
    const MenuSpec* menu = FindMenuSpec(item.id);
    if (menu == nullptr) {
      continue;
    }
    const SDL_Color background = item.active ? theme_.chrome_active : theme_.chrome_background;
    DrawFilledRect(renderer, item.rect, background);
    if (item.active) {
      DrawFilledRect(renderer,
                     MakeRect(item.rect.x, item.rect.y + item.rect.h - 2.0f, item.rect.w, 2.0f),
                     theme_.accent);
    }
    draw_centered_text_on(item.rect, 10.0f, item.active ? theme_.text_primary : theme_.text_secondary,
                          background, menu->label);
  }

  if (custom_window_chrome_enabled_) {
    const std::string title = "microide";
    const float title_width = text_renderer_.MeasureWidth(title);
    const float left_limit =
        visible_menu_items.empty() ? layout.menu_bar.x + 12.0f
                                   : visible_menu_items.back().rect.x +
                                         visible_menu_items.back().rect.w + 16.0f;
    const float right_limit =
        window_buttons.empty() ? layout.menu_bar.x + layout.menu_bar.w - 12.0f
                               : window_buttons.front().rect.x - 16.0f;
    const float title_x =
        std::floor(layout.menu_bar.x + (layout.menu_bar.w - title_width) * 0.5f);
    if (title_x >= left_limit && title_x + title_width <= right_limit) {
      draw_text_on(title_x, layout.menu_bar.y + 4.0f, theme_.text_muted, theme_.chrome_background,
                   title);
    }
  }

  for (const VisibleWindowControlButton& button : window_buttons) {
    SDL_Color background = button.hovered ? theme_.row_highlight : theme_.chrome_background;
    SDL_Color glyph = button.hovered ? theme_.text_primary : theme_.text_secondary;
    if (button.id == WindowControlButtonId::Close && button.hovered) {
      background = theme_.diff_deleted;
      glyph = theme_.text_primary;
    } else if (button.id == WindowControlButtonId::Maximize && window_maximized_ &&
               !button.hovered) {
      background = theme_.chrome_active;
      glyph = theme_.text_primary;
    }

    DrawFilledRect(renderer, button.rect, background);
    DrawWindowControlGlyph(renderer, button.rect, button.id, glyph, window_maximized_);
  }

  if (ActiveTabIsCompare()) {
    CompareTabState* compare_tab = ActiveCompareTab();
    if (compare_tab != nullptr) {
      const int visible_rows = CompareVisibleRows(layout.editor_surface);
      ClampCompareScrollRow(*compare_tab, visible_rows);
      const SDL_FRect scrollbar_track = MakeRect(
          layout.editor_surface.x + layout.editor_surface.w - kScrollbarThickness - kScrollbarInset,
          layout.editor_surface.y + kScrollbarInset, kScrollbarThickness,
          std::max(0.0f, layout.editor_surface.h - kScrollbarInset * 2.0f));
      const SDL_FRect marker_lane = MakeRect(
          std::max(layout.editor_surface.x,
                   scrollbar_track.x - kCompareMarkerLaneGap - kCompareMarkerLaneWidth),
          scrollbar_track.y, kCompareMarkerLaneWidth, scrollbar_track.h);
      const SDL_FRect marker_inner_lane =
          MakeRect(marker_lane.x + 1.0f, marker_lane.y + 1.0f,
                   std::max(0.0f, marker_lane.w - 2.0f),
                   std::max(0.0f, marker_lane.h - 2.0f));
      DrawFilledRect(renderer, marker_lane, theme_.surface_raised);
      DrawRect(renderer, marker_lane, theme_.border);
      DrawCompareScrollbarMarkers(renderer, theme_, marker_inner_lane, compare_tab->model);
      if (const auto geometry =
              MakeVerticalScrollbarGeometry(layout.editor_surface,
                                            static_cast<float>(compare_tab->model.rows.size()),
                                            static_cast<float>(visible_rows),
                                            static_cast<float>(compare_tab->scroll_row));
          geometry.has_value()) {
        DrawScrollbarTrack(renderer, theme_, geometry->track);
        DrawScrollbarThumb(renderer, theme_, geometry->thumb,
                           drag_target_ == DragTarget::CompareScrollbar);
      }
    }
  } else if (ActiveTabIsMerge()) {
    MergeTabState* merge_tab = ActiveMergeTab();
    if (merge_tab != nullptr) {
      const int visible_rows = MergeVisibleRows(layout.editor_surface);
      ClampMergeScrollRow(*merge_tab, visible_rows);
      draw_vertical_scrollbar(layout.editor_surface,
                              static_cast<float>(merge_tab->display_model.rows.size()),
                              static_cast<float>(visible_rows),
                              static_cast<float>(merge_tab->scroll_row),
                              drag_target_ == DragTarget::CompareScrollbar);
    }
  } else {
    const auto panes = ComputeEditorPaneLayouts(layout.editor_surface);
    auto* editor_tab = ActiveEditorTab();
    for (const EditorPaneLayout& pane : panes) {
      editor::TextViewport* viewport =
          pane.active ? &text_viewport_
                      : (editor_tab != nullptr ? FindEditorView(*editor_tab, pane.leaf_id)
                                               : nullptr);
      if (viewport == nullptr || viewport->is_placeholder()) {
        continue;
      }

      const editor::EditorViewMetrics metrics =
          editor::EditorViewRenderer::ComputeMetrics(text_renderer_, *viewport, pane.rect);
      viewport->SetViewportSize(metrics.visible_rows, metrics.visible_columns);
      const std::size_t total_columns =
          std::max<std::size_t>(viewport->visible_columns(), MaxVisualColumns(*viewport));
      const bool show_vertical = viewport->line_count() > viewport->visible_lines();
      const bool show_horizontal = total_columns > viewport->visible_columns();
      if (show_vertical) {
        draw_vertical_scrollbar(pane.rect, static_cast<float>(viewport->line_count()),
                                static_cast<float>(viewport->visible_lines()),
                                static_cast<float>(viewport->scroll_line()),
                                pane.active && drag_target_ == DragTarget::EditorVerticalScrollbar,
                                show_horizontal);
      }
      if (show_horizontal) {
        draw_horizontal_scrollbar(
            pane.rect, static_cast<float>(total_columns),
            static_cast<float>(viewport->visible_columns()),
            static_cast<float>(viewport->horizontal_scroll()),
            pane.active && drag_target_ == DragTarget::EditorHorizontalScrollbar, show_vertical);
      }
    }
    for (const EditorSplitDividerLayout& divider :
         ComputeEditorSplitDividerLayouts(layout.editor_surface)) {
      const bool divider_active =
          drag_target_ == DragTarget::EditorSplitDivider &&
          divider.divider_index == drag_editor_split_divider_index_ &&
          divider.node_path == drag_editor_split_path_;
      DrawFilledRect(renderer, divider.rect, divider_active ? theme_.accent : theme_.border);
    }
  }

  for (const VisibleProjectTab& tab : ComputeVisibleProjectTabs(layout.project_tab_strip)) {
    DrawFilledRect(renderer, tab.rect, tab.active ? theme_.chrome_active : theme_.surface_raised);
    if (tab.active) {
      DrawFilledRect(renderer, MakeRect(tab.rect.x, tab.rect.y, tab.rect.w, 2.0f), theme_.accent);
    }
    draw_text_on(tab.rect.x + 10.0f, tab.rect.y + 5.0f,
                 tab.active ? theme_.text_primary : theme_.text_secondary,
                 tab.active ? theme_.chrome_active : theme_.surface_raised,
                 TruncateLabel(ProjectTabDisplayTitle(tab.index), tab.rect.w - 46.0f));
    draw_text_on(tab.close_rect.x + 3.0f, tab.close_rect.y + 1.0f,
                 tab.active ? theme_.text_secondary : theme_.text_disabled,
                 tab.active ? theme_.chrome_active : theme_.surface_raised, "x");
  }

  if (!project_root_.empty() && open_tabs_.empty()) {
    const SDL_FRect placeholder_tab =
        MakeRect(layout.tab_strip.x + 12.0f, layout.tab_strip.y + 5.0f, 220.0f, 24.0f);
    DrawFilledRect(renderer, placeholder_tab, theme_.chrome_active);
    DrawFilledRect(renderer, MakeRect(placeholder_tab.x, placeholder_tab.y, placeholder_tab.w, 2.0f),
                   theme_.accent);
    draw_text_on(placeholder_tab.x + 10.0f, placeholder_tab.y + 6.0f, theme_.text_primary,
                 theme_.chrome_active, "welcome");
  } else if (!project_root_.empty()) {
    for (const VisibleTab& tab : ComputeVisibleTabs(layout.tab_strip)) {
      DrawFilledRect(renderer, tab.rect, tab.active ? theme_.chrome_active : theme_.surface_raised);
      if (tab.active) {
        DrawFilledRect(renderer, MakeRect(tab.rect.x, tab.rect.y, tab.rect.w, 2.0f),
                       theme_.accent);
      }
      const std::string display_title = TabDisplayTitle(tab.index);
      draw_text_on(tab.rect.x + 10.0f, tab.rect.y + 6.0f,
                   tab.active ? theme_.text_primary : theme_.text_secondary,
                   tab.active ? theme_.chrome_active : theme_.surface_raised,
                   TruncateLabel(display_title, tab.rect.w - 46.0f));
      draw_text_on(tab.close_rect.x + 3.0f, tab.close_rect.y + 1.0f,
                   tab.active ? theme_.text_secondary : theme_.text_disabled,
                   tab.active ? theme_.chrome_active : theme_.surface_raised, "x");
    }
  }

  const std::string project_label = ProjectLabel();
  const float project_label_width = text_renderer_.MeasureWidth(project_label);
  if (!project_root_.empty()) {
    draw_text_on(layout.tab_strip.x + layout.tab_strip.w - project_label_width - 16.0f,
                 layout.tab_strip.y + 9.0f, theme_.text_muted, theme_.chrome_background,
                 project_label);
  }

  const float breadcrumb_label_x = layout.breadcrumb.x + 12.0f;
  draw_text_on(breadcrumb_label_x, layout.breadcrumb.y + 7.0f, theme_.text_muted,
               theme_.chrome_background, project_label);
  const float breadcrumb_text_x =
      breadcrumb_label_x + project_label_width + (project_label.empty() ? 0.0f : 14.0f);
  draw_text_on(breadcrumb_text_x, layout.breadcrumb.y + 7.0f, theme_.text_primary,
               theme_.chrome_background,
               TruncateLabel(BreadcrumbLabel(),
                             layout.breadcrumb.w - (breadcrumb_text_x - layout.breadcrumb.x) - 14.0f));

  if (sidebar_visible_) {
    const float text_y_offset = 4.0f;
    const SDL_FRect sidebar_mode_rect = SidebarModeControlRect(layout.sidebar);
    const bool sidebar_mode_hovered =
        last_mouse_position_valid_ && Contains(sidebar_mode_rect, last_mouse_x_, last_mouse_y_);
    const bool sidebar_mode_open =
        menu_bar_open_ && active_menu_id_ == MenuId::SidebarMode && active_menu_anchor_rect_.has_value();
    DrawFilledRect(renderer, sidebar_mode_rect,
                   sidebar_mode_open || sidebar_mode_hovered ? theme_.row_highlight
                                                            : theme_.surface_raised);
    DrawRect(renderer, sidebar_mode_rect,
             sidebar_mode_open ? theme_.accent
                               : sidebar_mode_hovered ? theme_.text_secondary : theme_.border);
    draw_text_on(sidebar_mode_rect.x + 8.0f, sidebar_mode_rect.y + 4.0f,
                 sidebar_mode_open || sidebar_mode_hovered ? theme_.text_primary
                                                           : theme_.text_secondary,
                 sidebar_mode_open || sidebar_mode_hovered ? theme_.row_highlight
                                                           : theme_.surface_raised,
                 SidebarModeControlLabel());
    draw_text_on(sidebar_mode_rect.x + sidebar_mode_rect.w - 12.0f, sidebar_mode_rect.y + 4.0f,
                 sidebar_mode_open || sidebar_mode_hovered ? theme_.text_primary : theme_.text_muted,
                 sidebar_mode_open || sidebar_mode_hovered ? theme_.row_highlight
                                                           : theme_.surface_raised,
                 "v");

    if (sidebar_mode_ == SidebarMode::Search) {
      const std::string active_query =
          project_search_editing_ && project_search_edit_field_ == ProjectSearchEditField::Query
              ? project_search_edit_buffer_
              : project_search_query_;
      const std::string active_replace =
          project_search_editing_ && project_search_edit_field_ == ProjectSearchEditField::Replace
              ? project_search_edit_buffer_
              : project_replace_text_;
      draw_text_on(layout.sidebar.x + kSidebarInset, layout.sidebar.y + 38.0f,
                   project_search_editing_ &&
                           project_search_edit_field_ == ProjectSearchEditField::Query
                       ? theme_.text_primary
                       : theme_.text_secondary,
                   theme_.surface_background,
                   TruncateLabel("search> " + active_query,
                                 layout.sidebar.w - kSidebarInset * 2.0f));
      draw_text_on(layout.sidebar.x + kSidebarInset, layout.sidebar.y + 54.0f,
                   project_search_editing_ &&
                           project_search_edit_field_ == ProjectSearchEditField::Replace
                       ? theme_.text_primary
                       : theme_.text_secondary,
                   theme_.surface_background,
                   TruncateLabel("replace> " + active_replace,
                                 layout.sidebar.w - kSidebarInset * 2.0f));
      const auto draw_search_button = [&](const SDL_FRect& rect,
                                          std::string_view label,
                                          bool active) {
        const bool hovered =
            last_mouse_position_valid_ && Contains(rect, last_mouse_x_, last_mouse_y_);
        const SDL_Color background =
            active ? (hovered ? theme_.row_highlight : theme_.chrome_active)
                   : (hovered ? theme_.row_highlight : theme_.surface_raised);
        const SDL_Color border = active ? theme_.accent : (hovered ? theme_.text_secondary
                                                                   : theme_.border);
        const SDL_Color text = active || hovered ? theme_.text_primary : theme_.text_secondary;
        DrawFilledRect(renderer, rect, background);
        DrawRect(renderer, rect, border);
        draw_centered_text_on(rect, 4.0f, text, background, label);
      };

      draw_search_button(ProjectSearchModeButtonRect(layout.sidebar), ProjectSearchModeButtonLabel(),
                         project_search_options_.pattern_mode ==
                             project::ProjectSearchPatternMode::Regex);
      draw_search_button(ProjectSearchCaseButtonRect(layout.sidebar), ProjectSearchCaseButtonLabel(),
                         project_search_options_.case_mode !=
                             project::ProjectSearchCaseMode::Smart);
      draw_search_button(ProjectSearchHiddenButtonRect(layout.sidebar),
                         ProjectSearchHiddenButtonLabel(), project_search_options_.show_hidden);

      const std::string status_text =
          project_search_editing_
              ? (project_search_edit_field_ == ProjectSearchEditField::Query
                     ? "Editing query  |  Enter apply  Esc cancel"
                     : "Editing replace  |  Enter apply  Esc cancel")
          : !project_search_error_.empty()
              ? "Error  |  / query  = replace  r rerun"
          : project_search_running_
              ? "Searching " + std::to_string(project_search_results_.size()) + " matches"
          : project_search_results_.empty()
              ? (project_search_query_.empty()
                     ? "/ query  = replace  |  buttons change mode, case, hidden"
                     : ProjectSearchCanReplaceAll()
                           ? "No matches  |  / query  = replace  r rerun  R replace all"
                           : "No matches  |  / query  = replace  r rerun  R needs literal mode")
              : std::to_string(project_search_results_.size()) +
                    (ProjectSearchCanReplaceAll()
                         ? " matches  |  / query  = replace  r rerun  R replace all"
                         : " matches  |  / query  = replace  r rerun  R needs literal mode");
      draw_text_on(layout.sidebar.x + kSidebarInset, layout.sidebar.y + kProjectSearchStatusTop,
                   theme_.text_muted,
                   theme_.surface_background,
                   TruncateLabel(status_text, layout.sidebar.w - kSidebarInset * 2.0f));

      const float list_y = layout.sidebar.y + kProjectSearchResultsTop;
      const auto line_map = BuildProjectSearchLineMap();
      const int visible_rows =
          std::max(1, static_cast<int>((layout.sidebar.h - kProjectSearchResultsTop) / kSidebarRowHeight));
      const int max_scroll = std::max(0, static_cast<int>(line_map.size()) - visible_rows);
      const float row_width =
          std::max(0.0f, layout.sidebar.w - kSidebarInset * 2.0f -
                             (max_scroll > 0 ? kScrollbarThickness + 6.0f : 0.0f));
      int scroll_row = std::clamp(sidebar_scroll_row_, 0, max_scroll);
      const int selected_line = ProjectSearchLineForResult(project_search_selected_index_);
      if (selected_line < scroll_row) {
        scroll_row = selected_line;
      } else if (selected_line >= scroll_row + visible_rows) {
        scroll_row = selected_line - visible_rows + 1;
      }
      sidebar_scroll_row_ = scroll_row;

      for (int row = 0; row < visible_rows; ++row) {
        const int line_index = scroll_row + row;
        if (line_index >= static_cast<int>(line_map.size())) {
          break;
        }

        SDL_FRect row_rect = MakeRect(
            layout.sidebar.x + kSidebarInset,
            list_y + static_cast<float>(row) * kSidebarRowHeight,
            row_width,
            kSidebarRowHeight - 2.0f);

        const int result_index = line_map[static_cast<std::size_t>(line_index)];
        if (result_index < 0) {
          const std::size_t next_result_index =
              static_cast<std::size_t>(std::min(line_index + 1, static_cast<int>(line_map.size()) - 1));
          const auto& file_result =
              project_search_results_[static_cast<std::size_t>(line_map[next_result_index])];
          draw_text_on(row_rect.x + 4.0f, row_rect.y + text_y_offset, theme_.text_primary,
                       theme_.surface_background,
                       TruncateLabel(file_result.relative_path.string(), row_rect.w - 8.0f));
          continue;
        }

        const auto& result = project_search_results_[static_cast<std::size_t>(result_index)];
        const bool selected = static_cast<std::size_t>(result_index) == project_search_selected_index_;
        if (selected) {
          DrawFilledRect(renderer, row_rect, theme_.row_highlight);
        }

        const std::string snippet = CollapseWhitespace(result.preview);
        const std::string label =
            std::to_string(result.line + 1) + ":" + std::to_string(result.column + 1) + "  " + snippet;
        draw_text_on(row_rect.x + 6.0f, row_rect.y + text_y_offset,
                     selected ? theme_.text_primary : theme_.text_secondary,
                     selected ? theme_.row_highlight : theme_.surface_background,
                     TruncateLabel(label, row_rect.w - 12.0f));
      }

      if (line_map.empty()) {
        const std::string placeholder = !project_search_error_.empty()
                                            ? "Error: " + project_search_error_
                                        : project_search_running_
                                            ? "Searching..."
                                        : project_search_query_.empty() ? "Project search is idle"
                                                                        : "No matches";
        draw_text_on(layout.sidebar.x + kSidebarInset, list_y + 4.0f, theme_.text_muted,
                     theme_.surface_background,
                     TruncateLabel(placeholder, layout.sidebar.w - kSidebarInset * 2.0f));
      }

      draw_vertical_scrollbar(
          MakeRect(layout.sidebar.x, list_y, layout.sidebar.w,
                   std::max(0.0f, layout.sidebar.y + layout.sidebar.h - list_y)),
          static_cast<float>(line_map.size()), static_cast<float>(visible_rows),
          static_cast<float>(scroll_row), drag_target_ == DragTarget::SidebarScrollbar);
    } else if (sidebar_mode_ == SidebarMode::Git) {
      const SDL_FRect refresh_rect = GitSidebarRefreshButtonRect(layout.sidebar);
      const bool refresh_hovered =
          last_mouse_position_valid_ && Contains(refresh_rect, last_mouse_x_, last_mouse_y_);
      DrawFilledRect(renderer, refresh_rect,
                     refresh_hovered ? theme_.row_highlight : theme_.surface_raised);
      DrawRect(renderer, refresh_rect, refresh_hovered ? theme_.accent : theme_.border);
      draw_centered_text_on(refresh_rect, 8.0f,
                            refresh_hovered ? theme_.text_primary : theme_.accent,
                            refresh_hovered ? theme_.row_highlight : theme_.surface_raised,
                            "Refresh");
      const auto lines = BuildGitSidebarLines();
      const float list_y = layout.sidebar.y + kSidebarHeaderHeight + 6.0f;
      const float visible_units =
          std::max(1.0f, (layout.sidebar.h - 36.0f) / kSidebarRowHeight);
      const int visible_rows = std::max(1, static_cast<int>(std::floor(visible_units)));
      const int max_scroll = std::max(
          0, static_cast<int>(std::ceil(static_cast<float>(lines.size()) - visible_units)));
      const float row_width =
          std::max(0.0f, layout.sidebar.w - kSidebarInset * 2.0f -
                             (max_scroll > 0 ? kScrollbarThickness + 6.0f : 0.0f));
      int scroll_row = std::clamp(sidebar_scroll_row_, 0, max_scroll);
      sidebar_scroll_row_ = scroll_row;

      for (int row = 0; row < visible_rows; ++row) {
        const int line_index = scroll_row + row;
        if (line_index >= static_cast<int>(lines.size())) {
          break;
        }

        const auto& line = lines[static_cast<std::size_t>(line_index)];
        SDL_FRect row_rect = MakeRect(layout.sidebar.x + kSidebarInset,
                                      list_y + static_cast<float>(row) * kSidebarRowHeight, row_width,
                                      kSidebarRowHeight - 2.0f);

        if (line.kind == GitSidebarLine::Kind::Header) {
          draw_text_on(row_rect.x + 4.0f, row_rect.y + text_y_offset, theme_.text_muted,
                       theme_.surface_background, TruncateLabel(line.label, row_rect.w - 8.0f));
          continue;
        }
        if (line.kind == GitSidebarLine::Kind::Empty || line.entry_index < 0) {
          draw_text_on(row_rect.x + 4.0f, row_rect.y + text_y_offset, theme_.text_muted,
                       theme_.surface_background, TruncateLabel(line.label, row_rect.w - 8.0f));
          continue;
        }

        const auto& entry = git_sidebar_entries_[static_cast<std::size_t>(line.entry_index)];
        const bool selected = static_cast<std::size_t>(line.entry_index) == git_sidebar_selected_index_;
        if (selected) {
          DrawFilledRect(renderer, row_rect, theme_.row_highlight);
          DrawFilledRect(renderer, MakeRect(row_rect.x, row_rect.y, 2.0f, row_rect.h),
                         theme_.accent);
        }

        const char git_marker = GitMarker(entry.status);
        const std::string marker_text = git_marker == ' ' ? "" : std::string(1, git_marker);
        const float marker_width = marker_text.empty() ? 0.0f : text_renderer_.MeasureWidth(marker_text);
        float right_edge = row_rect.x + row_rect.w - 8.0f;

        const auto draw_button = [&](const SDL_FRect& button_rect,
                                     std::string_view label,
                                     SDL_Color text_color) {
          DrawRect(renderer, button_rect, selected ? theme_.accent : theme_.border);
          draw_centered_text_on(button_rect, 8.0f, text_color,
                                selected ? theme_.row_highlight : theme_.surface_background, label);
        };

        if (entry.section == GitSidebarEntry::Section::Modified) {
          const std::string_view stage_label = entry.staged ? "Unstage" : "Stage";
          const float stage_width =
              std::max(entry.staged ? 68.0f : 48.0f,
                       text_renderer_.MeasureWidth(stage_label) + 16.0f);
          const SDL_FRect stage_rect =
              MakeRect(right_edge - stage_width, row_rect.y + 1.0f, stage_width, row_rect.h - 2.0f);
          draw_button(stage_rect, stage_label, selected ? theme_.text_primary : theme_.accent);
          right_edge = stage_rect.x - 6.0f;
          const float discard_width =
              std::max(62.0f, text_renderer_.MeasureWidth("Discard") + 16.0f);
          const SDL_FRect discard_rect = MakeRect(right_edge - discard_width, row_rect.y + 1.0f,
                                                  discard_width, row_rect.h - 2.0f);
          draw_button(discard_rect, "Discard",
                      selected ? theme_.text_primary : theme_.diff_deleted);
          right_edge = discard_rect.x - 6.0f;
        }

        if (!marker_text.empty()) {
          draw_text_on(right_edge - marker_width, row_rect.y + text_y_offset,
                       selected ? theme_.text_primary : GitMarkerColor(theme_, entry.status),
                       selected ? theme_.row_highlight : theme_.surface_background, marker_text);
          right_edge -= marker_width + 8.0f;
        }

        const std::string label = entry.relative_path.string() + (entry.staged ? "  [staged]" : "");
        draw_text_on(row_rect.x + 6.0f, row_rect.y + text_y_offset,
                     selected ? theme_.text_primary : theme_.text_secondary,
                     selected ? theme_.row_highlight : theme_.surface_background,
                     TruncateLabel(label, std::max(20.0f, right_edge - row_rect.x - 6.0f)));
      }

      draw_vertical_scrollbar(
          MakeRect(layout.sidebar.x, list_y, layout.sidebar.w,
                   std::max(0.0f, layout.sidebar.y + layout.sidebar.h - list_y)),
          static_cast<float>(lines.size()), visible_units,
          static_cast<float>(scroll_row), drag_target_ == DragTarget::SidebarScrollbar);
    } else {
      const std::string tree_root_label = ProjectLabel();
      const float root_label_width = text_renderer_.MeasureWidth(tree_root_label);
      draw_text_on(layout.sidebar.x + layout.sidebar.w - root_label_width - kSidebarInset,
                   layout.sidebar.y + 8.0f, theme_.text_muted, theme_.chrome_background,
                   tree_root_label);

      const auto& entries = directory_tree_.entries();
      const float list_y = layout.sidebar.y + kSidebarHeaderHeight + 6.0f;
      const int visible_rows =
          std::max(1, static_cast<int>((layout.sidebar.h - 36.0f) / kSidebarRowHeight));
      const int max_scroll = std::max(0, static_cast<int>(entries.size()) - visible_rows);
      const float row_width =
          std::max(0.0f, layout.sidebar.w - kSidebarInset * 2.0f -
                             (max_scroll > 0 ? kScrollbarThickness + 6.0f : 0.0f));
      int scroll_row = std::clamp(sidebar_scroll_row_, 0, max_scroll);
      if (directory_tree_.selected_index() < static_cast<std::size_t>(scroll_row)) {
        scroll_row = static_cast<int>(directory_tree_.selected_index());
      } else if (directory_tree_.selected_index() >=
                 static_cast<std::size_t>(scroll_row + visible_rows)) {
        scroll_row = static_cast<int>(directory_tree_.selected_index()) - visible_rows + 1;
      }
      sidebar_scroll_row_ = scroll_row;

      for (int row = 0; row < visible_rows; ++row) {
        const int entry_index = scroll_row + row;
        if (entry_index >= static_cast<int>(entries.size())) {
          break;
        }

        const auto& entry = entries[entry_index];
        SDL_FRect row_rect = MakeRect(
            layout.sidebar.x + kSidebarInset,
            list_y + static_cast<float>(row) * kSidebarRowHeight,
            row_width,
            kSidebarRowHeight - 2.0f);

        const bool selected = static_cast<std::size_t>(entry_index) == directory_tree_.selected_index();
        if (selected) {
          DrawFilledRect(renderer, row_rect, theme_.row_highlight);
          DrawFilledRect(renderer, MakeRect(row_rect.x, row_rect.y, 2.0f, row_rect.h),
                         theme_.accent);
        }

        const float depth_offset = static_cast<float>(entry.depth) * kTreeIndentWidth;
        const float tree_x = row_rect.x + 6.0f + depth_offset;
        const float chevron_x = tree_x;
        const float label_x = tree_x + kTreeChevronSlotWidth + 4.0f;
        const float chevron_center_y = row_rect.y + row_rect.h * 0.5f;
        const char git_marker = GitMarker(entry.git_status);
        const bool has_git_marker = git_marker != ' ';
        const std::string git_marker_text = has_git_marker ? std::string(1, git_marker) : "";
        const float marker_width =
            has_git_marker ? text_renderer_.MeasureWidth(git_marker_text) : 0.0f;
        const float marker_x = row_rect.x + row_rect.w - marker_width - 8.0f;
        const float label_width =
            has_git_marker ? std::max(20.0f, marker_x - label_x - 8.0f)
                           : std::max(20.0f, row_rect.x + row_rect.w - label_x - 8.0f);

        if (entry.is_directory) {
          DrawChevron(renderer, chevron_x, chevron_center_y, entry.expanded,
                      selected ? theme_.text_primary : theme_.text_muted);
        }

        draw_text_on(label_x, row_rect.y + text_y_offset,
                     selected ? theme_.text_primary
                              : (entry.is_directory ? theme_.text_primary : theme_.text_secondary),
                     selected ? theme_.row_highlight : theme_.surface_background,
                     TruncateLabel(entry.label, label_width));
        if (has_git_marker) {
          draw_text_on(marker_x, row_rect.y + text_y_offset,
                       selected ? theme_.text_primary : GitMarkerColor(theme_, entry.git_status),
                       selected ? theme_.row_highlight : theme_.surface_background,
                       git_marker_text);
        }
      }

      draw_vertical_scrollbar(
          MakeRect(layout.sidebar.x, list_y, layout.sidebar.w,
                   std::max(0.0f, layout.sidebar.y + layout.sidebar.h - list_y)),
          static_cast<float>(entries.size()), static_cast<float>(visible_rows),
          static_cast<float>(scroll_row), drag_target_ == DragTarget::SidebarScrollbar);
    }
  }

  if (overlay_visible_) {
    DrawFilledRect(renderer, layout.editor_area, theme_.overlay_backdrop);
    const SDL_FRect overlay = ComputeOverlayRect(layout.editor_area);
    const SDL_FRect overlay_header = MakeRect(overlay.x, overlay.y, overlay.w, 30.0f);
    DrawFilledRect(renderer, overlay, theme_.overlay_background);
    DrawRect(renderer, overlay, theme_.border);
    DrawFilledRect(renderer, overlay_header, theme_.chrome_background);
    DrawFilledRect(renderer,
                   MakeRect(overlay_header.x, overlay_header.y + overlay_header.h - kDivider,
                            overlay_header.w, kDivider),
                   theme_.border);

    const float overlay_inset = 18.0f;
    const float overlay_row_height = 22.0f;
    ClampOverlayScrollRow(overlay);
    const float overlay_list_y = overlay.y + OverlayListStartOffset();
    const int overlay_visible_rows = OverlayVisibleRows(overlay);
    const int overlay_max_scroll =
        std::max(0, static_cast<int>(OverlayItemCount()) - overlay_visible_rows);
    const float overlay_row_width =
        overlay.w - overlay_inset * 2.0f -
        (overlay_max_scroll > 0 ? kScrollbarThickness + 6.0f : 0.0f);
    const auto draw_overlay_row =
        [&](int row_index, int selected_index, std::string_view label) {
          const bool selected = row_index == selected_index;
          SDL_FRect row = MakeRect(overlay.x + overlay_inset,
                                   overlay_list_y +
                                       static_cast<float>(row_index) * overlay_row_height,
                                   overlay_row_width, 18.0f);
          DrawFilledRect(renderer, row, selected ? theme_.row_highlight : theme_.surface_raised);
          draw_text_on(row.x + 6.0f, row.y + 2.0f,
                       selected ? theme_.text_primary : theme_.text_secondary,
                       selected ? theme_.row_highlight : theme_.surface_raised,
                       TruncateLabel(label, row.w - 12.0f));
        };

    if (overlay_mode_ == OverlayMode::BufferSearch) {
      draw_text_on(overlay.x + overlay_inset, overlay.y + 8.0f, theme_.text_primary,
                   theme_.chrome_background, "Search Buffer");
      draw_text_on(overlay.x + overlay_inset, overlay.y + 44.0f, theme_.text_secondary,
                   theme_.overlay_background, "> " + buffer_search_query_);
      const std::string summary =
          buffer_search_matches_.empty()
              ? "No matches"
              : std::to_string(buffer_search_selected_index_ + 1) + " / " +
                    std::to_string(buffer_search_matches_.size()) + " matches";
      draw_text_on(overlay.x + overlay_inset, overlay.y + 62.0f, theme_.text_muted,
                   theme_.overlay_background, summary);
      for (int row = 0; row < overlay_visible_rows; ++row) {
        const int item_index = overlay_scroll_row_ + row;
        if (item_index >= static_cast<int>(buffer_search_matches_.size())) {
          break;
        }
        const auto& match = buffer_search_matches_[static_cast<std::size_t>(item_index)];
        const std::string label =
            "Ln " + std::to_string(match.start.line + 1) + ", Col " +
            std::to_string(match.start.column + 1) + "  " +
            TruncateLabel(text_viewport_.lines()[match.start.line], overlay.w - 150.0f);
        draw_overlay_row(row, static_cast<int>(buffer_search_selected_index_) - overlay_scroll_row_,
                         label);
      }
    } else if (overlay_mode_ == OverlayMode::BufferReplace) {
      draw_text_on(overlay.x + overlay_inset, overlay.y + 8.0f, theme_.text_primary,
                   theme_.chrome_background, "Replace Buffer");
      draw_text_on(overlay.x + overlay_inset, overlay.y + 44.0f,
                   buffer_search_field_ == BufferSearchField::Search ? theme_.text_primary
                                                                     : theme_.text_secondary,
                   theme_.overlay_background, "find: " + buffer_search_query_);
      draw_text_on(overlay.x + overlay_inset, overlay.y + 62.0f,
                   buffer_search_field_ == BufferSearchField::Replace ? theme_.text_primary
                                                                      : theme_.text_secondary,
                   theme_.overlay_background, "replace: " + buffer_replace_text_);
      const std::string summary =
          buffer_search_matches_.empty()
              ? "No matches"
              : std::to_string(buffer_search_selected_index_ + 1) + " / " +
                    std::to_string(buffer_search_matches_.size()) +
                    " matches  |  Enter replace  Ctrl+Enter replace all";
      draw_text_on(overlay.x + overlay_inset, overlay.y + 82.0f, theme_.text_muted,
                   theme_.overlay_background, TruncateLabel(summary, overlay.w - 36.0f));
      for (int row = 0; row < overlay_visible_rows; ++row) {
        const int item_index = overlay_scroll_row_ + row;
        if (item_index >= static_cast<int>(buffer_search_matches_.size())) {
          break;
        }
        const auto& match = buffer_search_matches_[static_cast<std::size_t>(item_index)];
        const std::string label =
            "Ln " + std::to_string(match.start.line + 1) + ", Col " +
            std::to_string(match.start.column + 1) + "  " +
            TruncateLabel(text_viewport_.lines()[match.start.line], overlay.w - 150.0f);
        draw_overlay_row(row, static_cast<int>(buffer_search_selected_index_) - overlay_scroll_row_,
                         label);
      }
    } else if (overlay_mode_ == OverlayMode::ProjectSearch) {
      draw_text_on(overlay.x + overlay_inset, overlay.y + 8.0f, theme_.text_primary,
                   theme_.chrome_background, "Project Search");
      draw_text_on(overlay.x + overlay_inset, overlay.y + 44.0f, theme_.text_secondary,
                   theme_.overlay_background, "> " + project_search_query_);
      const std::string summary =
          project_search_results_.empty()
              ? "No results"
              : std::to_string(project_search_selected_index_ + 1) + " / " +
                    std::to_string(project_search_results_.size()) + " results";
      draw_text_on(overlay.x + overlay_inset, overlay.y + 62.0f, theme_.text_muted,
                   theme_.overlay_background, summary);
      for (int row = 0; row < overlay_visible_rows; ++row) {
        const int item_index = overlay_scroll_row_ + row;
        if (item_index >= static_cast<int>(project_search_results_.size())) {
          break;
        }
        const auto& result = project_search_results_[static_cast<std::size_t>(item_index)];
        const std::string label =
            result.relative_path.string() + ":" + std::to_string(result.line + 1) + ":" +
            std::to_string(result.column + 1) + "  " +
            TruncateLabel(result.preview, overlay.w - 220.0f);
        draw_overlay_row(row, static_cast<int>(project_search_selected_index_) - overlay_scroll_row_,
                         label);
      }
    } else if (overlay_mode_ == OverlayMode::CommitPicker) {
      draw_text_on(overlay.x + overlay_inset, overlay.y + 8.0f, theme_.text_primary,
                   theme_.chrome_background, "Compare against commit");
      draw_text_on(overlay.x + overlay_inset, overlay.y + 44.0f, theme_.text_muted,
                   theme_.overlay_background, compare_picker_path_.filename().string());
      draw_text_on(overlay.x + overlay_inset, overlay.y + 62.0f, theme_.text_secondary,
                   theme_.overlay_background, "> " + compare_picker_query_);
      for (int row = 0; row < overlay_visible_rows; ++row) {
        const int item_index = overlay_scroll_row_ + row;
        if (item_index >= static_cast<int>(compare_picker_matches_.size())) {
          break;
        }
        const auto& commit = compare_picker_matches_[static_cast<std::size_t>(item_index)];
        draw_overlay_row(row, static_cast<int>(compare_picker_selected_index_) - overlay_scroll_row_,
                         commit.short_hash + "  " + commit.subject);
      }
      if (compare_picker_matches_.empty()) {
        draw_text_on(overlay.x + overlay_inset, overlay.y + 92.0f, theme_.text_muted,
                     theme_.overlay_background, "No matching commits");
      }
    } else {
      draw_text_on(overlay.x + overlay_inset, overlay.y + 8.0f, theme_.text_primary,
                   theme_.chrome_background, "Find File");
      draw_text_on(overlay.x + overlay_inset, overlay.y + 44.0f, theme_.text_secondary,
                   theme_.overlay_background, "> " + file_finder_.query());

      const auto& results = file_finder_.results();
      for (int row = 0; row < overlay_visible_rows; ++row) {
        const int item_index = overlay_scroll_row_ + row;
        if (item_index >= static_cast<int>(results.size())) {
          break;
        }
        draw_overlay_row(row, static_cast<int>(file_finder_.selected_index()) - overlay_scroll_row_,
                         results[static_cast<std::size_t>(item_index)].relative_path.string());
      }
      if (results.empty()) {
        draw_text_on(overlay.x + overlay_inset, overlay.y + 80.0f, theme_.text_muted,
                     theme_.overlay_background, "No matching files");
      }
    }

    draw_vertical_scrollbar(
        MakeRect(overlay.x, overlay_list_y, overlay.w,
                 std::max(0.0f, overlay.y + overlay.h - overlay_list_y - 8.0f)),
        static_cast<float>(OverlayItemCount()), static_cast<float>(overlay_visible_rows),
        static_cast<float>(overlay_scroll_row_), drag_target_ == DragTarget::OverlayScrollbar);
  }

  if (bottom_panel_visible_) {
    const SDL_FRect panel_header =
        MakeRect(layout.bottom_panel.x, layout.bottom_panel.y, layout.bottom_panel.w,
                 kBottomPanelHeaderHeight);
    const bool terminal_panel = BottomPanelShowsTerminal();
    if (terminal_panel) {
      for (const VisibleTerminalTab& tab : ComputeVisibleTerminalTabs(panel_header)) {
        const auto* terminal_tab =
            tab.index < terminal_tabs_.size() ? terminal_tabs_[tab.index].get() : nullptr;
        if (terminal_tab == nullptr) {
          continue;
        }

        const SDL_Color background = tab.active ? theme_.chrome_active : theme_.surface_raised;
        const SDL_Color foreground = tab.active ? theme_.text_primary : theme_.text_secondary;
        DrawFilledRect(renderer, tab.rect, background);
        if (tab.active) {
          DrawFilledRect(renderer, MakeRect(tab.rect.x, tab.rect.y, tab.rect.w, 2.0f),
                         theme_.accent);
        }
        std::string label = terminal_tab->session.LaunchLabel();
        if (label.empty()) {
          label = "terminal";
        }
        draw_text_on(tab.rect.x + 8.0f, tab.rect.y + 4.0f, foreground, background,
                     TruncateLabel(label, tab.rect.w - 40.0f));
        draw_text_on(tab.close_rect.x + 3.0f, tab.close_rect.y + 1.0f, foreground, background,
                     "x");
      }
      const SDL_FRect new_tab_rect = BottomPanelTerminalNewTabRect(panel_header);
      DrawFilledRect(renderer, new_tab_rect, theme_.surface_raised);
      DrawRect(renderer, new_tab_rect, theme_.border);
      draw_text_on(new_tab_rect.x + 5.0f, new_tab_rect.y + 2.0f, theme_.text_secondary,
                   theme_.surface_raised, "+");
    } else {
      std::string panel_header_label = BottomPanelHeaderLabel();
      if (command_mode_) {
        panel_header_label += " | Command";
      }
      draw_text_on(layout.bottom_panel.x + 12.0f, layout.bottom_panel.y + 8.0f,
                   theme_.text_secondary, theme_.chrome_background,
                   TruncateLabel(panel_header_label, layout.bottom_panel.w - 24.0f));
    }

    const SDL_FRect panel_content = BottomPanelContentRect(layout, command_mode_);
    const float logs_y = panel_content.y + 8.0f;
    const std::size_t panel_line_count = terminal_panel ? terminal_lines.size() : log_messages_.size();
    const int visible_rows = BottomPanelVisibleRows(layout.bottom_panel.h);
    const int max_scroll = std::max(0, static_cast<int>(panel_line_count) - visible_rows);
    const int scroll_row = BottomPanelScrollRow(panel_line_count, visible_rows);
    SetBottomPanelScrollRow(scroll_row, panel_line_count, visible_rows);
    const float log_width =
        panel_content.w - 24.0f - (max_scroll > 0 ? kScrollbarThickness + 6.0f : 0.0f);
    for (int row = 0; row < visible_rows; ++row) {
      const int index = scroll_row + row;
      if (index >= static_cast<int>(panel_line_count)) {
        break;
      }
      const float line_y = logs_y + static_cast<float>(row) * text_renderer_.LineHeight();
      if (terminal_panel) {
        draw_terminal_line(panel_content.x + 12.0f, line_y, log_width,
                           terminal_lines[static_cast<std::size_t>(index)],
                           static_cast<std::size_t>(index));
      } else {
        draw_text_on(panel_content.x + 12.0f, line_y, theme_.text_muted,
                     theme_.surface_background,
                     TruncateLabel(log_messages_[static_cast<std::size_t>(index)], log_width));
      }
    }

    if (terminal_panel) {
      if (auto* active_terminal = ActiveTerminalTab(); active_terminal != nullptr &&
                                                   active_terminal->session.cursor_visible()) {
        const std::size_t cursor_row = active_terminal->session.cursor_row();
        const std::size_t cursor_column = active_terminal->session.cursor_column();
        if (cursor_row >= static_cast<std::size_t>(scroll_row) &&
            cursor_row < static_cast<std::size_t>(scroll_row + visible_rows) &&
            (focus_ != FocusTarget::Panel || CaretVisibleNow())) {
          const float char_width = std::max(1.0f, text_renderer_.CharWidth());
          const float cursor_x =
              panel_content.x + 12.0f + static_cast<float>(cursor_column) * char_width;
          const float cursor_y =
              logs_y + static_cast<float>(cursor_row - static_cast<std::size_t>(scroll_row)) *
                           text_renderer_.LineHeight();
          if (cursor_x < panel_content.x + panel_content.w - 2.0f) {
            DrawFilledRect(renderer,
                           MakeRect(cursor_x, cursor_y - 1.0f, 1.5f, text_renderer_.LineHeight()),
                           theme_.cursor);
          }
        }
      }
    }

    if (command_mode_) {
      const SDL_FRect command_area = BottomPanelCommandAreaRect(layout);
      DrawFilledRect(renderer, command_area, theme_.surface_raised);
      DrawFilledRect(renderer, MakeRect(command_area.x, command_area.y, command_area.w, kDivider),
                     theme_.border);

      const float status_y = command_area.y + kBottomPanelCommandTopPadding;
      draw_text_on(command_area.x + 12.0f, status_y, theme_.text_muted, theme_.surface_raised,
                   TruncateLabel(CommandPromptStatusText(), command_area.w - 24.0f));

      SDL_FRect prompt_rect = BottomPanelCommandPromptRect(layout);
      const float prompt_y = prompt_rect.y + 4.0f;
      DrawFilledRect(renderer, prompt_rect, theme_.chrome_active);
      draw_text_on(prompt_rect.x + 6.0f, prompt_y, theme_.text_primary, theme_.chrome_active,
                   "> " + command_input_);
    }

    draw_vertical_scrollbar(
        panel_content,
        static_cast<float>(panel_line_count), static_cast<float>(visible_rows),
        static_cast<float>(scroll_row), drag_target_ == DragTarget::BottomPanelScrollbar);
  }

  if (menu_bar_open_) {
    const auto draw_popup_menu =
        [&](MenuId menu_id, int active_item_index, const std::optional<SDL_FRect>& anchor_rect) {
          const MenuSpec* menu = FindMenuSpec(menu_id);
          if (menu == nullptr) {
            return;
          }
          const auto popup_rect =
              anchor_rect.has_value() ? ActiveSubmenuRect(layout.menu_bar)
                                      : ComputePopupMenuRect(layout.menu_bar, menu_id);
          if (!popup_rect.has_value()) {
            return;
          }

          DrawFilledRect(renderer, *popup_rect, theme_.overlay_background);
          DrawRect(renderer, *popup_rect, theme_.border);
          for (const VisiblePopupMenuItem& item :
               ComputeVisiblePopupMenuItems(menu->items, active_item_index, *popup_rect)) {
            if (item.separator) {
              DrawFilledRect(renderer,
                             MakeRect(item.rect.x + 8.0f, item.rect.y + item.rect.h * 0.5f,
                                      std::max(0.0f, item.rect.w - 16.0f), 1.0f),
                             theme_.border);
              continue;
            }

            const MenuItemSpec& spec = menu->items[item.index];
            const SDL_Color background =
                item.hovered && item.enabled ? theme_.row_highlight : theme_.overlay_background;
            const SDL_Color text_color = !item.enabled ? theme_.text_disabled
                                       : item.hovered ? theme_.text_primary
                                                      : theme_.text_secondary;
            const SDL_Color accel_color = !item.enabled ? theme_.text_disabled : theme_.text_muted;
            DrawFilledRect(renderer, item.rect, background);
            if (item.checked) {
              draw_centered_text_on(item.rect, 8.0f,
                                    item.enabled ? theme_.accent : theme_.text_disabled, background,
                                    "x");
            }
            const std::string accelerator = MenuItemAccelerator(spec);
            const float accelerator_width = text_renderer_.MeasureWidth(accelerator);
            const float label_width = std::max(0.0f, item.rect.w - 42.0f - accelerator_width);
            draw_centered_text_on(
                MakeRect(item.rect.x + 24.0f, item.rect.y, label_width, item.rect.h), 0.0f, text_color,
                background, TruncateLabel(MenuItemLabel(spec), label_width));
            if (!accelerator.empty()) {
              draw_centered_text_on(
                  MakeRect(item.rect.x + item.rect.w - accelerator_width - 10.0f, item.rect.y,
                           accelerator_width, item.rect.h),
                  0.0f, accel_color, background, accelerator);
            }
          }
        };
    draw_popup_menu(active_menu_id_, active_menu_item_index_, std::nullopt);
    if (active_submenu_id_ != MenuId::None) {
      draw_popup_menu(active_submenu_id_, active_submenu_item_index_, active_submenu_anchor_rect_);
    }
  }

  if (tree_context_menu_.open) {
    const auto items = TreeContextMenuItems(tree_context_menu_.target);
    const auto popup_rect = ComputeTreeContextMenuRect();
    if (!items.empty() && popup_rect.has_value()) {
      DrawFilledRect(renderer, *popup_rect, theme_.overlay_background);
      DrawRect(renderer, *popup_rect, theme_.border);
      for (const VisiblePopupMenuItem& item :
           ComputeVisiblePopupMenuItems(items, tree_context_menu_.active_item_index, *popup_rect)) {
        if (item.separator) {
          DrawFilledRect(renderer,
                         MakeRect(item.rect.x + 8.0f, item.rect.y + item.rect.h * 0.5f,
                                  std::max(0.0f, item.rect.w - 16.0f), 1.0f),
                         theme_.border);
          continue;
        }

        const MenuItemSpec& spec = items[item.index];
        const SDL_Color background =
            item.hovered && item.enabled ? theme_.row_highlight : theme_.overlay_background;
        const SDL_Color text_color = !item.enabled ? theme_.text_disabled
                                   : item.hovered ? theme_.text_primary
                                                  : theme_.text_secondary;
        const SDL_Color accel_color = !item.enabled ? theme_.text_disabled : theme_.text_muted;
        DrawFilledRect(renderer, item.rect, background);
        if (item.checked) {
          draw_text_on(item.rect.x + 8.0f, item.rect.y + 3.0f,
                       item.enabled ? theme_.accent : theme_.text_disabled, background, "x");
        }
        const std::string accelerator = MenuItemAccelerator(spec);
        const float accelerator_width = text_renderer_.MeasureWidth(accelerator);
        const float label_width =
            std::max(0.0f, item.rect.w - 42.0f - accelerator_width);
        draw_text_on(item.rect.x + 24.0f, item.rect.y + 3.0f, text_color, background,
                     TruncateLabel(MenuItemLabel(spec), label_width));
        if (!accelerator.empty()) {
          draw_text_on(item.rect.x + item.rect.w - accelerator_width - 10.0f, item.rect.y + 3.0f,
                       accel_color, background, accelerator);
        }
      }
    }
  }

  const std::string focus_text =
      focus_ == FocusTarget::Sidebar ? "sidebar"
      : focus_ == FocusTarget::Panel ? "panel"
      : focus_ == FocusTarget::Overlay ? "overlay"
                                       : "editor";
  const std::string sidebar_text =
      sidebar_mode_ == SidebarMode::Search ? (sidebar_temporary_ ? "search*" : "search")
      : sidebar_mode_ == SidebarMode::Git ? "git"
      : sidebar_mode_ == SidebarMode::Tree ? "tree"
                                           : "none";
  const std::string left_status =
      ProjectLabel() + "  |  " + std::string(text_renderer_.BackendName()) + "  |  focus " +
      focus_text + "  |  sidebar " + sidebar_text + "  |  scale " + UiScaleLabel(ui_scale_);
  std::string right_status;
  if (ActiveTabIsCompare()) {
    right_status =
        "Compare  |  Row " +
        std::to_string(ActiveCompareTab() == nullptr ? 1
                                                     : ActiveCompareTab()->selected_row + 1);
  } else if (ActiveTabIsMerge()) {
    const MergeTabState* merge_tab = ActiveMergeTab();
    const std::string dirty_prefix =
        merge_tab != nullptr && merge_tab->result_viewport.dirty() ? "* " : "";
    std::string hunk_state = "merge";
    if (merge_tab != nullptr && !merge_tab->model.hunks.empty()) {
      const std::size_t selected_hunk =
          std::min(merge_tab->selected_hunk, merge_tab->model.hunks.size() - 1);
      hunk_state = "Merge  |  Hunk " + std::to_string(selected_hunk + 1) + "/" +
                   std::to_string(merge_tab->model.hunks.size()) + "  |  " +
                   compare::MergeChoiceLabel(merge_tab->model.hunks[selected_hunk].choice);
    } else {
      hunk_state = "Merge  |  clean";
    }
    right_status = dirty_prefix + hunk_state;
  } else {
    const std::string dirty_prefix = text_viewport_.dirty() ? "* " : "";
    right_status =
        dirty_prefix + text_viewport_.EncodingLabel() + "  |  " +
        text_viewport_.LineEndingLabel() + "  |  Ln " +
        std::to_string(text_viewport_.cursor_line() + 1) + ", Col " +
        std::to_string(text_viewport_.cursor_column() + 1);
  }
  const float right_status_width = text_renderer_.MeasureWidth(right_status);
  const float right_status_x = std::max(layout.status_bar.x + 10.0f,
                                        layout.status_bar.x + layout.status_bar.w -
                                            right_status_width - 12.0f);
  const float left_max_width =
      std::max(0.0f, right_status_x - (layout.status_bar.x + 10.0f) - 12.0f);
  draw_text_on(layout.status_bar.x + 10.0f, layout.status_bar.y + 5.0f, theme_.text_secondary,
               theme_.chrome_background, TruncateLabel(left_status, left_max_width));
  draw_text_on(right_status_x, layout.status_bar.y + 5.0f, theme_.text_muted,
               theme_.chrome_background, right_status);

  if (prompt_surface_visible_) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    DrawFilledRect(renderer, layout.full, SDL_Color{0x05, 0x07, 0x0b, 0xcc});

    const SDL_FRect dialog = ComputePromptSurfaceRect(layout.full);
    const SDL_FRect header = MakeRect(dialog.x, dialog.y, dialog.w, 32.0f);
    const SDL_FRect message_rect = MakeRect(dialog.x + 16.0f, dialog.y + 50.0f, dialog.w - 32.0f, 36.0f);
    DrawFilledRect(renderer, dialog, theme_.overlay_background);
    DrawRect(renderer, dialog, theme_.border);
    DrawFilledRect(renderer, header, theme_.chrome_background);
    DrawFilledRect(renderer, MakeRect(header.x, header.y + header.h - 1.0f, header.w, 1.0f),
                   theme_.border);
    draw_text_on(header.x + 16.0f, header.y + 8.0f, theme_.text_primary, theme_.chrome_background,
                 PromptSurfaceTitle());
    draw_text_on(message_rect.x, message_rect.y, theme_.text_muted, theme_.overlay_background,
                 TruncateLabel(PromptSurfaceMessage(), message_rect.w));

    if (prompt_surface_state_.kind == PromptSurfaceState::Kind::TextInput) {
      const SDL_FRect input_rect = ComputePromptSurfaceInputRect(dialog);
      DrawFilledRect(renderer, input_rect, theme_.surface_background);
      DrawRect(renderer, input_rect, theme_.border);
      draw_text_on(input_rect.x + 6.0f, input_rect.y + 4.0f, theme_.text_primary,
                   theme_.surface_background,
                   TruncateLabel(prompt_surface_state_.input, input_rect.w - 12.0f));
    }

    const auto buttons = ComputePromptSurfaceButtonRects(dialog);
    const auto labels = PromptSurfaceActionLabels();
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      const bool selected = prompt_surface_state_.selected_button == static_cast<int>(i);
      const SDL_Color background = selected ? theme_.chrome_active : theme_.surface_raised;
      DrawFilledRect(renderer, buttons[i], background);
      DrawRect(renderer, buttons[i], selected ? theme_.accent : theme_.border);
      const float text_width = text_renderer_.MeasureWidth(labels[i]);
      draw_text_on(buttons[i].x + std::floor((buttons[i].w - text_width) * 0.5f),
                   buttons[i].y + 6.0f, theme_.text_primary, background, labels[i]);
    }
  }

  render_text_composition(active_text_input_visual);
  update_text_input_area(active_text_input_visual);

  if (dirty_prompt_visible_) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    DrawFilledRect(renderer, layout.full, SDL_Color{0x05, 0x07, 0x0b, 0xcc});

    const SDL_FRect dialog = ComputeDirtyPromptRect(layout.full);
    const SDL_FRect header = MakeRect(dialog.x, dialog.y, dialog.w, 32.0f);
    const SDL_FRect message_rect = MakeRect(dialog.x + 16.0f, dialog.y + 50.0f, dialog.w - 32.0f, 54.0f);
    DrawFilledRect(renderer, dialog, theme_.overlay_background);
    DrawRect(renderer, dialog, theme_.border);
    DrawFilledRect(renderer, header, theme_.chrome_background);
    DrawFilledRect(renderer,
                   MakeRect(header.x, header.y + header.h - 1.0f, header.w, 1.0f),
                   theme_.border);

    draw_text_on(header.x + 12.0f, header.y + 9.0f, theme_.text_primary, theme_.chrome_background,
                 DirtyPromptTitle());
    draw_text_on(message_rect.x, message_rect.y, theme_.text_secondary, theme_.overlay_background,
                 TruncateLabel(DirtyPromptMessage(), message_rect.w));
    draw_text_on(message_rect.x, message_rect.y + 22.0f, theme_.text_muted, theme_.overlay_background,
                 "Enter confirm  Left/Right choose  Esc cancel");

    const auto buttons = ComputeDirtyPromptButtonRects(dialog);
    const auto labels = DirtyPromptActionLabels();
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      const bool selected = dirty_prompt_state_.selected_action == static_cast<int>(i);
      DrawFilledRect(renderer, buttons[i],
                     selected ? theme_.chrome_active : theme_.surface_raised);
      DrawRect(renderer, buttons[i], selected ? theme_.accent : theme_.border);
      draw_text_on(buttons[i].x + 12.0f, buttons[i].y + 7.0f,
                   selected ? theme_.text_primary : theme_.text_secondary,
                   selected ? theme_.chrome_active : theme_.surface_raised, labels[i]);
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
  }
}


}  // namespace microide::workspace
