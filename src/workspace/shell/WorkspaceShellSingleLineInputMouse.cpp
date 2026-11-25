#include "workspace/shell/WorkspaceShell.h"

#include "workspace/ProjectSearchPanelLayout.h"

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
constexpr float kOverlayFieldOuterPad = 12.0f;
constexpr float kOverlayFieldHeight = 18.0f;

void PlaceCaret(editor::SingleLineEditor& state, std::size_t byte_offset, bool extend) {
  const std::size_t clamped = std::min(byte_offset, state.text().size());
  if (extend) {
    state.ExtendSelectionTo(clamped);
    return;
  }
  state.SetSelectionAnchor(std::nullopt);
  state.SetCaret(clamped);
}

}

std::size_t WorkspaceShell::HitTestSingleLineByteOffset(
    const editor::SingleLineEditor& state,
    std::string_view prefix,
    float available_width,
    float relative_x) const {
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
      view_start_idx < before_cursor.size() ? before_cursor[view_start_idx].start : cursor_byte;

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

}

std::optional<WorkspaceShell::SingleLineInputHit> WorkspaceShell::FindSingleLineInputHit(
    const WorkspaceLayout& layout, float x, float y) {
  if (context_.prompts.dirty_visible) {
    return std::nullopt;
  }
  if (context_.prompts.surface_visible) {
    if (context_.prompts.surface.kind == PromptSurfaceState::Kind::TextInput) {
      const SDL_FRect dialog = ComputePromptSurfaceRect(layout.full);
      const SDL_FRect input_rect = ComputePromptSurfaceInputRect(dialog);
      if (Contains(input_rect, x, y)) {
        return FilledHit(TextInputSurface::PromptInput, input_rect, "",
                         &context_.prompts.surface.input);
      }
    }
    return std::nullopt;
  }

  if (context_.current_project_state.overlay.visible) {
    const SDL_FRect overlay = ComputeOverlayRect(layout.overlay_anchor);
    if (!Contains(overlay, x, y)) {
      return std::nullopt;
    }
    const auto overlay_field_rect = [&](float text_y) {
      return MakeRect(overlay.x + kOverlayFieldOuterPad, text_y - 4.0f,
                      std::max(0.0f, overlay.w - 2.0f * kOverlayFieldOuterPad),
                      kOverlayFieldHeight);
    };

    auto& state = context_.current_project_state;
    const float query_row_offset = OverlayQueryRowOffset(state.overlay.mode);
    switch (state.overlay.mode) {
      case OverlayMode::FileFinder: {
        const SDL_FRect r = overlay_field_rect(overlay.y + query_row_offset);
        if (Contains(r, x, y)) {
          return FilledHit(TextInputSurface::FileFinder, r, "> ",
                           &state.file_finder.query_state());
        }
        break;
      }
      case OverlayMode::BufferSearch: {
        const FindWidgetLayout fw = ComputeBufferFindWidgetLayout(ActiveTabSurface(layout), false);
        if (Contains(fw.search_field, x, y)) {
          return FilledHit(TextInputSurface::BufferSearch, fw.search_field, "",
                           &state.overlay.workflow.buffer_search.query);
        }
        break;
      }
      case OverlayMode::BufferReplace: {
        const FindWidgetLayout fw = ComputeBufferFindWidgetLayout(ActiveTabSurface(layout), true);
        if (Contains(fw.search_field, x, y)) {
          return FilledHit(TextInputSurface::BufferReplaceSearch, fw.search_field, "",
                           &state.overlay.workflow.buffer_search.query);
        }
        if (Contains(fw.replace_field, x, y)) {
          return FilledHit(TextInputSurface::BufferReplaceReplace, fw.replace_field, "",
                           &state.overlay.workflow.buffer_search.replace_text);
        }
        break;
      }
      case OverlayMode::ProjectSearch: {
        const SDL_FRect r = overlay_field_rect(overlay.y + query_row_offset);
        if (Contains(r, x, y)) {
          return FilledHit(TextInputSurface::ProjectSearchOverlay, r, "> ",
                           &state.overlay.workflow.project_search.query);
        }
        break;
      }
      case OverlayMode::CommitPicker: {
        const SDL_FRect r = overlay_field_rect(overlay.y + query_row_offset);
        if (Contains(r, x, y)) {
          return FilledHit(TextInputSurface::CommitPicker, r, "> ",
                           &state.overlay.workflow.compare_picker.query);
        }
        break;
      }
      case OverlayMode::QuickPick: {
        const SDL_FRect r = overlay_field_rect(overlay.y + query_row_offset);
        if (Contains(r, x, y)) {
          return FilledHit(TextInputSurface::QuickPick, r, "> ",
                           &state.overlay.workflow.quick_pick.query);
        }
        break;
      }
      case OverlayMode::CommandPalette: {
        const SDL_FRect r = overlay_field_rect(overlay.y + query_row_offset);
        if (Contains(r, x, y)) {
          return FilledHit(TextInputSurface::CommandPalette, r, "> ",
                           &state.overlay.workflow.command_palette.query);
        }
        break;
      }
      case OverlayMode::Completion:
      case OverlayMode::CodeActions:
        break;
    }
    return std::nullopt;
  }

  if (context_.current_project_state.sidebar.visible &&
      ActiveSidebarMode() == SidebarMode::Search &&
      Contains(layout.sidebar, x, y)) {
    auto& search = context_.current_project_state.overlay.workflow.project_search;
    for (const auto& field : project_search_panel::SidebarSearchFieldRects(layout.sidebar, search.scope_expanded)) {
      if (field.rect.w <= 0.0f || !Contains(field.rect, x, y)) {
        continue;
      }
      if (!search.editing || search.edit_field != field.field) {
        BeginProjectSearchEdit(field.field);
      }
      return FilledHit(field.surface, field.rect, "", &search.edit_buffer);
    }
  }

  if (terminal_find_service_.visible() && BottomPanelShowsTerminal()) {
    const FindWidgetLayout fw =
        ComputeTerminalFindWidgetLayout(ActiveTerminalBodyRect(layout), layout);
    if (Contains(fw.search_field, x, y)) {
      return FilledHit(TextInputSurface::TerminalFind, fw.search_field, "",
                       &terminal_find_service_.query());
    }
  }

  if (context_.current_project_state.sidebar.visible && ActiveSidebarMode() == SidebarMode::Git &&
      context_.current_project_state.sidebar.git.commit_workflow.open) {
    auto& workflow = context_.current_project_state.sidebar.git.commit_workflow;
    if (workflow.subject_field_rect.w > 0.0f && Contains(workflow.subject_field_rect, x, y)) {
      workflow.focus_field = CommitWorkflowFocusField::Subject;
      return FilledHit(TextInputSurface::CommitSubject, workflow.subject_field_rect, "",
                       &workflow.subject);
    }
  }


  return std::nullopt;
}

