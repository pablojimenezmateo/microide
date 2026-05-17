#include "workspace/WorkspaceShell.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/SingleLineEditor.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "workspace/WorkspaceLayout.h"

namespace microide::workspace {

namespace {

constexpr float kSingleLineInputTextPadX = 6.0f;
constexpr float kOverlayInset = 18.0f;
constexpr float kOverlayFieldOuterPad = 12.0f;
constexpr float kOverlayFieldHeight = 18.0f;
constexpr float kSidebarSearchTextPadX = 6.0f;

// Sets caret to byte_offset, then either anchors a selection (extend=true) or
// clears any existing selection (extend=false). Mirrors editor convention so
// shift-click extends from the previous caret and a plain click collapses.
void PlaceCaret(editor::SingleLineEditor& state, std::size_t byte_offset, bool extend) {
  const std::size_t clamped = std::min(byte_offset, state.text().size());
  if (extend) {
    if (!state.selection_anchor().has_value()) {
      state.SetSelectionAnchor(state.caret());
    }
  } else {
    state.SetSelectionAnchor(std::nullopt);
  }
  state.SetCaret(clamped);
}

}  // namespace

std::size_t WorkspaceShell::HitTestSingleLineByteOffset(
    const editor::SingleLineEditor& state,
    std::string_view prefix,
    float available_width,
    float relative_x) const {
  // Recompute view_start the same way ComputeSingleLineViewMetrics does so the
  // returned byte index lines up with what the user actually clicked on,
  // including the horizontally-scrolled case where the leftmost visible
  // codepoint is well past byte 0.
  thread_local std::string full_scratch;
  full_scratch.clear();
  full_scratch.reserve(prefix.size() + state.text().size());
  full_scratch.append(prefix);
  full_scratch.append(state.text());
  const std::string_view full_text = full_scratch;
  const std::size_t prefix_size = prefix.size();
  const std::size_t cursor_byte = std::min(prefix_size + state.caret(), full_text.size());

  struct CharEntry {
    std::size_t start;
    float width;
  };
  thread_local std::vector<CharEntry> before_cursor_scratch;
  std::vector<CharEntry>& before_cursor = before_cursor_scratch;
  before_cursor.clear();
  before_cursor.reserve(64);
  for (std::size_t pos = 0; pos < cursor_byte;) {
    const std::size_t next = util::NextUtf8Boundary(full_text, pos);
    before_cursor.push_back(
        {pos, text_renderer_.MeasureWidth(full_text.substr(pos, next - pos))});
    pos = next;
  }

  float cursor_x = 0.0f;
  std::size_t view_start_idx = before_cursor.size();
  for (auto i = before_cursor.size(); i > 0; --i) {
    if (cursor_x + before_cursor[i - 1].width > available_width) {
      break;
    }
    cursor_x += before_cursor[i - 1].width;
    view_start_idx = i - 1;
  }
  const std::size_t view_start =
      before_cursor.empty() ? 0 : before_cursor[view_start_idx].start;

  const auto to_user_byte = [&](std::size_t full_byte) -> std::size_t {
    return full_byte > prefix_size ? full_byte - prefix_size : 0;
  };

  if (relative_x <= 0.0f) {
    return to_user_byte(view_start);
  }

  float prev_x = 0.0f;
  std::size_t prev_byte = view_start;
  for (std::size_t pos = view_start; pos < full_text.size();) {
    const std::size_t next = util::NextUtf8Boundary(full_text, pos);
    const float char_w = text_renderer_.MeasureWidth(full_text.substr(pos, next - pos));
    const float mid = prev_x + char_w * 0.5f;
    if (relative_x <= mid) {
      return to_user_byte(prev_byte);
    }
    prev_x += char_w;
    prev_byte = next;
    if (prev_x > available_width) {
      return to_user_byte(prev_byte);
    }
    pos = next;
  }
  return to_user_byte(prev_byte);
}

namespace {

std::optional<WorkspaceShell::SingleLineInputHit> FilledHit(
    TextInputSurface surface,
    SDL_FRect frame,
    std::string prefix,
    editor::SingleLineEditor* state) {
  if (state == nullptr) {
    return std::nullopt;
  }
  const float pad = kSingleLineInputTextPadX;
  const float available =
      std::max(1.0f, frame.w - 2.0f * pad);
  WorkspaceShell::SingleLineInputHit hit;
  hit.surface = surface;
  hit.text_rect = SDL_FRect{frame.x + pad, frame.y, available, frame.h};
  hit.available_width = available;
  hit.prefix = std::move(prefix);
  hit.state = state;
  hit.mutable_state = state;
  return hit;
}

}  // namespace

std::optional<WorkspaceShell::SingleLineInputHit> WorkspaceShell::FindSingleLineInputHit(
    const WorkspaceLayout& layout, float x, float y) {
  // Modal prompt (rename / create file / etc).
  if (context_.prompts.surface_visible &&
      context_.prompts.surface.kind == PromptSurfaceState::Kind::TextInput) {
    const SDL_FRect dialog = ComputePromptSurfaceRect(layout.full);
    const SDL_FRect input_rect = ComputePromptSurfaceInputRect(dialog);
    if (Contains(input_rect, x, y)) {
      return FilledHit(TextInputSurface::PromptInput, input_rect, "",
                       &context_.prompts.surface.input);
    }
    return std::nullopt;
  }

  // Modal overlay (file finder, buffer search/replace, project search, commit picker).
  if (context_.current_project_state.overlay.visible) {
    const SDL_FRect overlay = ComputeOverlayRect(layout.editor_area);
    if (!Contains(overlay, x, y)) {
      return std::nullopt;
    }
    const auto overlay_field_rect = [&](float text_y) {
      return MakeRect(overlay.x + kOverlayFieldOuterPad, text_y - 4.0f,
                      std::max(0.0f, overlay.w - 2.0f * kOverlayFieldOuterPad),
                      kOverlayFieldHeight);
    };

    auto& state = context_.current_project_state;
    switch (state.overlay.mode) {
      case OverlayMode::FileFinder: {
        const SDL_FRect r = overlay_field_rect(overlay.y + 44.0f);
        if (Contains(r, x, y)) {
          return FilledHit(TextInputSurface::FileFinder, r, "> ",
                           &state.file_finder.query_state());
        }
        break;
      }
      case OverlayMode::BufferSearch: {
        const SDL_FRect r = overlay_field_rect(overlay.y + 44.0f);
        if (Contains(r, x, y)) {
          return FilledHit(TextInputSurface::BufferSearch, r, "> ",
                           &state.overlay.workflow.buffer_search.query);
        }
        break;
      }
      case OverlayMode::BufferReplace: {
        const SDL_FRect search_r = overlay_field_rect(overlay.y + 44.0f);
        const SDL_FRect replace_r = overlay_field_rect(overlay.y + 62.0f);
        if (Contains(search_r, x, y)) {
          return FilledHit(TextInputSurface::BufferReplaceSearch, search_r, "find: ",
                           &state.overlay.workflow.buffer_search.query);
        }
        if (Contains(replace_r, x, y)) {
          return FilledHit(TextInputSurface::BufferReplaceReplace, replace_r, "replace: ",
                           &state.overlay.workflow.buffer_search.replace_text);
        }
        break;
      }
      case OverlayMode::ProjectSearch: {
        const SDL_FRect r = overlay_field_rect(overlay.y + 44.0f);
        if (Contains(r, x, y)) {
          return FilledHit(TextInputSurface::ProjectSearchOverlay, r, "> ",
                           &state.overlay.workflow.project_search.query);
        }
        break;
      }
      case OverlayMode::CommitPicker: {
        const SDL_FRect r = overlay_field_rect(overlay.y + 62.0f);
        if (Contains(r, x, y)) {
          return FilledHit(TextInputSurface::CommitPicker, r, "> ",
                           &state.overlay.workflow.compare_picker.query);
        }
        break;
      }
      case OverlayMode::Completion:
      case OverlayMode::CodeActions:
        break;
    }
    return std::nullopt;
  }

  // Sidebar search query / replace fields.
  if (context_.current_project_state.sidebar.visible &&
      ActiveSidebarMode() == SidebarMode::Search &&
      Contains(layout.sidebar, x, y)) {
    const SDL_FRect query_rect = ProjectSearchQueryRect(layout.sidebar);
    const SDL_FRect replace_rect = ProjectSearchReplaceRect(layout.sidebar);
    if (Contains(query_rect, x, y)) {
      auto& search = context_.current_project_state.overlay.workflow.project_search;
      // Activate the field if it's not already being edited so the click immediately
      // refers to the live edit_buffer (which the renderer + key handler also use).
      if (!search.editing || search.edit_field != ProjectSearchEditField::Query) {
        BeginProjectSearchEdit(ProjectSearchEditField::Query);
      }
      return FilledHit(TextInputSurface::SidebarSearchQuery, query_rect, "", &search.edit_buffer);
    }
    if (Contains(replace_rect, x, y)) {
      auto& search = context_.current_project_state.overlay.workflow.project_search;
      if (!search.editing || search.edit_field != ProjectSearchEditField::Replace) {
        BeginProjectSearchEdit(ProjectSearchEditField::Replace);
      }
      return FilledHit(TextInputSurface::SidebarSearchReplace, replace_rect, "",
                       &search.edit_buffer);
    }
  }

  // Bottom panel command prompt.
  if (context_.current_project_state.panel.command_mode) {
    const SDL_FRect prompt_rect = BottomPanelCommandPromptRect(layout);
    if (Contains(prompt_rect, x, y)) {
      return FilledHit(TextInputSurface::Command, prompt_rect, "> ",
                       &context_.current_project_state.panel.command.input);
    }
  }

  return std::nullopt;
}

bool WorkspaceShell::HandleSingleLineInputMouseDown(const SDL_Event& event,
                                                     const WorkspaceLayout& layout) {
  if (event.button.button != SDL_BUTTON_LEFT) {
    return false;
  }
  util::PerformanceTrace::Scope perf_scope("WorkspaceShell::HandleSingleLineInputMouseDown");
  auto hit = FindSingleLineInputHit(layout, static_cast<float>(event.button.x),
                                    static_cast<float>(event.button.y));
  if (!hit.has_value() || hit->mutable_state == nullptr) {
    return false;
  }

  const float relative_x =
      static_cast<float>(event.button.x) - hit->text_rect.x;
  const std::size_t byte =
      HitTestSingleLineByteOffset(*hit->state, hit->prefix, hit->available_width, relative_x);

  const SDL_Keymod modifiers = SDL_GetModState();
  const bool shift = (modifiers & SDL_KMOD_SHIFT) != 0;

  if (event.button.clicks >= 3) {
    hit->mutable_state->SelectAll();
  } else if (event.button.clicks == 2) {
    PlaceCaret(*hit->mutable_state, byte, false);
    if (!hit->mutable_state->SelectWordAt(byte)) {
      // No word at this byte — leave the caret in place.
    }
  } else {
    PlaceCaret(*hit->mutable_state, byte, shift);
    context_.interaction_state.drag_target = DragTarget::SingleLineSelection;
    context_.interaction_state.single_line_drag_surface = hit->surface;
  }

  // Focus the surface owning the input so subsequent keystrokes route correctly.
  switch (hit->surface) {
    case TextInputSurface::PromptInput:
      // Prompt is modal; focus already pinned by the prompt service.
      break;
    case TextInputSurface::Command:
      context_.current_project_state.surface.focus = FocusTarget::Panel;
      break;
    case TextInputSurface::FileFinder:
    case TextInputSurface::BufferSearch:
    case TextInputSurface::BufferReplaceSearch:
    case TextInputSurface::BufferReplaceReplace:
    case TextInputSurface::ProjectSearchOverlay:
    case TextInputSurface::CommitPicker:
      context_.current_project_state.surface.focus = FocusTarget::Overlay;
      break;
    case TextInputSurface::SidebarSearchQuery:
    case TextInputSurface::SidebarSearchReplace:
      context_.current_project_state.surface.focus = FocusTarget::Sidebar;
      break;
    case TextInputSurface::None:
    case TextInputSurface::Editor:
    case TextInputSurface::Terminal:
      break;
  }

  ResetCaretBlink();
  return true;
}

bool WorkspaceShell::HandleSingleLineInputDrag(const SDL_Event& event,
                                                const WorkspaceLayout& layout) {
  if (context_.interaction_state.drag_target != DragTarget::SingleLineSelection) {
    return false;
  }
  if ((event.motion.state & SDL_BUTTON_LMASK) == 0) {
    context_.interaction_state.drag_target = DragTarget::None;
    context_.interaction_state.single_line_drag_surface = TextInputSurface::None;
    return false;
  }

  const TextInputSurface surface = context_.interaction_state.single_line_drag_surface;
  // Resolve the input by re-running FindSingleLineInputHit at the original press
  // location is unnecessary — we trust the cached surface. Instead, query the
  // rect again by name so we use up-to-date layout (window resize during drag).
  // Iterate the same dispatch table the press used to find the matching state.
  std::optional<SingleLineInputHit> hit;
  // Sweep candidate centers: cheaper than re-running the full dispatcher because
  // the surface tells us exactly which rect helper to call.
  switch (surface) {
    case TextInputSurface::PromptInput: {
      if (context_.prompts.surface_visible &&
          context_.prompts.surface.kind == PromptSurfaceState::Kind::TextInput) {
        const SDL_FRect dialog = ComputePromptSurfaceRect(layout.full);
        hit = FilledHit(surface, ComputePromptSurfaceInputRect(dialog), "",
                        &context_.prompts.surface.input);
      }
      break;
    }
    case TextInputSurface::Command: {
      if (context_.current_project_state.panel.command_mode) {
        hit = FilledHit(surface, BottomPanelCommandPromptRect(layout), "> ",
                        &context_.current_project_state.panel.command.input);
      }
      break;
    }
    case TextInputSurface::FileFinder:
    case TextInputSurface::BufferSearch:
    case TextInputSurface::BufferReplaceSearch:
    case TextInputSurface::BufferReplaceReplace:
    case TextInputSurface::ProjectSearchOverlay:
    case TextInputSurface::CommitPicker:
    case TextInputSurface::SidebarSearchQuery:
    case TextInputSurface::SidebarSearchReplace: {
      // Re-dispatch via FindSingleLineInputHit at the *current* pointer is wrong
      // (the pointer may have left the rect mid-drag). Re-derive directly from
      // the stored surface using the same layout math the press used.
      const SDL_FRect overlay = ComputeOverlayRect(layout.editor_area);
      const auto overlay_field_rect = [&](float text_y) {
        return MakeRect(overlay.x + kOverlayFieldOuterPad, text_y - 4.0f,
                        std::max(0.0f, overlay.w - 2.0f * kOverlayFieldOuterPad),
                        kOverlayFieldHeight);
      };
      auto& proj = context_.current_project_state;
      switch (surface) {
        case TextInputSurface::FileFinder:
          if (proj.overlay.visible) {
            hit = FilledHit(surface, overlay_field_rect(overlay.y + 44.0f), "> ",
                            &proj.file_finder.query_state());
          }
          break;
        case TextInputSurface::BufferSearch:
          if (proj.overlay.visible) {
            hit = FilledHit(surface, overlay_field_rect(overlay.y + 44.0f), "> ",
                            &proj.overlay.workflow.buffer_search.query);
          }
          break;
        case TextInputSurface::BufferReplaceSearch:
          if (proj.overlay.visible) {
            hit = FilledHit(surface, overlay_field_rect(overlay.y + 44.0f), "find: ",
                            &proj.overlay.workflow.buffer_search.query);
          }
          break;
        case TextInputSurface::BufferReplaceReplace:
          if (proj.overlay.visible) {
            hit = FilledHit(surface, overlay_field_rect(overlay.y + 62.0f), "replace: ",
                            &proj.overlay.workflow.buffer_search.replace_text);
          }
          break;
        case TextInputSurface::ProjectSearchOverlay:
          if (proj.overlay.visible) {
            hit = FilledHit(surface, overlay_field_rect(overlay.y + 44.0f), "> ",
                            &proj.overlay.workflow.project_search.query);
          }
          break;
        case TextInputSurface::CommitPicker:
          if (proj.overlay.visible) {
            hit = FilledHit(surface, overlay_field_rect(overlay.y + 62.0f), "> ",
                            &proj.overlay.workflow.compare_picker.query);
          }
          break;
        case TextInputSurface::SidebarSearchQuery:
          if (proj.sidebar.visible) {
            hit = FilledHit(surface, ProjectSearchQueryRect(layout.sidebar), "",
                            &proj.overlay.workflow.project_search.edit_buffer);
          }
          break;
        case TextInputSurface::SidebarSearchReplace:
          if (proj.sidebar.visible) {
            hit = FilledHit(surface, ProjectSearchReplaceRect(layout.sidebar), "",
                            &proj.overlay.workflow.project_search.edit_buffer);
          }
          break;
        default:
          break;
      }
      break;
    }
    case TextInputSurface::None:
    case TextInputSurface::Editor:
    case TextInputSurface::Terminal:
      break;
  }

  if (!hit.has_value() || hit->mutable_state == nullptr) {
    context_.interaction_state.drag_target = DragTarget::None;
    context_.interaction_state.single_line_drag_surface = TextInputSurface::None;
    return false;
  }

  // Anchor the selection at the original press point if no anchor has been set
  // yet — happens when the initial press placed the caret without selecting.
  if (!hit->mutable_state->selection_anchor().has_value()) {
    hit->mutable_state->SetSelectionAnchor(hit->mutable_state->caret());
  }

  const float relative_x = static_cast<float>(event.motion.x) - hit->text_rect.x;
  const std::size_t byte =
      HitTestSingleLineByteOffset(*hit->state, hit->prefix, hit->available_width, relative_x);
  hit->mutable_state->SetCaret(byte);
  ResetCaretBlink();
  return true;
}

}  // namespace microide::workspace
