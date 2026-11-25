#include "workspace/coordinators/WorkspaceTabCoordinator.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/StartupTrace.h"
#include "util/PerformanceTrace.h"
#include "util/StringUtil.h"
#include "util/TextFileIO.h"
#include "workspace/git/MergeResultValidation.h"
#include "workspace/TabReorder.h"
#include "workspace/WorkspacePathUtils.h"
#include "workspace/WorkspaceTextSearch.h"
#include "util/InvokeIfSet.h"

namespace microide::workspace {

namespace {

editor::TextViewport::SaveTransforms SaveTransformsFor(SaveMode mode) {
  return mode == SaveMode::Autosave ? editor::TextViewport::SaveTransforms::Skip
                                    : editor::TextViewport::SaveTransforms::Apply;
}

}

namespace {

std::string EditorTabLabel(const editor::TextViewport& viewport, std::uint32_t untitled_number) {
  if (viewport.path().empty() && viewport.is_placeholder()) {
    return "Welcome";
  }
  return TabTitleForPath(viewport.path(), untitled_number);
}

}

TabCoordinator::TabCoordinator(ProjectCatalogState& project_catalog,
                               ProjectWorkspaceState& current_project_state,
                               Operations operations)
    : project_catalog_(project_catalog),
      state_(current_project_state),
      operations_(std::move(operations)) {}


bool TabCoordinator::Save(std::size_t index, SaveMode mode) {
  return SaveGroupTab(state_.clamped_focused_group_index(), index, mode);
}

bool TabCoordinator::SaveGroupTab(std::size_t group_index, std::size_t index, SaveMode mode) {
  if (group_index >= state_.editor_groups.size()) {
    return false;
  }
  EditorGroup& group = state_.editor_groups[group_index];
  if (index >= group.open_tabs.size()) {
    return false;
  }

  if (group.open_tabs[index].kind == TabEntry::Kind::Compare &&
      group.open_tabs[index].compare.has_value()) {
    auto& compare_tab = group.open_tabs[index].compare.value();
    if (!compare_tab.right_editable || !compare_tab.right_viewport.dirty()) {
      return true;
    }
    const PaneSave saved =
        WritePaneViewport(compare_tab.right_viewport, compare_tab.pending_format_save, mode);
    if (saved != PaneSave::Written) {
      return saved == PaneSave::Deferred;
    }
    AfterFileSaved(compare_tab.right_viewport.path().lexically_normal());
    return true;
  }

  if (group.open_tabs[index].kind == TabEntry::Kind::Merge &&
      group.open_tabs[index].merge.has_value()) {
    auto& merge_tab = group.open_tabs[index].merge.value();
    if (!merge_tab.result_viewport.dirty()) {
      return true;
    }
    const PaneSave saved =
        WritePaneViewport(merge_tab.result_viewport, merge_tab.pending_format_save, mode);
    if (saved != PaneSave::Written) {
      return saved == PaneSave::Deferred;
    }
    merge_tab.persisted_output_baseline =
        merge_tab.result_viewport.SerializeDocumentText(merge_tab.result_line_ending);
    merge_tab.disk_result_tick = util::FileModificationTick(merge_tab.result_viewport.path());
    merge_tab.external_result_stale = false;
    AfterFileSaved(merge_tab.result_viewport.path().lexically_normal());
    return true;
  }

  if (group.open_tabs[index].kind != TabEntry::Kind::Editor) {
    return false;
  }

  auto& editor_state = group.open_tabs[index].editor_state;
  if (!editor_state.has_value()) {
    return false;
  }

  if (editor_state->content_pending()) {
    return true;
  }
  editor::TextViewport* candidate = &editor_state->viewport;
  if (candidate->path().empty()) {
    return !candidate->dirty();
  }
  const std::filesystem::path normalized_path = candidate->path().lexically_normal();
  if (!candidate->dirty() && candidate->HasDiskSignature()) {
    operations_.notify_plugin_buffer_save(normalized_path);
    return true;
  }
  if (candidate->SaveWouldOverwriteUnseenDisk()) {
    util::InvokeIfSet(operations_.request_external_change_banner, normalized_path);
    return false;
  }
  if (!RefuseIfReadOnly(normalized_path, true)) {
    return false;
  }
  if (operations_.prepare_editor_view_for_save) {
    const SavePreparation prepared =
        operations_.prepare_editor_view_for_save(candidate->path(), *candidate, nullptr, mode);
    if (!prepared.ok()) {
      return false;
    }
    if (prepared.deferred()) {
      editor_state->pending_format_save.Arm(prepared.deferred_run_id,
                                            candidate->content_revision());
      return true;
    }
    if (!RefuseIfChangedOnDisk(*candidate, normalized_path)) {
      return false;
    }
  }
  editor_state->pending_format_save.Disarm();
  if (std::error_code why; !candidate->Save(state_.write_gate(), &why, SaveTransformsFor(mode))) {
    if (operations_.notify_save_failed) {
      operations_.notify_save_failed(candidate->path(), util::DescribeWriteError(why));
    }
    return false;
  }
  operations_.request_frame();
  AfterFileSaved(normalized_path);
  return true;
}

TabCoordinator::PaneSave TabCoordinator::WritePaneViewport(
    editor::TextViewport& viewport, editor::AsyncBufferWork& pending_format_save, SaveMode mode) {
  const std::filesystem::path normalized_path = viewport.path().lexically_normal();
  if (viewport.SaveWouldOverwriteUnseenDisk()) {
    util::InvokeIfSet(operations_.request_external_change_banner, normalized_path);
    return PaneSave::Refused;
  }
  if (operations_.prepare_editor_view_for_save) {
    const SavePreparation prepared =
        operations_.prepare_editor_view_for_save(viewport.path(), viewport, nullptr, mode);
    if (prepared.deferred()) {
      pending_format_save.Arm(prepared.deferred_run_id, viewport.content_revision());
      return PaneSave::Deferred;
    }
    if (!prepared.ok()) {
      return PaneSave::Refused;
    }
  }
  if (!RefuseIfChangedOnDisk(viewport, normalized_path) ||
      !RefuseIfReadOnly(normalized_path, false)) {
    return PaneSave::Refused;
  }
  if (std::error_code why; !viewport.Save(state_.write_gate(), &why, SaveTransformsFor(mode))) {
    if (operations_.notify_save_failed) {
      operations_.notify_save_failed(viewport.path(), util::DescribeWriteError(why));
    }
    return PaneSave::Refused;
  }
  return PaneSave::Written;
}

void TabCoordinator::AfterFileSaved(const std::filesystem::path& normalized_path) {
  operations_.invalidate_editor_blame_path(normalized_path);
  operations_.notify_plugin_buffer_save(normalized_path);
  state_.directory_tree.Refresh();
  util::InvokeIfSet(operations_.request_automatic_git_sidebar_refresh);
}

bool TabCoordinator::ActiveTabIsEditor() const {
  return ActiveEditorTab() != nullptr;
}

TabEntry::EditorTabState* TabCoordinator::ActiveEditorTab() {
  return const_cast<TabEntry::EditorTabState*>(std::as_const(*this).ActiveEditorTab());
}

const TabEntry::EditorTabState* TabCoordinator::ActiveEditorTab() const {
  return state_.focused_group().active_editor();
}

editor::TextViewport* TabCoordinator::ActiveEditorViewport() {
  auto* editor_tab = ActiveEditorTab();
  if (editor_tab == nullptr) {
    return &state_.focused_group().welcome_surface.viewport;
  }
  return &editor_tab->viewport;
}

void TabCoordinator::Activate(std::size_t index) {
  if (index >= state_.focused_group().open_tabs.size()) {
    return;
  }
  util::PerformanceTrace::ScopeLabel perf_label("TabCoordinator::Activate");
  perf_label.Field("index", static_cast<long long>(index));
  if (!state_.focused_group().open_tabs[index].path.empty()) {
    perf_label.Field("path", state_.focused_group().open_tabs[index].path);
  }
  util::StartupTrace::Scope trace_scope("TabCoordinator::Activate");
  util::PerformanceTrace::Scope perf_scope(perf_label.View());

  if (state_.focused_group().active_tab_index == index) {
    auto& active_tab = state_.focused_group().open_tabs[index];
    if (active_tab.kind == TabEntry::Kind::Editor &&
        (active_tab.editor_state.has_value() ? active_tab.editor_state->content_pending()
                                             : active_tab.deferred_handle.has_value())) {
      (void)LoadEditorTabForActivation(active_tab);
    }
    SyncActiveEditorTabMetadata();
    state_.surface.focus = FocusTarget::Editor;
    operations_.reset_caret_blink();
    operations_.request_frame();
    return;
  }

  if (state_.focused_group().active_tab_index < state_.focused_group().open_tabs.size() && state_.focused_group().active_tab_index != index) {
    SyncActiveEditorTab();
  }

  state_.focused_group().active_tab_index = index;
  auto& tab = state_.focused_group().open_tabs[index];
  (void)LoadEditorTabForActivation(tab);
  SyncActiveEditorTabMetadata();
  const editor::TextViewport* active_vp =
      (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value())
          ? &tab.editor_state->viewport
          : nullptr;
  const std::filesystem::path active_vp_path =
      active_vp != nullptr ? active_vp->path().lexically_normal() : std::filesystem::path{};
  if (tab.kind == TabEntry::Kind::Compare) {
    operations_.reveal_active_compare_selection();
  } else if (tab.kind == TabEntry::Kind::Merge) {
    operations_.reveal_active_merge_selection();
  } else if (tab.kind == TabEntry::Kind::Editor && !active_vp_path.empty()) {
    util::StartupTrace::Scope select_path_scope("TabCoordinator::Activate::SelectDirectoryPath");
    if (state_.directory_tree.SelectPathIfVisible(active_vp_path)) {
      operations_.reveal_selected_tree_sidebar_line();
    }
  }
  operations_.ensure_active_tab_visible();
  state_.surface.focus = FocusTarget::Editor;
  operations_.reset_caret_blink();
  if (tab.kind == TabEntry::Kind::Editor && !active_vp_path.empty() &&
      operations_.schedule_lsp_buffer_open) {
    operations_.schedule_lsp_buffer_open(active_vp_path);
  }
  operations_.request_frame();
}

void TabCoordinator::SyncActiveEditorTab() {
  if (state_.focused_group().active_tab_index >= state_.focused_group().open_tabs.size()) {
    return;
  }

  auto& tab = state_.focused_group().active_tab();
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
    return;
  }

