#include "workspace/coordinators/WorkspaceTabCoordinator.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "util/Parse.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "workspace/WorkspacePathUtils.h"

namespace microide::workspace {

bool TabCoordinator::RestoreEditorTab(TabEntry::EditorTabState& editor_state) {
  util::PerformanceTrace::Scope perf_scope("TabCoordinator::RestoreEditorTab");
  if (!editor_state.content_pending()) {
    return true;
  }
  // A read is already in flight, or has already failed. Either way this tab is
  // not hydrated here: forcing a synchronous read now is exactly the shell-thread
  // stall the off-thread open exists to avoid, and retrying a failure on every
  // activation would stall once per click.
  if (editor_state.content == TabEntry::EditorTabState::Content::Loading) {
    return true;
  }
  if (editor_state.content == TabEntry::EditorTabState::Content::Failed) {
    return false;
  }
  if (editor_state.restore.path.empty()) {
    return false;
  }
  if (ShouldOpenOffThread(editor_state.restore.path)) {
    editor::TextViewport loading_view;
    const std::uint64_t read_id = BeginOffThreadOpen(editor_state.restore.path, loading_view);
    if (read_id != 0) {
      editor_state.viewport = std::move(loading_view);
      editor_state.content = TabEntry::EditorTabState::Content::Loading;
      editor_state.pending_load.Arm(read_id, editor_state.viewport.content_revision());
      return true;
    }
  }

  editor::TextViewport loaded_view;
  {
    util::PerformanceTrace::Scope open_scope("TabCoordinator::RestoreEditorTab::OpenFile");
    // A restored session can hold the same file in two panes; the second one to
    // load shares the first's buffer rather than reading the file again.
    // Preferences / indent detection (applied in there for a fresh read)
    // internally re-run EnsureCursorVisible, so they come BEFORE the view-state
    // restore — otherwise they snap scroll back onto the caret (the "reopen
    // lands on line 1 after scrolling" bug).
    if (!OpenEditorViewForPath(editor_state.restore.path, loaded_view)) {
      return false;
    }
  }
  editor_state.restore.ApplyViewStateTo(loaded_view);
  editor_state.viewport = std::move(loaded_view);
  editor_state.content = EditorTabState::Content::Ready;
  return true;
}

void TabCoordinator::RetryFailedOpen(const std::filesystem::path& path) {
  const std::filesystem::path normalized = path.lexically_normal();
  DismissEditorBannerForPath(state_, normalized);
  for (EditorGroup& group : state_.editor_groups) {
    for (TabEntry& tab : group.open_tabs) {
      if (!tab.editor_state.has_value() ||
          tab.editor_state->content != EditorTabState::Content::Failed ||
          tab.editor_state->restore.path.lexically_normal() != normalized) {
        continue;
      }
      EditorTabState& editor_state = *tab.editor_state;
      editor_state.content = EditorTabState::Content::Deferred;
      if (!RestoreEditorTab(editor_state)) {
        editor_state.content = EditorTabState::Content::Failed;
        SetEditorBanner(state_, EditorBannerState::Kind::OpenFailed, normalized,
                        "Could not read " + normalized.filename().string());
      }
    }
  }
}

bool TabCoordinator::EnsureEditorTabLoaded(TabEntry& tab) {
  util::PerformanceTrace::Scope perf_scope("TabCoordinator::EnsureEditorTabLoaded");
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return false;
  }

  auto& editor_state = *tab.editor_state;
  if (editor_state.content_pending() && !RestoreEditorTab(editor_state)) {
    return false;
  }

  tab.path = operations_.editor_view_path(editor_state);
  tab.title = tab.path.empty() ? "untitled" : tab.path.filename().string();
  return true;
}

bool TabCoordinator::LoadEditorTabForActivation(TabEntry& tab) {
  if (tab.kind != TabEntry::Kind::Editor) {
    return true;
  }
  if (tab.editor_state.has_value()) {
    // A deferred tab gets its preferences applied inside RestoreEditorTab, after
    // which view state (scroll) is authoritative. Re-applying preferences here
    // would run EnsureCursorVisible again and snap scroll back onto the caret, so
    // only refresh preferences for tabs that were already loaded.
    const bool was_deferred = tab.editor_state->content_pending();
    if (!EnsureEditorTabLoaded(tab)) {
      return false;
    }
    if (!was_deferred) {
      operations_.apply_editor_preferences(tab.editor_state->viewport);
    }
    return true;
  }
  // Not materialized yet: a session tab that only ever carried its handle, or a
  // bare tab entry naming its path. Both become a Deferred editor state and load
  // through RestoreEditorTab, the one hydration path — which decides on- or
  // off-thread, and shares a buffer another pane already has live (one file is
  // one buffer). This branch used to repeat that logic with a plain OpenFile, so
  // a restored background tab of a file open elsewhere got a second, independent
  // buffer (TD-2026-09-29-314).
  DeferredTabHandle handle = tab.deferred_handle.has_value() ? std::move(*tab.deferred_handle)
                                                             : DeferredTabHandle{.path = tab.path};
  handle.path = handle.path.lexically_normal();
  if (handle.path.empty()) {
    return false;
  }
  tab.deferred_handle.reset();
  tab.editor_state = operations_.make_editor_tab_state(editor::TextViewport{});
  tab.editor_state->content = EditorTabState::Content::Deferred;
  tab.editor_state->restore = std::move(handle);
  return EnsureEditorTabLoaded(tab);
}

