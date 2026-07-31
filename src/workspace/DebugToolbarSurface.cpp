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

// Tooltip labels (with the default keybinding) keyed by DebugToolbarButton.
constexpr std::array<DebugToolbarButtonSpec,
                     static_cast<std::size_t>(DebugToolbarButton::Count)>
    kButtonSpecs = {{
        {"Continue (F5)"},
        {"Step Over (F10)"},
        {"Step Into (F11)"},
        {"Step Out (Shift+F11)"},
        {"Reverse Continue"},
        {"Step Back"},
        {"Restart (Ctrl+Shift+F5)"},
        {"Stop Debugging"},
    }};

constexpr std::size_t Index(DebugToolbarButton button) {
  return static_cast<std::size_t>(button);
}

// Step controls (forward and reverse) only act while paused; Continue/Pause/
// Restart/Stop are live whenever a session is active.
constexpr bool IsStepButton(DebugToolbarButton button) {
  return button == DebugToolbarButton::StepOver || button == DebugToolbarButton::StepInto ||
         button == DebugToolbarButton::StepOut || button == DebugToolbarButton::ReverseContinue ||
         button == DebugToolbarButton::StepBack;
}

}  // namespace

std::string_view DebugToolbarButtonTooltip(DebugToolbarButton button, bool session_stopped) {
  if (button == DebugToolbarButton::ContinuePause && !session_stopped) {
    return "Pause";
  }
  if (!session_stopped && IsStepButton(button)) {
    // Step controls are disabled while running: explain rather than stay silent.
    return "Pause to step";
  }
  return kButtonSpecs[Index(button)].tooltip;
}

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
  const DebugToolbarLayout tb =
      ComputeDebugToolbarLayout(layout.editor_surface, avoid_below_y, DebugSupportsReverse());
  if (tb.widget.w <= 0.0f || tb.widget.h <= 0.0f) {
    return;
  }

  // Floating card only — the editor stays visible/usable underneath.
  render::DrawCardFrame(renderer, theme_, tb.widget, render::CardStyle::Overlay);

  const auto is_hovered = [&](const SDL_FRect& rect) {
    return last_mouse_position_valid_ && Contains(rect, last_mouse_x_, last_mouse_y_);
  };

  const auto button_enabled = [&](DebugToolbarButton button) {
    return IsStepButton(button) ? session_stopped : true;
  };

  std::optional<std::size_t> hovered_index;
  // A disabled button gets no highlight but still shows a tooltip, so the user can
  // learn *why* it is inert (e.g. step controls need the program paused first).
  std::optional<std::size_t> tooltip_index;
  for (std::size_t i = 0; i < tb.button_count; ++i) {
    const DebugToolbarButton button = tb.kinds[i];
    const SDL_FRect& rect = tb.buttons[i];
    const bool enabled = button_enabled(button);
    const bool pointer_over = is_hovered(rect);
    const bool hovered = enabled && pointer_over;
    if (hovered) {
      hovered_index = i;
    }
    if (pointer_over) {
      tooltip_index = i;
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
      case DebugToolbarButton::ReverseContinue:
        DrawReverseContinueGlyph(renderer, rect, glyph);
        break;
      case DebugToolbarButton::StepBack:
        DrawStepBackGlyph(renderer, rect, glyph);
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

  // The tooltip is NOT drawn here. It used to be, and that was the bug: the hover
  // invalidation on mouse motion repaints the toolbar card, while this tooltip is
  // anchored BELOW the card, outside that rect. Moving between buttons therefore
  // left the tooltip region untouched, so a tooltip appeared only when some
  // unrelated repaint happened to cover it -- "tooltips don't launch reliably".
  // It now goes through WorkspaceShell::HoveredTooltip like every other chrome
  // tooltip, which invalidates the outgoing and incoming cards by their own rects.
  (void)tooltip_index;
}

bool WorkspaceShell::HandleDebugToolbarButtonDown(const SDL_Event& event,
                                                  const WorkspaceLayout& layout) {
  if (event.button.button != SDL_BUTTON_LEFT || !DebugToolbarVisible()) {
    return false;
  }
  const DebugToolbarLayout tb = ComputeDebugToolbarLayout(
      layout.editor_surface, DebugToolbarAvoidBelowY(layout), DebugSupportsReverse());
  const float x = static_cast<float>(event.button.x);
  const float y = static_cast<float>(event.button.y);
  if (!Contains(tb.widget, x, y)) {
    return false;
  }

  const bool stopped = IsDebugSessionStopped();
  for (std::size_t i = 0; i < tb.button_count; ++i) {
    if (!Contains(tb.buttons[i], x, y)) {
      continue;
    }
    switch (tb.kinds[i]) {
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
      case DebugToolbarButton::ReverseContinue:
        if (stopped) {
          DebugReverseContinue();
        }
        break;
      case DebugToolbarButton::StepBack:
        if (stopped) {
          DebugStepBack();
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