  auto& editor_state = *tab.editor_state;
  if (editor_state.content_pending()) {
    tab.path = operations_.editor_view_path(editor_state);
    tab.title = TabTitleForPath(tab.path, tab.untitled_number);
    return;
  }
  editor_state.restore.path = editor_state.viewport.path().lexically_normal();
  editor_state.restore.cursor_line = editor_state.viewport.cursor_line();
  editor_state.restore.cursor_column = editor_state.viewport.cursor_column();
  editor_state.restore.scroll_line = editor_state.viewport.scroll_line();
  editor_state.restore.horizontal_scroll = editor_state.viewport.horizontal_scroll();
  if (state_.focused_group().active_tab_index < state_.focused_group().open_tabs.size() &&
      &tab == &state_.focused_group().active_tab()) {
    SyncActiveEditorTabMetadata();
  }
}

bool TabCoordinator::ActivateCurrentTabAfterStateLoad() {
  for (std::size_t gi = 0; gi < state_.editor_groups.size(); ++gi) {
    if (gi == state_.focused_group_index) {
      continue;
    }
    EditorGroup& group = state_.editor_groups[gi];
    if (group.active_tab_index >= group.open_tabs.size()) {
      continue;
    }
    TabEntry& tab = group.active_tab();
    if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
      const bool was_deferred = tab.editor_state->content_pending();
      if (EnsureEditorTabLoaded(tab) && !was_deferred) {
        operations_.apply_editor_preferences(tab.editor_state->viewport);
      }
    }
  }

  if (state_.focused_group().open_tabs.empty()) {
    return true;
  }

  const std::size_t active_index = std::min(state_.focused_group().active_tab_index, state_.focused_group().open_tabs.size() - 1);
  state_.focused_group().active_tab_index = state_.focused_group().open_tabs.size();
  const FocusTarget loaded_focus = state_.surface.focus;
  Activate(active_index);
  state_.surface.focus = loaded_focus;
  return state_.focused_group().active_tab_index == active_index;
}

