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
#include "util/ErrorOut.h"

namespace microide::workspace {

bool TabCoordinator::RestoreEditorTab(TabEntry::EditorTabState& editor_state) {
  util::PerformanceTrace::Scope perf_scope("TabCoordinator::RestoreEditorTab");
  if (!editor_state.content_pending()) {
    return true;
  }
  if (editor_state.content == TabEntry::EditorTabState::Content::Loading) {
    return true;
  }
  if (editor_state.content == TabEntry::EditorTabState::Content::Failed) {
    return false;
  }
  editor::TextViewport view;
  std::uint64_t read_id = 0;
  if (!LoadRestoreView(editor_state.restore, view, read_id)) {
    return false;
  }
  editor_state.viewport = std::move(view);
  if (read_id != 0) {
    editor_state.content = EditorTabState::Content::Loading;
    editor_state.pending_load.Arm(read_id, editor_state.viewport.content_revision());
  } else {
    editor_state.content = EditorTabState::Content::Ready;
  }
  return true;
}

bool TabCoordinator::LoadRestoreView(const DeferredTabHandle& restore, editor::TextViewport& view,
                                     std::uint64_t& read_id) {
  read_id = 0;
  if (restore.path.empty()) {
    return false;
  }
  if (ShouldOpenOffThread(restore.path)) {
    read_id = BeginOffThreadOpen(restore.path, view);
    if (read_id != 0) {
      return true;
    }
  }
  util::PerformanceTrace::Scope open_scope("TabCoordinator::RestoreEditorTab::OpenFile");
  if (!OpenEditorViewForPath(restore.path, view)) {
    return false;
  }
  restore.ApplyViewStateTo(view);
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
  tab.title = TabTitleForPath(tab.path, tab.untitled_number);
  return true;
}

bool TabCoordinator::LoadEditorTabForActivation(TabEntry& tab) {
  if (tab.kind != TabEntry::Kind::Editor) {
    return true;
  }
  if (tab.editor_state.has_value()) {
    const bool was_deferred = tab.editor_state->content_pending();
    if (!EnsureEditorTabLoaded(tab)) {
      return false;
    }
    if (!was_deferred) {
      operations_.apply_editor_preferences(tab.editor_state->viewport);
    }
    return true;
  }
  DeferredTabHandle handle = tab.deferred_handle.has_value() ? std::move(*tab.deferred_handle)
                                                             : DeferredTabHandle{.path = tab.path};
  handle.path = handle.path.lexically_normal();
  if (handle.path.empty()) {
    return false;
  }
  editor::TextViewport view;
  std::uint64_t read_id = 0;
  if (!LoadRestoreView(handle, view, read_id)) {
    const std::filesystem::path path = handle.path;
    editor::TextViewport stand_in;
    stand_in.LoadContent("", path);
    stand_in.SetReadOnly(true);
    operations_.apply_editor_preferences(stand_in);
    tab.editor_state = operations_.make_editor_tab_state(stand_in);
    tab.editor_state->restore = std::move(handle);
    tab.editor_state->content = EditorTabState::Content::Failed;
    tab.path = path;
    tab.title = TabTitleForPath(tab.path, tab.untitled_number);
    SetEditorBanner(state_, EditorBannerState::Kind::OpenFailed, path,
                    "Could not read " + path.filename().string());
    return false;
  }
  tab.deferred_handle.reset();
  tab.editor_state = operations_.make_editor_tab_state(view);
  tab.editor_state->restore = std::move(handle);
  if (read_id != 0) {
    tab.editor_state->content = EditorTabState::Content::Loading;
    tab.editor_state->pending_load.Arm(read_id, tab.editor_state->viewport.content_revision());
  }
  tab.path = operations_.editor_view_path(*tab.editor_state);
  tab.title = TabTitleForPath(tab.path, tab.untitled_number);
  return true;
}

std::optional<std::size_t> TabCoordinator::FindIndexBySpecifier(std::string_view specifier,
                                                                std::string* error_message) const {
  if (specifier.empty()) {
    util::SetError(error_message, "usage: tabswitch <tab>");
    return std::nullopt;
  }

  const std::size_t tab_count = state_.focused_group().open_tabs.size();
  if ((specifier.front() == '+' || specifier.front() == '-') && specifier.size() > 1) {
    const bool forward = specifier.front() == '+';
    const std::optional<int> magnitude = util::ParseInt(specifier.substr(1));
    if (!magnitude.has_value() || *magnitude < 0) {
      util::SetError(error_message, "Invalid tab offset");
      return std::nullopt;
    }
    if (tab_count == 0) {
      util::SetError(error_message, "No open tabs");
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
    util::SetError(error_message, "Invalid tab index");
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
    util::SetError(error_message, "Multiple tabs match: " + std::string(specifier));
    return std::nullopt;
  }
  if (partial_matches.size() == 1) {
    return partial_matches.front();
  }
  if (partial_matches.size() > 1) {
    util::SetError(error_message, "Multiple tabs match: " + std::string(specifier));
    return std::nullopt;
  }
  util::SetError(error_message, "Unknown tab: " + std::string(specifier));
  return std::nullopt;
}

bool TabCoordinator::ReopenActive() {
  if (state_.focused_group().active_tab_index >= state_.focused_group().open_tabs.size()) {
    return false;
  }

  auto& tab = state_.focused_group().active_tab();
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

  ReloadEditorTabsForPath(reopen_path, true);

  SyncActiveEditorTabMetadata();
  state_.surface.focus = FocusTarget::Editor;
  operations_.reset_caret_blink();
  operations_.request_frame();
  return true;
}

}
