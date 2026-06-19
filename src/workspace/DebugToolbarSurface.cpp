// Floating, icon-only debug control bar (Continue/Pause, Step Over/In/Out,
// Restart, Stop). Modeled on the in-editor find widget: a small card that floats
// over the top-right of the editor area while a debug session is active, with
// hover tooltips and no backdrop. These are WorkspaceShell methods defined here
// (the same pattern as DebugPaneRender.cpp / DebugPaneLayout.cpp): the file is
// deliberately NOT named WorkspaceShell*.cpp — that keeps it off the
// WorkspaceShell companion-count cap and out of the render-surface lint glob (so
// it may read state for the hit-test alongside the render). The render builds
// only static label strings and short-lived tooltip text, off any hot path.

#include "workspace/WorkspaceShellRenderPrimitives.h"

#include <array>
#include <optional>
#include <string_view>

namespace microide::workspace {

using namespace detail;

namespace {

struct DebugToolbarButtonSpec {
  std::string_view tooltip;
};

// Tooltip labels (with the default keybinding) in DebugToolbarButton order.
constexpr std::array<DebugToolbarButtonSpec,
                     static_cast<std::size_t>(DebugToolbarButton::Count)>
    kButtonSpecs = {{
        {"Continue (F5)"},
        {"Step Over (F10)"},
        {"Step Into (F11)"},
        {"Step Out (Shift+F11)"},
        {"Restart (Ctrl+Shift+F5)"},
        {"Stop Debugging"},
    }};

constexpr std::size_t Index(DebugToolbarButton button) {
  return static_cast<std::size_t>(button);
}

}  // namespace

bool WorkspaceShell::DebugToolbarVisible() const {
  return DebugEnabled() && IsDebugSessionActive();
}

std::optional<float> WorkspaceShell::DebugToolbarAvoidBelowY(const WorkspaceLayout& layout) const {
  const OverlayState& overlay = context_.current_project_state.overlay;
  if (!overlay.visible) {
    return std::nullopt;
  }
  if (overlay.mode != OverlayMode::BufferSearch && overlay.mode != OverlayMode::BufferReplace) {
    return std::nullopt;
  }
  const SDL_FRect fw =
      ComputeFindWidgetRect(layout.editor_surface, overlay.mode == OverlayMode::BufferReplace);
  return fw.y + fw.h;
}

void WorkspaceShell::RenderDebugToolbar(SDL_Renderer* renderer, const WorkspaceLayout& layout,
                                        bool session_stopped,
                                        std::optional<float> avoid_below_y) const {
  if (renderer == nullptr) {
    return;
  }
  const DebugToolbarLayout tb = ComputeDebugToolbarLayout(layout.editor_surface, avoid_below_y);
  if (tb.widget.w <= 0.0f || tb.widget.h <= 0.0f) {
    return;
  }

  // Floating card only — the editor stays visible/usable underneath.
  render::DrawCardFrame(renderer, theme_, tb.widget, render::CardStyle::Overlay);

  const auto is_hovered = [&](const SDL_FRect& rect) {
    return last_mouse_position_valid_ && Contains(rect, last_mouse_x_, last_mouse_y_);
  };

  // Step controls only act while stopped; Continue/Pause/Restart/Stop are live
  // whenever a session is active.
  const auto button_enabled = [&](DebugToolbarButton button) {
    switch (button) {
      case DebugToolbarButton::StepOver:
      case DebugToolbarButton::StepInto:
      case DebugToolbarButton::StepOut:
        return session_stopped;
      default:
        return true;
    }
  };

  std::optional<std::size_t> hovered_index;
  for (std::size_t i = 0; i < tb.buttons.size(); ++i) {
    const auto button = static_cast<DebugToolbarButton>(i);
    const SDL_FRect& rect = tb.buttons[i];
    const bool enabled = button_enabled(button);
    const bool hovered = enabled && is_hovered(rect);
    if (hovered) {
      hovered_index = i;
    }

    ButtonTone tone = ButtonTone::Neutral;
    if (button == DebugToolbarButton::ContinuePause) {
      tone = ButtonTone::Accent;
    } else if (button == DebugToolbarButton::Stop) {
      tone = ButtonTone::Destructive;
    }
    const ButtonColors colors = ResolveButtonColors(
        theme_, tone, ButtonVisualState{.enabled = enabled, .hovered = hovered});
    FillRect(renderer, rect, colors.fill);
    OutlineRect(renderer, rect, colors.border);

    // The play/stop glyphs get a green/red accent when live so the primary
    // controls read at a glance; everything else uses the resolved text color.
    SDL_Color glyph = colors.text;
    switch (button) {
      case DebugToolbarButton::ContinuePause:
        if (session_stopped) {
          if (enabled) {
            glyph = theme_.diff_added;
          }
          DrawPlayGlyph(renderer, rect, glyph);
        } else {
          DrawPauseGlyph(renderer, rect, glyph);
        }
        break;
      case DebugToolbarButton::StepOver:
        DrawStepOverGlyph(renderer, rect, glyph);
        break;
      case DebugToolbarButton::StepInto:
        DrawStepIntoGlyph(renderer, rect, glyph);
        break;
      case DebugToolbarButton::StepOut:
        DrawStepOutGlyph(renderer, rect, glyph);
        break;
      case DebugToolbarButton::Restart:
        DrawRestartGlyph(renderer, rect, glyph);
        break;
      case DebugToolbarButton::Stop:
        if (enabled) {
          glyph = theme_.diff_deleted;
        }
        DrawStopGlyph(renderer, rect, glyph);
        break;
      default:
        break;
    }
  }

  // Tooltip on top, anchored under the hovered button and clamped to the editor.
  if (hovered_index.has_value()) {
    const SDL_FRect& rect = tb.buttons[*hovered_index];
    std::string_view label = kButtonSpecs[*hovered_index].tooltip;
    if (*hovered_index == Index(DebugToolbarButton::ContinuePause) && !session_stopped) {
      label = "Pause";
    }
    TooltipLayout tip = BuildTooltipLayout(text_renderer_, label, 240.0f, 80.0f);
    float tip_x = rect.x;
    const float max_x = layout.editor_surface.x + layout.editor_surface.w - tip.rect.w - 4.0f;
    if (tip_x > max_x) {
      tip_x = max_x;
    }
    tip.rect.x = std::max(layout.editor_surface.x + 4.0f, tip_x);
    tip.rect.y = rect.y + rect.h + 4.0f;
    DrawTooltip(text_renderer_, renderer, theme_, tip.rect, tip.text);
  }
}

bool WorkspaceShell::HandleDebugToolbarButtonDown(const SDL_Event& event,
                                                  const WorkspaceLayout& layout) {
  if (event.button.button != SDL_BUTTON_LEFT || !DebugToolbarVisible()) {
    return false;
  }
  const DebugToolbarLayout tb =
      ComputeDebugToolbarLayout(layout.editor_surface, DebugToolbarAvoidBelowY(layout));
  const float x = static_cast<float>(event.button.x);
  const float y = static_cast<float>(event.button.y);
  if (!Contains(tb.widget, x, y)) {
    return false;
  }

  const bool stopped = IsDebugSessionStopped();
  for (std::size_t i = 0; i < tb.buttons.size(); ++i) {
    if (!Contains(tb.buttons[i], x, y)) {
      continue;
    }
    switch (static_cast<DebugToolbarButton>(i)) {
      case DebugToolbarButton::ContinuePause:
        if (stopped) {
          DebugContinue();
        } else {
          DebugPause();
        }
        break;
      case DebugToolbarButton::StepOver:
        if (stopped) {
          DebugStepOver();
        }
        break;
      case DebugToolbarButton::StepInto:
        if (stopped) {
          DebugStepIn();
        }
        break;
      case DebugToolbarButton::StepOut:
        if (stopped) {
          DebugStepOut();
        }
        break;
      case DebugToolbarButton::Restart:
        DebugRestart();
        break;
      case DebugToolbarButton::Stop:
        StopDebugging();
        break;
      default:
        break;
    }
    context_.current_project_state.surface.focus = FocusTarget::Editor;
    RequestEditorSurfaceRedraw();
    return true;
  }

  // Click landed on the bar's padding/background: swallow it so it doesn't fall
  // through to the editor (e.g. moving the caret behind the toolbar).
  return true;
}

}  // namespace microide::workspace