void TabCoordinator::SyncActiveEditorTabMetadata() {
  if (state_.focused_group().active_tab_index >= state_.focused_group().open_tabs.size()) {
    return;
  }

  auto& tab = state_.focused_group().active_tab();
  if (tab.kind != TabEntry::Kind::Editor) {
    return;
  }

  std::filesystem::path active_path;
  if (tab.editor_state.has_value() && tab.editor_state->content_pending()) {
    active_path = operations_.editor_view_path(*tab.editor_state);
    if (!active_path.empty()) {
      tab.path = active_path;
      tab.title = active_path.filename().string();
    }
  } else if (tab.editor_state.has_value()) {
    const editor::TextViewport& viewport = tab.editor_state->viewport;
    active_path = viewport.path().lexically_normal();
    tab.path = active_path;
    tab.title = EditorTabLabel(viewport, tab.untitled_number);
  } else if (tab.deferred_handle.has_value() || !tab.path.empty()) {
    if (tab.path.empty() && tab.deferred_handle.has_value()) {
      tab.path = tab.deferred_handle->path.lexically_normal();
      tab.title = TabTitleForPath(tab.path, tab.untitled_number);
    }
    active_path = tab.path.lexically_normal();
  } else {
    const editor::TextViewport& viewport = state_.focused_group().welcome_surface.viewport;
    active_path = viewport.path().lexically_normal();
    tab.path = active_path;
    tab.title = EditorTabLabel(viewport, tab.untitled_number);
  }
  if (!active_path.empty() && state_.directory_tree.SelectPathIfVisible(active_path)) {
    operations_.reveal_selected_tree_sidebar_line();
  } else if (!active_path.empty() &&
             !state_.directory_tree.HasManuallyCollapsedAncestor(active_path) &&
             state_.directory_tree.SelectPath(active_path)) {
      operations_.reveal_selected_tree_sidebar_line();
  }
}

