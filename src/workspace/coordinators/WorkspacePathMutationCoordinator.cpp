#include "workspace/coordinators/WorkspacePathMutationCoordinator.h"

#include <filesystem>
#include <utility>

#include "util/Parse.h"
#include "workspace/git/GitSidebarCommandCenter.h"
#include "workspace/services/PromptSurfaceService.h"
#include "workspace/WorkspacePathUtils.h"
#include "workspace/shell/WorkspaceShell.h"

namespace microide::workspace {

PathMutationCoordinator::PathMutationCoordinator(WorkspaceContext& context,
                                                 EditorTabService& editor_tabs,
                                                 PromptSurfaceService& prompt_surfaces,
                                                 Operations operations)
    : context_(context),
      editor_tabs_(editor_tabs),
      prompt_surfaces_(prompt_surfaces),
      operations_(std::move(operations)) {}

ProjectWorkspaceState& PathMutationCoordinator::CurrentProjectState() {
  return context_.current_project_state;
}

const ProjectWorkspaceState& PathMutationCoordinator::CurrentProjectState() const {
  return context_.current_project_state;
}

void PathMutationCoordinator::FocusSidebarIfShown() {
  CurrentProjectState().surface.focus = FocusBackTo(CurrentProjectState(), FocusTarget::Sidebar);
}

void PathMutationCoordinator::ConfirmPromptSurface(DirtyPathResolution resolution) {
  if (!context_.prompts.surface_visible) {
    return;
  }

  const PromptSurfaceState state = context_.prompts.surface;
  if (state.selected_button == 1) {
    if (state.action == PromptSurfaceState::Action::DiscardPatchPreview &&
        operations_.cancel_discard_patch_preview) {
      operations_.cancel_discard_patch_preview();
    }
    prompt_surfaces_.DismissPromptSurface(true);
    return;
  }

  if (state.kind == PromptSurfaceState::Kind::TextInput) {
    if (state.input.text().empty()) {
      return;
    }

    std::filesystem::path typed_path(state.input.text());
    if (typed_path.is_absolute()) {
      return;
    }

    std::filesystem::path destination;
    if (state.action == PromptSurfaceState::Action::RenamePath) {
      if (!ResolveDirtyTabsForPath(state.path, DirtyPromptState::Kind::RenamePath, resolution)) {
        return;
      }
      destination = (state.path.parent_path() / typed_path).lexically_normal();
    } else {
      destination = (state.path / typed_path).lexically_normal();
    }

    if (!PathEqualsOrWithin(destination, CurrentProjectState().root)) {
      return;
    }

    using TreeOp = project::FileWriteGate::TreeOp;
    const TreeOp op{
        .kind = state.action == PromptSurfaceState::Action::CreateFile ? TreeOp::Kind::CreateFile
                : state.action == PromptSurfaceState::Action::CreateDirectory
                    ? TreeOp::Kind::CreateDirectory
                    : TreeOp::Kind::Rename,
        .path = state.action == PromptSurfaceState::Action::RenamePath ? state.path : destination,
        .new_path = destination,
    };
    const project::FileWriteGate::TreeResult result =
        CurrentProjectState().write_gate().ApplyTreeOp(op);

    if (!result.ok) {
      if (operations_.notify) {
        const char* verb = state.action == PromptSurfaceState::Action::CreateFile  ? "create file"
                           : state.action == PromptSurfaceState::Action::CreateDirectory
                               ? "create folder"
                               : "rename";
        std::string message = std::string("Could not ") + verb;
        if (!result.error_message.empty()) {
          message += ": " + result.error_message;
        }
        operations_.notify(NotificationService::Tone::Error, std::move(message));
      }
      return;
    }

    if (context_.prompts.dirty_visible) {
      prompt_surfaces_.DismissDirtyPrompt(false);
    }
    prompt_surfaces_.DismissPromptSurface(false);
    if (state.action == PromptSurfaceState::Action::CreateFile) {
      RefreshProjectViewsAfterMutation(result.resulting_path);
      operations_.open_file(result.resulting_path);
      return;
    }
    if (state.action == PromptSurfaceState::Action::CreateDirectory) {
      RefreshProjectViewsAfterMutation(result.resulting_path);
      FocusSidebarIfShown();
      return;
    }

    FollowRename(state.path, result.resulting_path, resolution != DirtyPathResolution::Discard);
    FocusSidebarIfShown();
    return;
  }

  if (state.action == PromptSurfaceState::Action::DiscardGitChanges) {
    const bool discarded = operations_.discard_all_git_sidebar_entries();
    prompt_surfaces_.DismissPromptSurface(discarded ? false : true);
    if (discarded) {
      FocusSidebarIfShown();
    }
    return;
  }

  if (state.action == PromptSurfaceState::Action::DiscardGitEntry) {
    const auto entry_index = util::ParseSize(state.input.text());
    // Pass the confirmed path so the discard re-validates the (possibly stale)
    // index still points at it — an async refresh may have reordered the entries.
    const bool discarded =
        entry_index.has_value() &&
        operations_.discard_git_sidebar_entry(*entry_index, state.path);
    prompt_surfaces_.DismissPromptSurface(discarded ? false : true);
    if (discarded) {
      FocusSidebarIfShown();
    }
    return;
  }

  if (state.action == PromptSurfaceState::Action::DiscardPatchPreview) {
    const bool discarded =
        operations_.confirm_discard_patch_preview && operations_.confirm_discard_patch_preview();
    prompt_surfaces_.DismissPromptSurface(discarded ? false : true);
    if (discarded) {
      CurrentProjectState().surface.focus = FocusTarget::Editor;
    }
    return;
  }

  if (!ResolveDirtyTabsForPath(state.path, DirtyPromptState::Kind::DeletePath, resolution)) {
    return;
  }

  const project::FileWriteGate::TreeResult result = CurrentProjectState().write_gate().ApplyTreeOp(
      {.kind = project::FileWriteGate::TreeOp::Kind::Trash, .path = state.path});
  if (!result.ok) {
    if (operations_.notify) {
      std::string message = "Could not delete";
      if (!result.error_message.empty()) {
        message += ": " + result.error_message;
      }
      operations_.notify(NotificationService::Tone::Error, std::move(message));
    }
    return;
  }

  const std::filesystem::path parent = state.path.parent_path();
  if (context_.prompts.dirty_visible) {
    prompt_surfaces_.DismissDirtyPrompt(false);
  }
  prompt_surfaces_.DismissPromptSurface(false);
  CloseOpenTabsForPath(state.path);
  ClearDiagnosticsForPath(state.path);
  ClearPluginDecorationsForPath(state.path);
  operations_.clear_editor_blame();
  RefreshProjectViewsAfterMutation(parent);
  FocusSidebarIfShown();
}

void PathMutationCoordinator::FollowRename(const std::filesystem::path& old_path,
                                           const std::filesystem::path& new_path,
                                           bool preserve_unsaved_state) {
  RetargetOpenTabsForRename(old_path, new_path, preserve_unsaved_state);
  RetargetDiagnosticsForRename(old_path, new_path);
  RetargetPluginDecorationsForRename(old_path, new_path);
  operations_.clear_editor_blame();
  RefreshProjectViewsAfterMutation(new_path);
  if (operations_.request_window_redraw) {
    operations_.request_window_redraw();
  }
}

namespace {

// "name copy.ext", then "name copy 2.ext", … -- the first name free in
// `directory` (a folder's "name copy" has no extension to keep).
std::filesystem::path FreeCopyName(const std::filesystem::path& directory, const std::filesystem::path& source) {
  std::error_code error;
  const bool is_directory = std::filesystem::is_directory(std::filesystem::symlink_status(source, error));
  const std::string stem = is_directory ? source.filename().string() : source.stem().string();
  const std::string extension = is_directory ? std::string{} : source.extension().string();
  for (int n = 1;; ++n) {
    const std::string suffix = n == 1 ? " copy" : " copy " + std::to_string(n);
    std::filesystem::path candidate = directory / (stem + suffix + extension);
    if (std::filesystem::symlink_status(candidate, error).type() == std::filesystem::file_type::not_found) {
      return candidate;
    }
  }
}

}  // namespace

std::string PathMutationCoordinator::PastePath(const std::filesystem::path& source,
                                               bool cut,
                                               const std::filesystem::path& directory) {
  const std::filesystem::path from = source.lexically_normal();
  const std::filesystem::path into = directory.lexically_normal();
  // Returned, not notified: each caller reports a refusal once, its own way (the
  // tree action through its rejection, which also answers the control channel; a
  // drop with a toast). Notified here as well, a refused Ctrl+V showed the same
  // sentence twice, as an error and as a warning.
  const auto refuse = [](std::string why) { return why; };
  std::error_code error;
  if (std::filesystem::symlink_status(from, error).type() == std::filesystem::file_type::not_found) {
    return refuse(from.filename().string() + " is no longer there");
  }
  if (!PathEqualsOrWithin(into, CurrentProjectState().root)) {
    return refuse("Paste only into the project");
  }
  std::filesystem::path destination = into / from.filename();
  if (cut && destination == from) {
    return {};  // cut and pasted where it is: nothing moves
  }
  if (cut && PathEqualsOrWithin(into, from)) {
    return refuse("A folder cannot be moved into itself");
  }
  const bool taken =
      std::filesystem::symlink_status(destination, error).type() != std::filesystem::file_type::not_found;
  if (taken && cut) {
    return refuse(from.filename().string() + " already exists in " + into.filename().string());
  }
  if (taken) {
    destination = FreeCopyName(into, from);
  }
  using TreeOp = project::FileWriteGate::TreeOp;
  const project::FileWriteGate::TreeResult result = CurrentProjectState().write_gate().ApplyTreeOp(
      {.kind = cut ? TreeOp::Kind::Rename : TreeOp::Kind::Copy, .path = from, .new_path = destination});
  if (!result.ok) {
    return refuse(std::string(cut ? "Could not move " : "Could not copy ") + from.filename().string() +
                  (result.error_message.empty() ? std::string{} : ": " + result.error_message));
  }
  if (cut) {
    FollowRename(from, result.resulting_path, /*preserve_unsaved_state=*/true);
  } else {
    RefreshProjectViewsAfterMutation(result.resulting_path);
  }
  FocusSidebarIfShown();
  return {};
}

PathMutationCoordinator WorkspaceShell::MakePathMutationCoordinator(EditorTabService& editor_tabs,
                                                                    PromptSurfaceService& prompt_surfaces) {
  return PathMutationCoordinator(
      context_,
      editor_tabs,
      prompt_surfaces,
      PathMutationCoordinator::Operations{
          .open_file = [this](const std::filesystem::path& path) { OpenFile(path); },
          .clear_editor_blame = [this]() { ClearEditorBlame(); },
          .notify =
              [this](NotificationService::Tone tone, std::string message) {
                Notify(tone, std::move(message));
              },
          .discard_all_git_sidebar_entries = [this]() { return DiscardAllGitSidebarEntries(); },
          .discard_git_sidebar_entry =
              [this](std::size_t index, const std::filesystem::path& expected_path) {
                return DiscardGitSidebarEntry(index, expected_path);
              },
          .confirm_discard_patch_preview =
              [this]() { return patch_apply_service_.ConfirmPendingDiscard(); },
          .cancel_discard_patch_preview =
              [this]() { patch_apply_service_.CancelPendingDiscard(); },
          .refresh_project_files = [this]() { RefreshProjectFiles(); },
          .request_automatic_git_sidebar_refresh =
              [this]() { RequestAutomaticGitSidebarRefresh(); },
          .reveal_selected_tree_sidebar_line = [this]() { RevealSelectedTreeSidebarLine(); },
          .refresh_project_search = [this]() { RefreshProjectSearch(); },
          .refresh_problems_sidebar = [this]() { RefreshProblemsSidebar(); },
          .queue_editor_hover_refresh = [this]() { QueueEditorHoverRefresh(); },
          .request_editor_surface_redraw = [this]() { RequestEditorSurfaceRedraw(); },
          .request_window_redraw = [this]() { RequestWindowRedraw(); },
          .apply_editor_preferences =
              [this](editor::TextViewport& viewport) { ApplyEditorPreferences(viewport); },
          .apply_detected_indent_on_open =
              [this](editor::TextViewport& viewport) { ApplyDetectedIndentOnOpen(viewport); },
          .editor_view_path =
              [this](const TabEntry::EditorTabState& editor_state) {
                return EditorViewPath(editor_state);
              },
          .sync_active_editor_tab_metadata = [this]() { SyncActiveEditorTabMetadata(); },
          .build_compare_tab_entry =
              [this](const std::filesystem::path& path, const CompareTabState& compare_state) {
                return BuildCompareTabEntry(path, compare_state);
              },
          .build_merge_tab_entry =
              [this](const std::filesystem::path& base_path,
                     const std::filesystem::path& incoming_path,
                     const std::filesystem::path& current_path,
                     const std::filesystem::path& output_path) {
                return BuildMergeTabEntry(base_path, incoming_path, current_path, output_path);
              },
      });
}

}  // namespace microide::workspace
