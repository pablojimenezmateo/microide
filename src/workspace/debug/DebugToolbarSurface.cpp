#include "workspace/MergeToolbarMetrics.h"
#include "workspace/render/WorkspaceShellRenderPrimitives.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

namespace microide::workspace {

using namespace detail;

namespace {

struct DebugToolbarButtonSpec {
  std::string_view tooltip;
};

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

constexpr bool IsStepButton(DebugToolbarButton button) {
  return button == DebugToolbarButton::StepOver || button == DebugToolbarButton::StepInto ||
         button == DebugToolbarButton::StepOut || button == DebugToolbarButton::ReverseContinue ||
         button == DebugToolbarButton::StepBack;
}

}

std::string_view DebugToolbarButtonTooltip(DebugToolbarButton button, bool session_stopped) {
  if (button == DebugToolbarButton::ContinuePause && !session_stopped) {
    return "Pause";
  }
  if (!session_stopped && IsStepButton(button)) {
    return "Pause to step";
  }
  return kButtonSpecs[Index(button)].tooltip;
}

bool WorkspaceShell::DebugToolbarVisible() const {
  return DebugEnabled() && debug_service_.IsSessionActive();
}

std::optional<float> WorkspaceShell::DebugToolbarAvoidBelowY(const WorkspaceLayout& layout) const {
  std::optional<float> below;
  const OverlayState& overlay = context_.current_project_state.overlay;
  if (overlay.visible &&
      (overlay.mode == OverlayMode::BufferSearch || overlay.mode == OverlayMode::BufferReplace)) {
    const SDL_FRect fw =
        ComputeFindWidgetRect(ActiveTabSurface(layout), overlay.mode == OverlayMode::BufferReplace);
    below = fw.y + fw.h;
  }
  if (const SDL_FRect banner =
          ActiveEditorBannerStripRect(context_.current_project_state, ComputeEditorGroupRectsForState(layout));
      banner.h > 0.0f) {
    below = std::max(below.value_or(banner.y + banner.h), banner.y + banner.h);
  }
  if (const MergeTabState* merge_tab = ActiveTabIsMerge() ? ActiveMergeTab() : nullptr; merge_tab != nullptr) {
    const MergeSurfaceLayout merge = ComputeMergeSurfaceLayout(ActiveTabSurface(layout), *merge_tab);
    const float toolbar_bottom = merge.secondary_button_y + kMergeToolbarButtonHeight;
    below = std::max(below.value_or(toolbar_bottom), toolbar_bottom);
  }
  return below;
}

void WorkspaceShell::RenderDebugToolbar(SDL_Renderer* renderer) const {
  const PreparedFrameState::DebugToolbarPaint& toolbar = frame_state_.debug_toolbar;
  const DebugToolbarLayout& tb = toolbar.layout;
  const bool session_stopped = toolbar.stopped;
  if (renderer == nullptr || !toolbar.visible || tb.widget.w <= 0.0f || tb.widget.h <= 0.0f) {
    return;
  }

  render::DrawCardFrame(renderer, theme_, tb.widget, render::CardStyle::Overlay);

  const auto button_enabled = [&](DebugToolbarButton button) {
    return IsStepButton(button) ? session_stopped : true;
  };

  std::optional<std::size_t> hovered_index;
  std::optional<std::size_t> tooltip_index;
  for (std::size_t i = 0; i < tb.button_count; ++i) {
    const DebugToolbarButton button = tb.kinds[i];
    const SDL_FRect& rect = tb.buttons[i];
    const bool enabled = button_enabled(button);
    const bool pointer_over = toolbar.pointer_over[i];
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

  (void)tooltip_index;
}

bool WorkspaceShell::HandleDebugToolbarButtonDown(const SDL_Event& event,
                                                  const WorkspaceLayout& layout) {
  if (event.button.button != SDL_BUTTON_LEFT || !DebugToolbarVisible()) {
    return false;
  }
  const DebugToolbarLayout tb = ComputeDebugToolbarLayout(
      ActiveTabSurface(layout), DebugToolbarAvoidBelowY(layout), debug_service_.SupportsStepBack());
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
          debug_service_.Continue();
        } else {
          debug_service_.Pause();
        }
        break;
      case DebugToolbarButton::StepOver:
        if (stopped) {
          debug_service_.StepOver();
        }
        break;
      case DebugToolbarButton::StepInto:
        if (stopped) {
          debug_service_.StepIn();
        }
        break;
      case DebugToolbarButton::StepOut:
        if (stopped) {
          debug_service_.StepOut();
        }
        break;
      case DebugToolbarButton::ReverseContinue:
        if (stopped) {
          debug_service_.ReverseContinue();
        }
        break;
      case DebugToolbarButton::StepBack:
        if (stopped) {
          debug_service_.StepBack();
        }
        break;
      case DebugToolbarButton::Restart:
        debug_service_.Restart();
        break;
      case DebugToolbarButton::Stop:
        StopDebugging();
        break;
      default:
        break;
    }
    context_.current_project_state.surface.focus = FocusTarget::Editor;
    RequestPreparedFrame();
    return true;
  }

  return true;
}

}