bool TabCoordinator::OpenUntitled() {
  if (state_.root.empty()) {
    return false;
  }
  if (state_.focused_group().open_tabs.size() >= kMaxOpenTabsPerGroup) {
    return false;
  }

  editor::TextViewport untitled_view;
  untitled_view.SetUntitledBuffer();
  operations_.apply_editor_preferences(untitled_view);

  const std::uint32_t untitled_number = NextUntitledNumber(state_.editor_groups);
  state_.focused_group().open_tabs.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = {},
      .title = UntitledTitle(untitled_number),
      .untitled_number = untitled_number,
      .editor_state = operations_.make_editor_tab_state(untitled_view),
      .deferred_handle = std::nullopt,
      .compare = std::nullopt,
      .merge = std::nullopt,
  });
  state_.focused_group().active_tab_index = state_.focused_group().open_tabs.size() - 1;
  operations_.ensure_active_tab_visible();
  state_.surface.focus = FocusTarget::Editor;
  operations_.reset_caret_blink();
  operations_.request_frame();
  return true;
}
const editor::TextViewport* TabCoordinator::FindOpenEditorViewOfPath(
    const std::filesystem::path& normalized_path) const {
  return LiveBufferViewOfPath(state_.editor_groups, normalized_path);
}

bool TabCoordinator::OpenEditorViewForPath(const std::filesystem::path& path,
                                           editor::TextViewport& view) const {
  const std::filesystem::path normalized_path = path.lexically_normal();
  if (const editor::TextViewport* live = FindOpenEditorViewOfPath(normalized_path);
      live != nullptr) {
    view = *live;
    return true;
  }
  if (!view.OpenFile(normalized_path)) {
    return false;
  }
  operations_.apply_editor_preferences(view);
  operations_.apply_detected_indent_on_open(view);
  return true;
}

bool TabCoordinator::ShouldOpenOffThread(const std::filesystem::path& normalized_path) const {
  if (!operations_.begin_async_file_read) {
    return false;
  }
  if (FindOpenEditorViewOfPath(normalized_path) != nullptr) {
    return false;
  }
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(normalized_path, error);
  return !error && size >= kAsyncOpenThresholdBytes;
}

