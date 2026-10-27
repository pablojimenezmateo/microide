#include "workspace/coordinators/WorkspaceKeyInputCoordinator.h"

#include <cstddef>
#include <functional>
#include <vector>

#include "project/CommitWorkflowTypes.h"
#include "workspace/git/GitSidebarCommandCenter.h"
#include "workspace/ListSelection.h"

namespace microide::workspace {
bool KeyInputCoordinator::HandleSidebarKeyDown(const SDL_KeyboardEvent& event,
                                               SDL_Keymod modifiers) {
  const SidebarMode sidebar_mode = operations_.active_sidebar_mode();

  // Shift+F10 and the Menu key open the selected row's context menu, as in VS
  // Code's lists: the keyboard's way to every row command the menu carries
  // (Stage, Discard, Reveal, Copy Path...). Not while a field holds the keyboard.
  const bool field_active =
      (sidebar_mode == SidebarMode::Search && state_.overlay.workflow.project_search.editing) ||
      (sidebar_mode == SidebarMode::Git && state_.sidebar.git.commit_workflow.open);
  const bool context_menu_key =
      event.key == SDLK_APPLICATION || event.key == SDLK_MENU ||
      (event.key == SDLK_F10 && NormalizedKeyModifiers(modifiers) == SDL_KMOD_SHIFT);
  if (context_menu_key && !field_active && operations_.open_sidebar_selection_context_menu) {
    return operations_.open_sidebar_selection_context_menu();
  }

  // Escape closes an auto-opened (temporary) sidebar from *every* mode. Search,
  // Problems, Tests and the plugin/outline list already did; the file tree and
  // the git sidebar silently ignored it, so the one key users reach for to
  // dismiss a transient panel worked in four modes out of six. Sub-editors that
  // own the keyboard (the search field editor, the commit workflow) keep their
  // own Escape below — this only fires when neither is active.
  if (event.key == SDLK_ESCAPE) {
    const bool sub_editor_active =
        (sidebar_mode == SidebarMode::Search &&
         state_.overlay.workflow.project_search.editing) ||
        (sidebar_mode == SidebarMode::Git && state_.sidebar.git.commit_workflow.open);
    if (!sub_editor_active) {
      if (!state_.sidebar.temporary) {
        return false;
      }
      operations_.close_sidebar();
      return true;
    }
  }

  if (sidebar_mode == SidebarMode::Search) {
    const char input_character = operations_.keycode_to_ascii(event.key, modifiers);
    auto& search_state = state_.overlay.workflow.project_search;
    // Fields the panel currently offers, in Tab order. The scope globs are only
    // reachable while the scope section is expanded.
    const auto visible_fields = [&search_state]() {
      std::vector<ProjectSearchEditField> fields{ProjectSearchEditField::Query,
                                                 ProjectSearchEditField::Replace};
      if (search_state.scope_expanded) {
        fields.push_back(ProjectSearchEditField::Include);
        fields.push_back(ProjectSearchEditField::Exclude);
      }
      return fields;
    };

    // Alt+R / Alt+C / Alt+H toggle regex, case sensitivity and hidden-file
    // inclusion — VSCode's search-box chords, in the panel's button order. They
    // were mouse-only (TD-2026-07-30-001). Handled before the editing branch so
    // they work while typing a query as well as from the results list; the
    // toggles flush the in-flight field first, so the re-run search sees the
    // text on screen and the caret stays in the box.
    if ((modifiers & SDL_KMOD_ALT) != 0) {
      const std::function<void()>* toggle =
          event.key == SDLK_R   ? &operations_.toggle_project_search_pattern_mode
          : event.key == SDLK_C ? &operations_.cycle_project_search_case_mode
          : event.key == SDLK_H ? &operations_.toggle_project_search_hidden_files
                                : nullptr;
      if (toggle != nullptr && *toggle != nullptr) {
        (*toggle)();
        return true;
      }
    }

    if (search_state.editing) {
      switch (event.key) {
        case SDLK_ESCAPE:
          operations_.cancel_project_search_edit();
          return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
          // Ctrl+Alt+Enter in the replace field replaces all (VS Code's chord).
          if (search_state.edit_field == ProjectSearchEditField::Replace &&
              (modifiers & SDL_KMOD_CTRL) != 0 && (modifiers & SDL_KMOD_ALT) != 0) {
            operations_.replace_all_project_search_matches();
            return true;
          }
          operations_.commit_project_search_edit();
          return true;
        case SDLK_TAB: {
          // Tab/Shift+Tab walk the visible fields, committing on the way out so a
          // query or glob edit takes effect as the focus leaves it.
          const std::vector<ProjectSearchEditField> fields = visible_fields();
          std::size_t index = 0;
          for (std::size_t i = 0; i < fields.size(); ++i) {
            if (fields[i] == search_state.edit_field) {
              index = i;
              break;
            }
          }
          const std::size_t count = fields.size();
          const std::size_t next = (modifiers & SDL_KMOD_SHIFT) != 0
                                       ? (index + count - 1) % count
                                       : (index + 1) % count;
          operations_.commit_project_search_edit();
          operations_.begin_project_search_edit(fields[next]);
          return true;
        }
        default:
          return operations_.text_input_handle_single_line_key_down(event, modifiers);
      }
    }

    if (const auto delta = ListNavigationKeyDelta(
            event.key, state_.overlay.workflow.project_search.results.size());
        delta.has_value()) {
      operations_.move_project_search_selection(*delta);
      return true;
    }

    switch (event.key) {
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
      case SDLK_RIGHT:
        if (!state_.overlay.workflow.project_search.results.empty() &&
            state_.overlay.workflow.project_search.selected_index <
                state_.overlay.workflow.project_search.results.size()) {
          const auto& result =
              state_.overlay.workflow.project_search.results[state_.overlay.workflow.project_search.selected_index];
          operations_.open_file(state_.root / result.relative_path);
          if (editor::TextViewport* viewport = operations_.active_editor_viewport();
              viewport != nullptr) {
            viewport->JumpCursorTo(result.line, result.column);
          }
          if (state_.sidebar.temporary) {
            operations_.restore_previous_sidebar();
          }
          state_.surface.focus = FocusTarget::Editor;
          operations_.seed_buffer_search_from_project_search();
        }
        return true;
      default:
        break;
    }
    // Typing navigates, as in VS Code's search view: the next result whose line
    // starts with what was typed. Bare letters used to be commands here (`r`
    // reran, `R` replaced all, `c` flipped the exact count, `i`/`x`/`=`/`/`
    // opened fields, `j`/`k` moved), so a query typed into the list by mistake ran
    // them one letter at a time (TD-2026-10-16-350). They are commands now
    // (Search: Refresh / Toggle Exact Match Count / Toggle Search Details, Replace
    // in Files on Ctrl+Shift+H, Find in Project on Ctrl+Shift+F) and buttons.
    if ((modifiers & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0 && input_character > ' ' &&
        input_character < 127) {
      const auto& results = search_state.results;
      const auto preview = [&results](std::size_t i) {
        std::string_view text = results[i].preview;
        while (!text.empty() && text.front() == ' ') {
          text.remove_prefix(1);
        }
        return text;
      };
      if (const auto index = TypeNavigate(search_state.type_navigation, input_character, event.timestamp,
                                          search_state.selected_index, results.size(), preview);
          index.has_value()) {
        operations_.move_project_search_selection(static_cast<int>(*index) -
                                                  static_cast<int>(search_state.selected_index));
      }
      return true;
    }
    return false;
  }

  if (sidebar_mode == SidebarMode::Git) {
    if (state_.sidebar.git.commit_workflow.open) {
      auto& workflow = state_.sidebar.git.commit_workflow;
      // Ctrl+Enter commits from either field; plain Enter is a newline in the body and a
      // no-op in the single-line subject.
      if ((event.key == SDLK_RETURN || event.key == SDLK_KP_ENTER) &&
          (modifiers & SDL_KMOD_CTRL) != 0 && operations_.request_commit_workflow_commit != nullptr) {
        return operations_.request_commit_workflow_commit();
      }
      switch (event.key) {
        case SDLK_ESCAPE:
          if (operations_.close_commit_workflow != nullptr) {
            operations_.close_commit_workflow();
          }
          return true;
        case SDLK_TAB:
          // Tab moves between subject and body (Shift+Tab always returns to the subject),
          // keeping the panel keyboard-navigable like every other multi-field surface.
          if (operations_.set_commit_workflow_focus_field != nullptr) {
            const CommitWorkflowFocusField next =
                (modifiers & SDL_KMOD_SHIFT) != 0
                    ? CommitWorkflowFocusField::Subject
                    : (workflow.focus_field == CommitWorkflowFocusField::Subject
                           ? CommitWorkflowFocusField::Body
                           : CommitWorkflowFocusField::Subject);
            operations_.set_commit_workflow_focus_field(next);
          }
          return true;
        default:
          break;
      }
      if (workflow.focus_field == CommitWorkflowFocusField::Body) {
        return HandleCommitBodyKeyDown(event, modifiers);
      }
      // The subject is a single-line field: hand navigation/editing keys to the shared
      // single-line handler. Plain character keys return false here and are inserted by the
      // separate SDL_TextInput event; returning early keeps git-action keys (s/c/d/...) from
      // firing while the workflow owns the keyboard.
      return operations_.text_input_handle_single_line_key_down(event, modifiers);
    }
    const std::size_t selected_index = state_.sidebar.git.selected_index;
    if (const auto delta =
            ListNavigationKeyDelta(event.key, state_.sidebar.git.entries.size());
        delta.has_value()) {
      // Home/End used to assign the raw first/last index, which can point at an
      // entry hidden under a collapsed directory. MoveGitSidebarSelection walks
      // visible rows only, so routing through it keeps the highlight on screen.
      operations_.move_git_sidebar_selection(*delta);
      return true;
    }
    switch (event.key) {
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        return operations_.dispatch_git_sidebar_action(GitSidebarActionId::DefaultView,
                                                       selected_index);
      default:
        break;
    }
    // Typing navigates by file name, as in VS Code's source control view. Bare
    // letters used to be commands here (`s` staged, `x` discarded, `d`, `u`,
    // `m`, `c`, `o`, `r`), so a name typed into the list by mistake staged and
    // discarded files one letter at a time (TD-2026-10-16-350). They are on the
    // row's context menu (right-click, Shift+F10, the Menu key), the view's
    // buttons and the palette (Git: Commit…, Git: Refresh).
    if ((modifiers & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0) {
      const char typed = operations_.keycode_to_ascii(event.key, modifiers);
      if (typed > ' ' && typed < 127) {
        if (operations_.type_navigate_git_sidebar) {
          (void)operations_.type_navigate_git_sidebar(typed, event.timestamp);
        }
        return true;
      }
    }
    return false;
  }

  if (sidebar_mode == SidebarMode::Problems) {
    if (const auto delta =
            ListNavigationKeyDelta(event.key, state_.sidebar.problems.entries.size());
        delta.has_value()) {
      operations_.move_problems_sidebar_selection(*delta);
      return true;
    }
    switch (event.key) {
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
      case SDLK_RIGHT:
        return operations_.open_selected_problem_sidebar_item();
      case SDLK_R:
        return operations_.refresh_problems_sidebar();
      default:
        return false;
    }
  }

  if (sidebar_mode == SidebarMode::Tests) {
    if (const auto delta =
            ListNavigationKeyDelta(event.key, state_.sidebar.tests.entries.size());
        delta.has_value()) {
      operations_.move_tests_sidebar_selection(*delta);
      return true;
    }
    switch (event.key) {
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
      case SDLK_RIGHT:
        return operations_.open_selected_test_sidebar_item();
      case SDLK_R: {
        const char input_character = operations_.keycode_to_ascii(event.key, modifiers);
        if (input_character == 'r') {
          return operations_.run_selected_test_sidebar_item();
        }
        return operations_.refresh_tests_sidebar();
      }
      default:
        return false;
    }
  }

  if (UsesPluginItemSurface(sidebar_mode)) {
    const auto* selected_plugin_item =
        state_.sidebar.plugin.selected_index < state_.sidebar.plugin.items.size()
            ? &state_.sidebar.plugin.items[state_.sidebar.plugin.selected_index]
            : nullptr;
    const bool selection_is_collapsible =
        selected_plugin_item != nullptr && selected_plugin_item->collapsible;
    if (const auto delta =
            ListNavigationKeyDelta(event.key, state_.sidebar.plugin.items.size());
        delta.has_value()) {
      operations_.move_plugin_sidebar_selection(*delta);
      return true;
    }
    switch (event.key) {
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        return operations_.open_selected_plugin_sidebar_item();
      case SDLK_RIGHT:
        // Expand a collapsed tree row; otherwise preserve the flat-list behavior
        // where Right opens (confirms) the selection.
        if (selection_is_collapsible && selected_plugin_item->collapsed) {
          return operations_.toggle_plugin_sidebar_item();
        }
        return operations_.open_selected_plugin_sidebar_item();
      case SDLK_LEFT:
        if (selection_is_collapsible && !selected_plugin_item->collapsed) {
          return operations_.toggle_plugin_sidebar_item();
        }
        return false;
      case SDLK_SPACE:
        if (selection_is_collapsible) {
          return operations_.toggle_plugin_sidebar_item();
        }
        return false;
      case SDLK_R:
        return operations_.refresh_plugin_sidebar();
      case SDLK_DELETE:
        return operations_.end_selected_plugin_sidebar_item && operations_.end_selected_plugin_sidebar_item();
      default:
        return false;
    }
  }

  if (const auto delta =
          ListNavigationKeyDelta(event.key, state_.directory_tree.entries().size());
      delta.has_value()) {
    state_.directory_tree.MoveSelection(*delta);
    operations_.reveal_selected_tree_sidebar_line();
    return true;
  }
  // VS Code's explorer clipboard: Ctrl+X / Ctrl+C hold the selected entry, Ctrl+V
  // moves or copies it into the selected folder (a file's folder). Keys of the
  // tree, not bindings, so the editor's and the fields' Ctrl+C/X/V stay theirs.
  if (NormalizedKeyModifiers(modifiers) == SDL_KMOD_CTRL &&
      (event.key == SDLK_X || event.key == SDLK_C || event.key == SDLK_V)) {
    (void)operations_.execute_action(event.key == SDLK_X   ? ActionId::TreeCut
                                     : event.key == SDLK_C ? ActionId::TreeCopy
                                                           : ActionId::TreePaste,
                                     {}, ActionSource::Shortcut);
    return true;
  }
  switch (event.key) {
    // VS Code's explorer keys: F2 renames and Delete deletes the selected entry,
    // through the same prompts as the context menu.
    case SDLK_F2:
      (void)operations_.execute_action(ActionId::RenamePath, {}, ActionSource::Shortcut);
      return true;
    case SDLK_DELETE:
      (void)operations_.execute_action(ActionId::DeletePath, {}, ActionSource::Shortcut);
      return true;
    case SDLK_LEFT:
      state_.directory_tree.CollapseSelection();
      operations_.reveal_selected_tree_sidebar_line();
      return true;
    case SDLK_RIGHT:
      state_.directory_tree.ExpandSelection();
      operations_.reveal_selected_tree_sidebar_line();
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: {
      const auto opened = state_.directory_tree.ActivateSelection();
      operations_.reveal_selected_tree_sidebar_line();
      if (opened.has_value()) {
        operations_.open_file(*opened);
        // Keep keyboard focus on the tree after opening so arrow/Enter
        // navigation continues without a manual refocus.
        state_.surface.focus = FocusTarget::Sidebar;
      }
      return true;
    }
    default:
      break;
  }
  // Typing navigates, as in VS Code's explorer: the next entry whose name starts
  // with what was typed. Bare letters used to be commands here (`r` refreshed, `d`
  // opened the compare picker), so a query typed into the tree by mistake ran them
  // one letter at a time (TD-2026-10-15-342); Refresh and Compare are on the
  // tree's context menu, its header button and the palette.
  if ((modifiers & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0) {
    const char typed = operations_.keycode_to_ascii(event.key, modifiers);
    if (typed > ' ' && typed < 127) {
      const auto& entries = state_.directory_tree.entries();
      if (const auto index = TypeNavigate(
              state_.sidebar.tree_type_navigation, typed, event.timestamp,
              state_.directory_tree.selected_index(), entries.size(),
              [&entries](std::size_t i) { return std::string_view(entries[i].label); });
          index.has_value()) {
        state_.directory_tree.SetSelectedIndex(*index);
        operations_.reveal_selected_tree_sidebar_line();
      }
      return true;
    }
  }
  return false;
}


}  // namespace microide::workspace