bool WorkspaceShell::HandleSingleLineInputMouseDown(const SDL_Event& event,
                                                     const WorkspaceLayout& layout) {
  if (event.button.button != SDL_BUTTON_LEFT) {
    return false;
  }

  const OverlayState& overlay = context_.current_project_state.overlay;
  const bool modal_up = context_.prompts.surface_visible ||
                        (overlay.visible && OverlayIsModal(overlay.mode));
  if (auto& workflow = context_.current_project_state.sidebar.git.commit_workflow;
      !modal_up && context_.current_project_state.sidebar.visible &&
      ActiveSidebarMode() == SidebarMode::Git && workflow.open &&
      workflow.body_field_rect.w > 0.0f &&
      Contains(workflow.body_field_rect, static_cast<float>(event.button.x),
               static_cast<float>(event.button.y))) {
    workflow.focus_field = CommitWorkflowFocusField::Body;
    context_.current_project_state.surface.focus = FocusTarget::Sidebar;
    editor::TextViewport& body = workflow.body;
    const float line_height = std::max(1.0f, text_renderer_.LineHeight());
    const float char_width = std::max(1.0f, text_renderer_.CharWidth());
    const float text_x = workflow.body_field_rect.x + 6.0f;
    const float top_y = workflow.body_field_rect.y + 3.0f;
    const std::size_t line_count = body.line_count();
    const int row = static_cast<int>((static_cast<float>(event.button.y) - top_y) / line_height);
    const std::size_t line =
        line_count == 0
            ? 0
            : std::min(line_count - 1,
                       body.scroll_line() + static_cast<std::size_t>(std::max(0, row)));
    const std::size_t visual_col = static_cast<std::size_t>(
        std::max(0.0f, (static_cast<float>(event.button.x) - text_x) / char_width) + 0.5f);
    body.MoveCursorToVisualColumn(line, visual_col, false);
    if (event.button.clicks >= 3) {
      body.SelectLineAtCursor();
    } else if (event.button.clicks == 2) {
      body.SelectWordOrRunAtCursor();
    }
    ResetCaretBlink();
    return true;
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

  const SDL_Keymod modifiers = context_.interaction_state.key_modifiers;
  const bool shift = (modifiers & SDL_KMOD_SHIFT) != 0;

  if (event.button.clicks >= 3) {
    hit->mutable_state->SelectAll();
  } else if (event.button.clicks == 2) {
    PlaceCaret(*hit->mutable_state, byte, false);
    if (!hit->mutable_state->SelectWordAt(byte)) {
    }
  } else {
    PlaceCaret(*hit->mutable_state, byte, shift);
    context_.interaction_state.drag_target = DragTarget::SingleLineSelection;
    context_.interaction_state.single_line_drag_surface = hit->surface;
  }

  switch (hit->surface) {
    case TextInputSurface::PromptInput:
      break;
    case TextInputSurface::TerminalFind:
      terminal_find_service_.SetFocused(true);
      context_.current_project_state.surface.focus = FocusTarget::Panel;
      break;
    case TextInputSurface::BufferReplaceSearch:
      context_.current_project_state.overlay.buffer_search_field = BufferSearchField::Search;
      context_.current_project_state.surface.focus = FocusTarget::Overlay;
      break;
    case TextInputSurface::BufferReplaceReplace:
      context_.current_project_state.overlay.buffer_search_field = BufferSearchField::Replace;
      context_.current_project_state.surface.focus = FocusTarget::Overlay;
      break;
    case TextInputSurface::FileFinder:
    case TextInputSurface::BufferSearch:
    case TextInputSurface::ProjectSearchOverlay:
    case TextInputSurface::CommitPicker:
    case TextInputSurface::QuickPick:
    case TextInputSurface::CommandPalette:
      context_.current_project_state.surface.focus = FocusTarget::Overlay;
      break;
    case TextInputSurface::SidebarSearchQuery:
    case TextInputSurface::SidebarSearchReplace:
    case TextInputSurface::SidebarSearchInclude:
    case TextInputSurface::SidebarSearchExclude:
    case TextInputSurface::CommitSubject:
    case TextInputSurface::CommitBody:
      context_.current_project_state.surface.focus = FocusTarget::Sidebar;
      break;
    case TextInputSurface::SettingsQuery:
    case TextInputSurface::SettingsValueEdit:
    case TextInputSurface::DebugVariableEdit:
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
  std::optional<SingleLineInputHit> hit;
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
    case TextInputSurface::FileFinder:
    case TextInputSurface::BufferSearch:
    case TextInputSurface::BufferReplaceSearch:
    case TextInputSurface::BufferReplaceReplace:
    case TextInputSurface::ProjectSearchOverlay:
    case TextInputSurface::CommitPicker:
    case TextInputSurface::QuickPick:
    case TextInputSurface::CommandPalette:
    case TextInputSurface::SidebarSearchQuery:
    case TextInputSurface::SidebarSearchReplace:
    case TextInputSurface::SidebarSearchInclude:
    case TextInputSurface::SidebarSearchExclude:
    case TextInputSurface::TerminalFind: {
      const SDL_FRect overlay = ComputeOverlayRect(layout.overlay_anchor);
      const auto overlay_field_rect = [&](float text_y) {
        return MakeRect(overlay.x + kOverlayFieldOuterPad, text_y - 4.0f,
                        std::max(0.0f, overlay.w - 2.0f * kOverlayFieldOuterPad),
                        kOverlayFieldHeight);
      };
      auto& proj = context_.current_project_state;
      const float query_row_offset = OverlayQueryRowOffset(proj.overlay.mode);
      switch (surface) {
        case TextInputSurface::FileFinder:
          if (proj.overlay.visible) {
            hit = FilledHit(surface, overlay_field_rect(overlay.y + query_row_offset), "> ",
                            &proj.file_finder.query_state());
          }
          break;
        case TextInputSurface::BufferSearch:
          if (proj.overlay.visible) {
            hit = FilledHit(surface, ComputeBufferFindWidgetLayout(ActiveTabSurface(layout), false).search_field,
                            "", &proj.overlay.workflow.buffer_search.query);
          }
          break;
        case TextInputSurface::BufferReplaceSearch:
          if (proj.overlay.visible) {
            hit = FilledHit(surface, ComputeBufferFindWidgetLayout(ActiveTabSurface(layout), true).search_field,
                            "", &proj.overlay.workflow.buffer_search.query);
          }
          break;
        case TextInputSurface::BufferReplaceReplace:
          if (proj.overlay.visible) {
            hit = FilledHit(surface, ComputeBufferFindWidgetLayout(ActiveTabSurface(layout), true).replace_field,
                            "", &proj.overlay.workflow.buffer_search.replace_text);
          }
          break;
        case TextInputSurface::ProjectSearchOverlay:
          if (proj.overlay.visible) {
            hit = FilledHit(surface, overlay_field_rect(overlay.y + query_row_offset), "> ",
                            &proj.overlay.workflow.project_search.query);
          }
          break;
        case TextInputSurface::CommitPicker:
          if (proj.overlay.visible) {
            hit = FilledHit(surface, overlay_field_rect(overlay.y + query_row_offset), "> ",
                            &proj.overlay.workflow.compare_picker.query);
          }
          break;
        case TextInputSurface::QuickPick:
          if (proj.overlay.visible) {
            hit = FilledHit(surface, overlay_field_rect(overlay.y + query_row_offset), "> ",
                            &proj.overlay.workflow.quick_pick.query);
          }
          break;
        case TextInputSurface::CommandPalette:
          if (proj.overlay.visible) {
            hit = FilledHit(surface, overlay_field_rect(overlay.y + query_row_offset), "> ",
                            &proj.overlay.workflow.command_palette.query);
          }
          break;
        case TextInputSurface::SidebarSearchQuery:
        case TextInputSurface::SidebarSearchReplace:
        case TextInputSurface::SidebarSearchInclude:
        case TextInputSurface::SidebarSearchExclude:
          if (proj.sidebar.visible) {
            auto& search = proj.overlay.workflow.project_search;
            for (const auto& field :
                 project_search_panel::SidebarSearchFieldRects(layout.sidebar, search.scope_expanded)) {
              if (field.surface == surface && field.rect.w > 0.0f) {
                hit = FilledHit(surface, field.rect, "", &search.edit_buffer);
                break;
              }
            }
          }
          break;
        default:
          break;
      }
      break;
    }
    case TextInputSurface::CommitSubject: {
      auto& workflow = context_.current_project_state.sidebar.git.commit_workflow;
      if (workflow.open && workflow.subject_field_rect.w > 0.0f) {
        hit = FilledHit(surface, workflow.subject_field_rect, "", &workflow.subject);
      }
      break;
    }
    case TextInputSurface::CommitBody:
    case TextInputSurface::SettingsQuery:
    case TextInputSurface::SettingsValueEdit:
    case TextInputSurface::DebugVariableEdit:
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

  const float relative_x = static_cast<float>(event.motion.x) - hit->text_rect.x;
  const std::size_t byte =
      HitTestSingleLineByteOffset(*hit->state, hit->prefix, hit->available_width, relative_x);
  hit->mutable_state->ExtendSelectionTo(byte);
  ResetCaretBlink();
  return true;
}

}