std::uint64_t TabCoordinator::BeginOffThreadOpen(const std::filesystem::path& normalized_path,
                                                 editor::TextViewport& view) const {
  const std::uint64_t read_id = operations_.begin_async_file_read(normalized_path);
  if (read_id == 0) {
    return 0;
  }
  view.LoadContent("", normalized_path);
  view.SetReadOnly(true);
  operations_.apply_editor_preferences(view);
  return read_id;
}

bool TabCoordinator::OpenFileInNewTab(const std::filesystem::path& path) {
  util::PerformanceTrace::ScopeLabel perf_label("TabCoordinator::OpenFileInNewTab");
  perf_label.Field("path", path);
  util::PerformanceTrace::Scope perf_scope(perf_label.View());
  if (state_.root.empty()) {
    return false;
  }
  const std::filesystem::path normalized_path = path.lexically_normal();

  auto existing = std::find_if(state_.focused_group().open_tabs.begin(), state_.focused_group().open_tabs.end(),
                               [&](const TabEntry& tab) {
                                 return tab.kind == TabEntry::Kind::Editor &&
                                        tab.path == normalized_path;
                               });

  {
    util::PerformanceTrace::Scope select_path_scope(
        "TabCoordinator::OpenFileInNewTab::SelectDirectoryPath");
    if (!state_.directory_tree.SelectPathIfVisible(normalized_path)) {
      state_.directory_tree.SelectPath(normalized_path);
    }
  }

  if (existing != state_.focused_group().open_tabs.end()) {
    const std::size_t existing_index =
        static_cast<std::size_t>(std::distance(state_.focused_group().open_tabs.begin(), existing));
    if (!IsDirty(existing_index)) {
      ReloadCleanEditorTabsForPath(normalized_path);
    }
    Activate(existing_index);
    return true;
  }

  if (state_.focused_group().open_tabs.size() >= kMaxOpenTabsPerGroup) {
    return false;
  }

  editor::TextViewport opened_view;
  std::uint64_t read_id = 0;
  {
    util::PerformanceTrace::Scope open_scope("TabCoordinator::OpenFileInNewTab::OpenFile");
    if (ShouldOpenOffThread(normalized_path)) {
      read_id = BeginOffThreadOpen(normalized_path, opened_view);
    }
    if (read_id == 0 && !OpenEditorViewForPath(normalized_path, opened_view)) {
      return false;
    }
  }

  state_.focused_group().open_tabs.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = normalized_path,
      .title = normalized_path.filename().string(),
      .editor_state = operations_.make_editor_tab_state(opened_view),
      .deferred_handle = std::nullopt,
      .compare = std::nullopt,
      .merge = std::nullopt,
  });
  if (read_id != 0) {
    TabEntry::EditorTabState& editor_state =
        *state_.focused_group().open_tabs.back().editor_state;
    editor_state.content = EditorTabState::Content::Loading;
    editor_state.restore.path = normalized_path;
    editor_state.pending_load.Arm(read_id, editor_state.viewport.content_revision());
  }
  state_.focused_group().active_tab_index = state_.focused_group().open_tabs.size() - 1;
  operations_.ensure_active_tab_visible();
  state_.surface.focus = FocusTarget::Editor;
  operations_.reset_caret_blink();
  if (read_id == 0) {
    operations_.notify_plugin_buffer_open(normalized_path);
  }
  operations_.request_frame();
  return true;
}
bool TabCoordinator::ReopenEditor(DeferredTabHandle view) {
  if (state_.root.empty() || view.path.empty()) {
    return false;
  }
  view.path = view.path.lexically_normal();
  EditorGroup& group = state_.focused_group();
  const auto existing = std::find_if(group.open_tabs.begin(), group.open_tabs.end(),
                                     [&](const TabEntry& tab) {
                                       return tab.kind == TabEntry::Kind::Editor &&
                                              tab.path == view.path;
                                     });
  if (existing != group.open_tabs.end()) {
    Activate(static_cast<std::size_t>(std::distance(group.open_tabs.begin(), existing)));
    return true;
  }
  if (group.open_tabs.size() >= kMaxOpenTabsPerGroup) {
    return false;
  }
  const std::filesystem::path path = view.path;
  group.open_tabs.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = path,
      .title = TabTitleForPath(path, 0),
      .editor_state = std::nullopt,
      .deferred_handle = std::move(view),
      .compare = std::nullopt,
      .merge = std::nullopt,
  });
  Activate(group.open_tabs.size() - 1);
  return true;
}