std::optional<std::size_t> TabCoordinator::FindIndexBySpecifier(std::string_view specifier,
                                                                std::string* error_message) const {
  if (specifier.empty()) {
    if (error_message != nullptr) {
      *error_message = "usage: tabswitch <tab>";
    }
    return std::nullopt;
  }

  // A signed offset selects relative to the active tab and wraps, so `tabswitch +1`
  // is "next tab" (what Ctrl+PageDown is bound to) and `-1` is "previous". `tabmove`
  // has taken the same +N/-N form all along; tabswitch only accepted an absolute
  // slot or a title, so the two sibling commands read the same argument differently.
  // Move clamps at the ends because sliding a tab past the edge is meaningless;
  // switching wraps, as it does in VS Code.
  const std::size_t tab_count = state_.focused_group().open_tabs.size();
  if ((specifier.front() == '+' || specifier.front() == '-') && specifier.size() > 1) {
    const bool forward = specifier.front() == '+';
    const std::optional<int> magnitude = util::ParseInt(specifier.substr(1));
    if (!magnitude.has_value() || *magnitude < 0) {
      if (error_message != nullptr) {
        *error_message = "Invalid tab offset";
      }
      return std::nullopt;
    }
    if (tab_count == 0) {
      if (error_message != nullptr) {
        *error_message = "No open tabs";
      }
      return std::nullopt;
    }
    const auto count = static_cast<std::int64_t>(tab_count);
    const std::int64_t offset =
        (static_cast<std::int64_t>(*magnitude) % count) * (forward ? 1 : -1);
    const auto active = static_cast<std::int64_t>(state_.focused_group().active_tab_index);
    return static_cast<std::size_t>(((active + offset) % count + count) % count);
  }

  const std::string lowered_specifier = util::ToLowerAscii(specifier);
  if (const auto tab_number = util::ParseInt(specifier); tab_number.has_value()) {
    if (*tab_number >= 1 && static_cast<std::size_t>(*tab_number) <= state_.focused_group().open_tabs.size()) {
      return static_cast<std::size_t>(*tab_number - 1);
    }
    if (error_message != nullptr) {
      *error_message = "Invalid tab index";
    }
    return std::nullopt;
  }

  std::vector<std::size_t> exact_matches;
  std::vector<std::size_t> partial_matches;
  for (std::size_t i = 0; i < state_.focused_group().open_tabs.size(); ++i) {
    const TabEntry& tab = state_.focused_group().open_tabs[i];
    const std::string lowered_title = util::ToLowerAscii(tab.title);
    const std::string lowered_path = util::ToLowerAscii(RelativePathLabel(state_.root, tab.path));
    const std::string lowered_absolute_path = util::ToLowerAscii(tab.path.lexically_normal().string());
    const bool exact_match = lowered_title == lowered_specifier ||
                             (!lowered_path.empty() && lowered_path == lowered_specifier) ||
                             (!lowered_absolute_path.empty() &&
                              lowered_absolute_path == lowered_specifier);
    const bool partial_match = lowered_title.find(lowered_specifier) != std::string::npos ||
                               (!lowered_path.empty() &&
                                lowered_path.find(lowered_specifier) != std::string::npos) ||
                               (!lowered_absolute_path.empty() &&
                                lowered_absolute_path.find(lowered_specifier) != std::string::npos);
    if (exact_match) {
      exact_matches.push_back(i);
    } else if (partial_match) {
      partial_matches.push_back(i);
    }
  }

  if (exact_matches.size() == 1) {
    return exact_matches.front();
  }
  if (exact_matches.size() > 1) {
    if (error_message != nullptr) {
      *error_message = "Multiple tabs match: " + std::string(specifier);
    }
    return std::nullopt;
  }
  if (partial_matches.size() == 1) {
    return partial_matches.front();
  }
  if (partial_matches.size() > 1) {
    if (error_message != nullptr) {
      *error_message = "Multiple tabs match: " + std::string(specifier);
    }
    return std::nullopt;
  }
  if (error_message != nullptr) {
    *error_message = "Unknown tab: " + std::string(specifier);
  }
  return std::nullopt;
}

bool TabCoordinator::ReopenActive() {
  if (state_.focused_group().active_tab_index >= state_.focused_group().open_tabs.size()) {
    return false;
  }

  auto& tab = state_.focused_group().open_tabs[state_.focused_group().active_tab_index];
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return false;
  }
  auto& editor_state = *tab.editor_state;
  const std::filesystem::path reopen_path =
      editor_state.viewport.path().empty() ? tab.path.lexically_normal()
                                           : editor_state.viewport.path().lexically_normal();
  if (reopen_path.empty() || editor_state.viewport.dirty()) {
    return false;
  }
  std::error_code exists_error;
  if (!std::filesystem::is_regular_file(reopen_path, exists_error) || exists_error) {
    return false;
  }

  // Re-reading the file is a reload of the DOCUMENT, and a file open in several
  // panes (or as a compare's editable side) is one document: this used to swap a
  // fresh viewport into the active tab alone, which left every other view on the
  // old buffer -- the panes then diverged and saved over each other. The shared
  // reload re-points every clean view, keeps each one's caret and scroll, clears
  // their folds and re-syncs the language server once; when the file on disk
  // still matches what the views loaded it is a no-op, which is what reverting an
  // unchanged file should be.
  ReloadEditorTabsForPath(reopen_path, /*clean_only=*/true);

  SyncActiveEditorTabMetadata();
  state_.surface.focus = FocusTarget::Editor;
  operations_.reset_caret_blink();
  operations_.request_editor_surface_redraw();
  return true;
}

}  // namespace microide::workspace