bool TabCoordinator::OpenNewBufferInNewTab(const std::filesystem::path& path) {
  if (state_.root.empty() || path.empty() || path.filename().empty()) {
    return false;
  }
  const std::filesystem::path normalized_path = path.lexically_normal();
  std::error_code exists_error;
  if (std::filesystem::exists(normalized_path, exists_error)) {
    return OpenFileInNewTab(normalized_path);
  }
  EditorGroup& group = state_.focused_group();
  auto existing = std::find_if(group.open_tabs.begin(), group.open_tabs.end(),
                               [&](const TabEntry& tab) {
                                 return tab.kind == TabEntry::Kind::Editor &&
                                        tab.path == normalized_path;
                               });
  if (existing != group.open_tabs.end()) {
    Activate(static_cast<std::size_t>(std::distance(group.open_tabs.begin(), existing)));
    return true;
  }
  if (group.open_tabs.size() >= kMaxOpenTabsPerGroup) {
    return false;
  }
  editor::TextViewport opened_view;
  if (const editor::TextViewport* live = FindOpenEditorViewOfPath(normalized_path);
      live != nullptr) {
    opened_view = *live;
  } else {
    opened_view.LoadContent("", normalized_path);
    operations_.apply_editor_preferences(opened_view);
  }
  group.open_tabs.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = normalized_path,
      .title = normalized_path.filename().string(),
      .editor_state = operations_.make_editor_tab_state(opened_view),
      .deferred_handle = std::nullopt,
      .compare = std::nullopt,
      .merge = std::nullopt,
  });
  group.active_tab_index = group.open_tabs.size() - 1;
  operations_.ensure_active_tab_visible();
  state_.surface.focus = FocusTarget::Editor;
  operations_.reset_caret_blink();
  operations_.notify_plugin_buffer_open(normalized_path);
  operations_.request_frame();
  return true;
}

bool TabCoordinator::OpenVirtualDocumentInNewTab(const std::filesystem::path& virtual_path,
                                                 std::string_view content,
                                                 std::string_view title) {
  if (state_.root.empty() || virtual_path.empty()) {
    return false;
  }

  auto existing = std::find_if(state_.focused_group().open_tabs.begin(), state_.focused_group().open_tabs.end(),
                               [&](const TabEntry& tab) {
                                 return tab.kind == TabEntry::Kind::Editor &&
                                        tab.path == virtual_path;
                               });
  if (existing != state_.focused_group().open_tabs.end()) {
    const std::size_t index =
        static_cast<std::size_t>(std::distance(state_.focused_group().open_tabs.begin(), existing));
    if (!IsDirty(index) && existing->editor_state.has_value() &&
        operations_.editor_view_path(*existing->editor_state) == virtual_path) {
      existing->editor_state->viewport.ReloadPreservingViewState(content);
    }
    Activate(index);
    return true;
  }

  editor::TextViewport viewport;
  viewport.LoadContent(content, virtual_path);
  operations_.apply_editor_preferences(viewport);
  operations_.apply_detected_indent_on_open(viewport);

  if (state_.focused_group().open_tabs.size() >= kMaxOpenTabsPerGroup) {
    return false;
  }
  state_.focused_group().open_tabs.push_back(TabEntry{
      .kind = TabEntry::Kind::Editor,
      .path = virtual_path,
      .title = std::string(title),
      .editor_state = operations_.make_editor_tab_state(viewport),
      .deferred_handle = std::nullopt,
      .compare = std::nullopt,
      .merge = std::nullopt,
  });
  state_.focused_group().active_tab_index = state_.focused_group().open_tabs.size() - 1;
  operations_.ensure_active_tab_visible();
  state_.surface.focus = FocusTarget::Editor;
  operations_.reset_caret_blink();
  operations_.request_frame();
  return true;
}
void TabCoordinator::ReloadVirtualDocumentTabs(const std::filesystem::path& virtual_path,
                                               std::string_view content) {
  if (virtual_path.empty()) {
    return;
  }

  bool reloaded_any = false;
  for (std::size_t gi = 0; gi < state_.editor_groups.size(); ++gi) {
    EditorGroup& group = state_.editor_groups[gi];
    for (std::size_t i = 0; i < group.open_tabs.size(); ++i) {
      auto& tab = group.open_tabs[i];
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
          TabStateIsDirty(tab)) {
        continue;
      }
      if (operations_.editor_view_path(*tab.editor_state) != virtual_path) {
        continue;
      }
      tab.editor_state->viewport.ReloadPreservingViewState(content);
      reloaded_any = true;
      if (i == group.active_tab_index) {
        operations_.apply_editor_preferences(tab.editor_state->viewport);
      }
    }
  }

  if (reloaded_any) {
    operations_.request_frame();
  }
}
std::filesystem::path TabCoordinator::LspCloseCandidatePath(const TabEntry& tab) const {
  if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
    return operations_.editor_view_path(*tab.editor_state);
  }
  if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
    const auto& compare_tab = *tab.compare;
    if (compare_tab.right_editable && !compare_tab.right_viewport.path().empty()) {
      return compare_tab.right_viewport.path().lexically_normal();
    }
  } else if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value()) {
    const auto& merge_tab = *tab.merge;
    if (!merge_tab.result_viewport.path().empty()) {
      return merge_tab.result_viewport.path().lexically_normal();
    }
  }
  return {};
}

void TabCoordinator::CancelPendingAsyncRead(const TabEntry& tab) {
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
      !operations_.cancel_async_file_read) {
    return;
  }
  const std::uint64_t read_id = tab.editor_state->pending_load.id();
  if (read_id != 0) {
    operations_.cancel_async_file_read(read_id);
  }
}

void TabCoordinator::MaybeNotifyLspClose(const TabEntry& tab) {
  const std::filesystem::path path = LspCloseCandidatePath(tab);
  if (!path.empty() && operations_.count_open_buffer_views(path) == 1) {
    operations_.notify_lsp_buffer_close(path);
  }
}

bool TabCoordinator::RefuseIfChangedOnDisk(const editor::TextViewport& viewport,
                                           const std::filesystem::path& normalized_path) {
  if (!viewport.SaveWouldOverwriteUnseenDisk()) {
    return true;
  }
  util::InvokeIfSet(operations_.request_external_change_banner, normalized_path);
  return false;
}

bool TabCoordinator::RefuseIfReadOnly(const std::filesystem::path& normalized_path,
                                      bool banner_shows) {
  if (!state_.write_gate().IsReadOnly(normalized_path)) {
    return true;
  }
  if (banner_shows) {
    SetEditorBanner(state_, EditorBannerState::Kind::ReadOnly, normalized_path);
    util::InvokeIfSet(operations_.request_editor_surface_redraw);
  } else util::InvokeIfSet(operations_.notify_save_failed, normalized_path, "the file is read-only");
  return false;
}

TabCoordinator::SaveForCloseResult TabCoordinator::SaveForClose(std::size_t index) {
  const std::size_t group_index = state_.clamped_focused_group_index();
  if (!SaveGroupTab(group_index, index, SaveMode::Deferred)) {
    return {};
  }
  EditorGroup& group = state_.editor_groups[group_index];
  if (index < group.open_tabs.size() && group.open_tabs[index].editor_state.has_value() &&
      group.open_tabs[index].editor_state->pending_format_save.armed()) {
    return {.saved = true, .deferred_tab_id = EnsureTabStableId(group.open_tabs[index])};
  }
  Close(index);
  return {.saved = true};
}

void TabCoordinator::RememberClosedEditor(const TabEntry& tab) {
  if (tab.kind != TabEntry::Kind::Editor || tab.path.empty() || !tab.path.is_absolute()) {
    return;
  }
  std::optional<DeferredTabHandle> view = EditorTabViewState(tab);
  DeferredTabHandle entry = view.has_value() ? std::move(*view) : DeferredTabHandle{};
  entry.path = tab.path.lexically_normal();
  auto& closed = state_.recently_closed_editors;
  std::erase_if(closed, [&](const auto& old) { return old.path == entry.path; });
  if (closed.size() >= ProjectWorkspaceState::kMaxRecentlyClosedEditors) {
    closed.erase(closed.begin());
  }
  closed.push_back(std::move(entry));
}

void TabCoordinator::ObserveNavigationLocation() {
  const EditorGroup& group = state_.focused_group();
  if (group.active_tab_index >= group.open_tabs.size()) {
    return;
  }
  const TabEntry& tab = group.active_tab();
  if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
      !tab.path.is_absolute()) {
    return;
  }
  const EditorTabState& editor = *tab.editor_state;
  state_.navigation_history.Observe(
      tab.path, editor.content_pending() ? editor.restore.cursor_line : editor.viewport.cursor_line(),
      editor.content_pending() ? editor.restore.cursor_column : editor.viewport.cursor_column());
}

std::optional<EditorNavigationHistory::Location> TabCoordinator::StepNavigation(int direction) {
  ObserveNavigationLocation();
  return state_.navigation_history.Step(direction, [](const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
  });
}

void TabCoordinator::Close(std::size_t index) {
  if (index >= state_.focused_group().open_tabs.size()) {
    return;
  }
  const TabEntry& closing_tab = state_.focused_group().open_tabs[index];

  MaybeNotifyLspClose(closing_tab);
  CancelPendingAsyncRead(closing_tab);
  RememberClosedEditor(closing_tab);
  const std::uint64_t closed_id = closing_tab.stable_id;

  state_.focused_group().open_tabs.erase(state_.focused_group().open_tabs.begin() + static_cast<std::ptrdiff_t>(index));
  util::InvokeIfSet(operations_.tab_closed, closed_id);

  if (state_.focused_group().open_tabs.empty()) {
    if (state_.editor_groups.size() >= 2) {
      CollapseFocusedGroup();
      operations_.ensure_active_tab_visible();
      operations_.request_frame();
      return;
    }
    state_.focused_group().active_tab_index = 0;
    state_.focused_group().tab_scroll_index = 0;
    state_.focused_group().welcome_surface.viewport.SetPlaceholderText("microide\n\n"
                                            "Project loaded.\n"
                                            "Use the sidebar to open files.\n");
    state_.surface.focus = FocusTarget::Editor;
    operations_.request_frame();
    return;
  }

  if (index < state_.focused_group().active_tab_index) {
    --state_.focused_group().active_tab_index;
  } else if (index == state_.focused_group().active_tab_index) {
    state_.focused_group().active_tab_index = std::min(index, state_.focused_group().open_tabs.size() - 1);
    auto& tab = state_.focused_group().active_tab();
    if (tab.kind == TabEntry::Kind::Editor) {
      (void)LoadEditorTabForActivation(tab);
    } else if (tab.kind == TabEntry::Kind::Compare) {
      operations_.reveal_active_compare_selection();
    } else if (tab.kind == TabEntry::Kind::Merge) {
      operations_.reveal_active_merge_selection();
    }
    if (!tab.path.empty()) {
      state_.directory_tree.SelectPath(tab.path);
      operations_.reveal_selected_tree_sidebar_line();
    }
    state_.surface.focus = FocusTarget::Editor;
  }

  state_.focused_group().tab_scroll_index = std::clamp(state_.focused_group().tab_scroll_index, 0,
                                       std::max(0, static_cast<int>(state_.focused_group().open_tabs.size()) - 1));
  operations_.ensure_active_tab_visible();
  operations_.request_frame();
}
bool TabCoordinator::MoveActiveTo(std::size_t index) {
  if (!ReorderActive(state_.focused_group().open_tabs, state_.focused_group().active_tab_index, index)) {
    return false;
  }
  operations_.ensure_active_tab_visible();
  state_.surface.focus = FocusTarget::Editor;
  operations_.request_frame();
  return true;
}

}
