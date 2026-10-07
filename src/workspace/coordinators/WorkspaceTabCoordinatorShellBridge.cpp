#include <memory>

#include "workspace/shell/WorkspaceShell.h"

#include "workspace/SettingFlags.h"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/RuntimeSyntaxRegistry.h"
#include "platform/ProcessLauncher.h"
#include "util/Parse.h"
#include "util/StringUtil.h"
#include "workspace/services/EditorTabService.h"
#include "workspace/WorkspacePathUtils.h"
#include "workspace/WorkspaceProjectPresentation.h"
#include "workspace/coordinators/WorkspaceTabCoordinator.h"

namespace microide::workspace {

namespace {

std::string SerializeViewportText(const editor::TextViewport& viewport) {
  return util::SerializeLinesStreaming(editor::LineSpan(viewport.lines()), viewport.line_ending());
}

// Above this a read takes long enough that an editor sitting empty with no
// explanation reads as a bug rather than as work in progress. Well above the
// threshold that sends the read off-thread at all (4 MiB, where the wait is a
// frame or two and a toast would only flash).
constexpr std::uintmax_t kAnnounceReadAboveBytes = 48ull * 1024 * 1024;

bool FileIsBigEnoughToAnnounceTheRead(const std::filesystem::path& path) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  return !error && size >= kAnnounceReadAboveBytes;
}

}  // namespace

TabCoordinator WorkspaceShell::MakeTabCoordinator() {
  return TabCoordinator(
      context_.project_catalog,
      context_.current_project_state,
      TabCoordinator::Operations{
          .invalidate_editor_blame_path =
              [this](const std::filesystem::path& path) { InvalidateEditorBlamePath(path); },
          .notify_plugin_buffer_save =
              [this](const std::filesystem::path& path) { NotifyPluginBufferSave(path); },
          .notify_plugin_buffer_open =
              [this](const std::filesystem::path& path) { NotifyPluginBufferOpen(path); },
          .schedule_lsp_buffer_open =
              [this](const std::filesystem::path& path) {
                // Record the hydration and request a frame; the actual didOpen +
                // token/inlay work runs post-present in OnFramePresented so a large
                // buffer's hydration never blocks the tab switch (TD-2026-07-17A-033).
                lsp_service_.ScheduleBufferOpen(path);
                RequestWindowRedraw();
              },
          .notify_lsp_buffer_close =
              [this](const std::filesystem::path& path) { NotifyLspBufferClose(path); },
          .notify_lsp_buffer_reloaded =
              [this](const editor::TextViewport& viewport, std::size_t before_line_count,
                     std::size_t first_changed_line) {
                lsp_service_.SyncLspForBufferChange(
                    viewport, {before_line_count, first_changed_line});
              },
          .count_open_buffer_views =
              [this](const std::filesystem::path& path) { return CountOpenBufferViews(path); },
          .open_buffer_view_counts = [this]() { return OpenBufferViewCounts(); },
          .prepare_editor_view_for_save =
              [this](const std::filesystem::path& path,
                     editor::TextViewport& viewport,
                     std::string* error_message,
                     SaveMode mode) {
                return PrepareEditorViewportForSave(path, viewport, error_message, mode);
              },
          .apply_editor_preferences =
              [this](editor::TextViewport& viewport) { ApplyEditorPreferences(viewport); },
          .apply_detected_indent_on_open =
              [this](editor::TextViewport& viewport) { ApplyDetectedIndentOnOpen(viewport); },
          .make_editor_tab_state =
              [](const editor::TextViewport& viewport) { return MakeEditorTabState(viewport); },
          .editor_view_path =
              [this](const TabEntry::EditorTabState& editor_state) {
                return EditorViewPath(editor_state);
              },
          .reveal_selected_tree_sidebar_line = [this]() { RevealSelectedTreeSidebarLine(); },
          .reveal_active_compare_selection = [this]() { RevealActiveCompareSelection(); },
          .reveal_active_merge_selection = [this]() { RevealActiveMergeSelection(); },
          .ensure_active_tab_visible = [this]() { tab_strip_chrome_.EnsureActiveTabVisible(); },
          .ensure_all_active_tabs_visible =
              [this]() { tab_strip_chrome_.EnsureActiveTabVisibleForAllGroups(); },
          .reset_caret_blink = [this]() { ResetCaretBlink(); },
          .request_active_tab_redraw =
              [this](bool include_tree_sidebar) { RequestActiveTabRedraw(include_tree_sidebar); },
          .request_tab_strip_redraw = [this]() { RequestTabStripRedraw(); },
          .invalidate_tab_strip_geometry =
              [this]() { tab_strip_service_.InvalidateTabStripGeometry(); },
          .request_editor_surface_redraw = [this]() { RequestEditorSurfaceRedraw(); },
          .editor_group_rects =
              [this]() {
                const auto layout = CurrentWorkspaceLayout();
                return layout.has_value() ? ComputeEditorGroupRectsForState(*layout)
                                          : EditorGroupRectsLayout{};
              },
          .request_automatic_git_sidebar_refresh =
              [this]() { RequestAutomaticGitSidebarRefresh(); },
          .request_external_change_banner =
              [this](const std::filesystem::path& path) {
                SetEditorBanner(context_.current_project_state,
                                EditorBannerState::Kind::ExternalChange, path);
                RequestEditorSurfaceRedraw();
              },
          .notify_save_failed =
              [this](const std::filesystem::path& path) {
                const std::string name =
                    path.empty() ? std::string("file") : path.filename().string();
                Notify(NotificationService::Tone::Error, "Failed to save " + name);
              },
          .begin_async_file_read =
              [this](const std::filesystem::path& path) {
                // The whole load runs on the reader's thread: the classification
                // (content hash, encoding sniff, line-ending scan, CRLF rewrite)
                // AND the buffer build, which `PieceTree::RebuildFromOriginal`
                // itself calls the dominant cost of opening a file — a newline
                // index over every byte, plus its reservation. A `TextViewport`
                // owns its document and touches no process-wide mutable state
                // while being built (the perf counters it bumps are atomic, the
                // trace channel is thread-aware, and nothing on this path consults
                // the syntax registry), so it is constructed here and MOVED into
                // the tab by the completion. The shared slot is written by the
                // worker before it posts and read by the completion after the
                // post, which is what orders the two.
                // A file this big takes long enough that an empty editor with no
                // explanation reads as a bug. Sticky, because it reports a state
                // that ends when the read does — and posted only above a size
                // where the wait is actually perceptible, so an ordinary open
                // does not flash a toast for two frames.
                if (FileIsBigEnoughToAnnounceTheRead(path)) {
                  Notify(NotificationService::Request{
                      .tone = NotificationService::Tone::Info,
                      .key = std::string(kFileOpenInProgressNotificationKey),
                      .message = "Opening " + path.filename().string() + "…",
                      .sticky = true,
                  });
                }
                auto loaded = std::make_shared<editor::TextViewport>();
                return file_read_service_.Begin({
                    .path = path,
                    .on_worker =
                        [loaded, path](std::string& bytes) {
                          (void)loaded->AdoptFileContent(path, std::move(bytes));
                        },
                    .on_complete =
                        [this, loaded](project::FileReadService::Completion completion) {
                          // Dismissed here rather than in ApplyAsyncFileRead: a
                          // completion whose tab is gone never reaches that, and a
                          // sticky row nothing dismisses stays on screen forever.
                          if (notification_service_.DismissKey(
                                  kFileOpenInProgressNotificationKey)) {
                            RequestFullRedraw();
                          }
                          ApplyAsyncFileRead(std::move(completion), std::move(*loaded));
                        },
                });
              },
          .cancel_async_file_read = [this](std::uint64_t id) { file_read_service_.Cancel(id); },
      });
}

EditorTabService& WorkspaceShell::MakeEditorTabService() {
  if (glue_->editor_tab_service != nullptr) {
    return *glue_->editor_tab_service;
  }
  glue_->editor_tab_service = std::make_unique<EditorTabService>(MakeTabCoordinator());
  return *glue_->editor_tab_service;
}

bool WorkspaceShell::SaveTab(std::size_t index, SaveMode mode) {
  std::lock_guard<std::mutex> lock(save_tab_mutex_);
  return MakeEditorTabService().Save(index, mode);
}

bool WorkspaceShell::SaveGroupTab(std::size_t group_index, std::size_t index, SaveMode mode) {
  std::lock_guard<std::mutex> lock(save_tab_mutex_);
  return MakeEditorTabService().SaveGroupTab(group_index, index, mode);
}

void WorkspaceShell::ReportSaveFormatterFailure(
    const SaveFormatterService::Completion& completion, std::string* error_message) {
  const std::string what = completion.timed_out ? "timed out" : "failed";
  if (error_message != nullptr) {
    *error_message = "formatter '" + completion.formatter_id + "' " + what;
  }

  // Everything the formatter said, in a channel the user can scroll. The warning
  // below used to be the whole of it — no exit status, no parse error, no line
  // number — which for a formatter that fails on ONE file and not the others is
  // the only question worth answering. The subprocess layer captured stderr all
  // along and this path dropped it.
  static constexpr std::string_view kFormatterChannelId = "formatter";
  static constexpr std::string_view kFormatterChannelLabel = "Formatter";
  // Bounded: a formatter that fails per line could otherwise put its whole
  // opinion of the file in the panel, and the channel store keeps every line.
  constexpr std::size_t kMaxReportedLines = 50;
  std::string first_line;
  if (!completion.error_text.empty()) {
    output_channels_.AppendLine(kFormatterChannelId, kFormatterChannelLabel,
                                "[" + completion.formatter_id + "] " + what);
    std::size_t start = 0;
    std::size_t emitted = 0;
    while (start < completion.error_text.size() && emitted < kMaxReportedLines) {
      std::size_t end = completion.error_text.find('\n', start);
      if (end == std::string::npos) {
        end = completion.error_text.size();
      }
      std::string_view line(completion.error_text.data() + start, end - start);
      while (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
      }
      if (!line.empty()) {
        if (first_line.empty()) {
          first_line.assign(line);
        }
        output_channels_.AppendLine(kFormatterChannelId, kFormatterChannelLabel,
                                    std::string(line));
        ++emitted;
      }
      start = end + 1;
    }
    if (emitted == kMaxReportedLines && start < completion.error_text.size()) {
      output_channels_.AppendLine(kFormatterChannelId, kFormatterChannelLabel,
                                  "… output truncated");
    }
  }

  // The file still saves (unformatted); warn so the silent formatter failure is
  // visible rather than swallowed. The first line of stderr rides along, because
  // a toast saying only "failed" sends the user looking for a panel they have no
  // reason to know exists. NotificationService byte-caps the result.
  std::string message =
      "Formatter '" + completion.formatter_id + "' " + what + "; saved unformatted";
  if (!first_line.empty()) {
    message += " — " + first_line;
  }
  Notify(NotificationService::Tone::Warning, std::move(message));
}

void WorkspaceShell::ApplyDeferredSaveFormat(
    const SaveFormatterService::Completion& completion) {
  if (completion.id == 0) {
    return;
  }
  // Find the tab that posted this run. A walk rather than a map: run ids are
  // process-monotonic so at most one tab carries this one, the tab may have moved
  // group or index while the formatter ran, and the walk costs one pass over the
  // open tabs once per save.
  for (std::size_t group_index = 0;
       group_index < context_.current_project_state.editor_groups.size(); ++group_index) {
    EditorGroup& group = context_.current_project_state.editor_groups[group_index];
    for (std::size_t tab_index = 0; tab_index < group.open_tabs.size(); ++tab_index) {
      TabEntry& tab = group.open_tabs[tab_index];
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
          !tab.editor_state->pending_format_save.Holds(completion.id)) {
        continue;
      }
      auto& editor_state = *tab.editor_state;
      const editor::AsyncBufferWork::Claim claim = editor_state.pending_format_save.Resolve(
          completion.id, editor_state.viewport.content_revision());
      if (!completion.ok) {
        if (!completion.formatter_id.empty()) {
          ReportSaveFormatterFailure(completion, nullptr);
        }
      } else if (!completion.formatted_text.empty()) {
        if (claim == editor::AsyncBufferWork::Claim::Current) {
          editor_state.viewport.ReloadPreservingViewState(completion.formatted_text);
          editor_state.viewport.SetDirty(true);
        } else {
          // The user typed while the formatter ran. Applying its output now would
          // silently undo that edit, so the buffer as it stands is what gets written
          // and the formatter's answer is dropped.
          Notify(NotificationService::Tone::Info,
                 "Buffer changed while formatting; saved unformatted");
        }
      }
      // One re-entry, with the formatter suppressed, so this lands exactly one write
      // and cannot post another run.
      const bool close_after_save = editor_state.close_after_save;
      editor_state.close_after_save = false;
      editor_state.skip_formatter_once = true;
      const bool saved = SaveGroupTab(group_index, tab_index, SaveMode::Blocking);
      // The tab was closed with unsaved edits and the user chose Save, so the
      // close was waiting on this write. Only now — closing on a FAILED write
      // would discard exactly the edits they asked to keep, so a refused save
      // leaves the tab open with its contents intact.
      if (close_after_save && saved) {
        MakeEditorTabService().CloseGroupTab(group_index, tab_index);
      }
      return;
    }
  }
}

void WorkspaceShell::ApplyAsyncFileRead(project::FileReadService::Completion completion,
                                       editor::TextViewport loaded) {
  if (completion.id == 0) {
    return;
  }
  // Find the tab that posted this read. A walk, for the same reason the deferred
  // save's completion walks: read ids are process-monotonic so at most one tab
  // carries this one, the tab may have moved group or index while the read ran,
  // and the walk costs one pass over the open tabs once per open.
  for (std::size_t group_index = 0;
       group_index < context_.current_project_state.editor_groups.size(); ++group_index) {
    EditorGroup& group = context_.current_project_state.editor_groups[group_index];
    for (TabEntry& tab : group.open_tabs) {
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
          !tab.editor_state->pending_load.Holds(completion.id)) {
        continue;
      }
      auto& editor_state = *tab.editor_state;
      // Resolved only to CONSUME the slot (a duplicate delivery then reads
      // NotMine). The claim itself is not the decision here: `Holds` above already
      // matched the id, and a Loading tab is read-only, so its content revision
      // cannot have moved — the Content check below is what decides.
      (void) editor_state.pending_load.Resolve(completion.id,
                                               editor_state.viewport.content_revision());
      if (editor_state.content != TabEntry::EditorTabState::Content::Loading) {
        // Something already gave this tab content — a retarget, a reload, a
        // session restore. Whatever it installed is newer than these bytes.
        return;
      }
      if (completion.status == project::FileReadService::Status::Cancelled) {
        // The tab is on its way out, or the read was superseded. Leave the state
        // alone: whoever cancelled owns what happens next.
        return;
      }
      const std::filesystem::path path = completion.path;
      if (!completion.ok()) {
        // The tab stays, showing which file it was. Dropping it would make a
        // failed open look like a click that did nothing.
        editor_state.content = TabEntry::EditorTabState::Content::Failed;
        const std::string name = path.empty() ? std::string("file") : path.filename().string();
        Notify(NotificationService::Tone::Error,
               completion.status == project::FileReadService::Status::TooLarge
                   ? "Cannot open " + name + ": file is too large"
                   : "Could not read " + name);
        RequestEditorSurfaceRedraw();
        return;
      }
      // The loaded view replaces the empty read-only stand-in wholesale, and then
      // gets the preferences and indent detection a synchronous open applies.
      // (Every other tab waiting on this path is handed a copy below, so a pane
      // that was sharing the stand-in's document does not keep pointing at it.)
      // One file is one buffer. If another surface made the file live while this
      // read ran — a compare tab opened, or finished its own load, on the same
      // path — share THAT document rather than installing a second, independent
      // copy of the same file: two buffers diverge on the first edit and save
      // over each other. The stand-in is excluded by `content_pending()`.
      if (const editor::TextViewport* live =
              LiveBufferViewOfPath(context_.current_project_state.editor_groups, path);
          live != nullptr) {
        editor_state.viewport = *live;
      } else {
        editor_state.viewport = std::move(loaded);
      }
      ApplyEditorPreferences(editor_state.viewport);
      ApplyDetectedIndentOnOpen(editor_state.viewport);
      // View state last: preferences re-run EnsureCursorVisible, which would snap
      // scroll back onto the caret if it ran after the restore.
      editor_state.viewport.ApplyRestoredViewState(editor_state.restored_cursor_line,
                                                   editor_state.restored_cursor_column,
                                                   editor_state.restored_scroll_line,
                                                   editor_state.restored_horizontal_scroll);
      editor_state.content = TabEntry::EditorTabState::Content::Ready;
      editor_state.folding_model->Clear();
      NotifyPluginBufferOpen(path);
      const bool is_active = group_index == context_.current_project_state.focused_group_index &&
                             &tab == &group.open_tabs[group.active_tab_index];
      if (is_active) {
        SyncActiveEditorTabMetadata();
        lsp_service_.ScheduleBufferOpen(path);
      }
      ShareLoadedBufferWithWaitingTabs(path, editor_state.viewport);
      RequestEditorSurfaceRedraw();
      RequestTabStripRedraw();
      return;
    }
  }
}

void WorkspaceShell::ShareLoadedBufferWithWaitingTabs(const std::filesystem::path& path,
                                                      const editor::TextViewport& loaded) {
  // One file is one buffer, whoever reads it. A split made while the read was in
  // flight, or a second open of the same path, leaves other tabs waiting on the
  // same bytes; handing them a copy of this view shares the DocumentState rather
  // than reading the file again, which is what the synchronous open has always
  // done through `OpenEditorViewForPath`.
  for (EditorGroup& group : context_.current_project_state.editor_groups) {
    for (TabEntry& tab : group.open_tabs) {
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
        continue;
      }
      auto& waiting = *tab.editor_state;
      if (!waiting.content_pending() || &waiting.viewport == &loaded ||
          waiting.restored_path.lexically_normal() != path) {
        continue;
      }
      // Its own read, if it posted one, is now pointless work on a file already
      // in memory.
      if (waiting.pending_load.armed()) {
        file_read_service_.Cancel(waiting.pending_load.id());
        waiting.pending_load.Disarm();
      }
      waiting.viewport = loaded;  // shares the DocumentState; view state is its own
      waiting.viewport.ApplyRestoredViewState(waiting.restored_cursor_line,
                                              waiting.restored_cursor_column,
                                              waiting.restored_scroll_line,
                                              waiting.restored_horizontal_scroll);
      waiting.content = TabEntry::EditorTabState::Content::Ready;
      waiting.folding_model->Clear();
    }
  }
}

void WorkspaceShell::MaybeAutosaveDirtyTabs(bool on_focus_change) {
  const std::string mode = GetSettingValue("editor.autosave").value_or("off");
  const bool trigger =
      on_focus_change ? (mode == "on_focus_change") : (mode == "after_delay");
  if (!trigger) {
    return;
  }
  // TabCoordinator::Save refuses untitled buffers and surfaces the external-change
  // banner on a disk conflict, so autosave never pops a dialog or clobbers a file
  // changed on disk. Suppress the synchronous format-on-save subprocess for the
  // duration: an autosave (focus change / after delay) must not stall the UI thread
  // on an external formatter — most visibly during window blur / alt-tab.
  autosave_suppress_format_on_save_ = true;
  // Flush every dirty tab across ALL editor groups, not just the focused one, so a
  // buffer dirtied in the non-focused split group is autosaved too (VSCode parity).
  for (const GroupTabRef& ref : MakeEditorTabService().DirtyGroupTabs()) {
    SaveGroupTab(ref.group_index, ref.tab_index);
  }
  autosave_suppress_format_on_save_ = false;
}

Uint64 WorkspaceShell::AutosaveDelayMs() const {
  const int parsed = util::ParseIntOr(GetSettingValue("editor.autosave.delay_ms"), 1000);
  return static_cast<Uint64>(std::clamp(parsed, 200, 60000));
}

void WorkspaceShell::MaybeArmAutosaveTimer() {
  // Only the "after delay" mode uses the debounce; other modes leave the timer idle.
  if (GetSettingValue("editor.autosave").value_or("off") != "after_delay") {
    autosave_armed_ = false;
    autosave_last_viewport_ = nullptr;
    return;
  }
  // Detect a real buffer mutation via the active editable viewport's content revision.
  // A path-less (untitled) or absent buffer has nothing to autosave, so we do not arm.
  const editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return;
  }
  // content_revision() is per-viewport, so switching tabs changes the sampled value
  // without any edit having occurred. Re-baseline against the newly-active viewport
  // WITHOUT resetting the debounce or disarming: a timer armed by an edit on the
  // previous tab must survive the switch so that buffer still autosaves — the flush
  // (MaybeAutosaveDirtyTabs) saves every dirty tab, not just the active one.
  if (viewport != autosave_last_viewport_) {
    autosave_last_viewport_ = viewport;
    autosave_last_content_revision_ = viewport->content_revision();
    return;
  }
  const std::uint64_t revision = viewport->content_revision();
  if (revision == autosave_last_content_revision_) {
    return;  // No edit since the last sample (navigation/focus only): keep the debounce.
  }
  autosave_last_content_revision_ = revision;
  autosave_edit_epoch_ms_ = SDL_GetTicks();
  // Arm while any saveable tab is dirty, not only the active one (and across all
  // editor groups, so a dirty non-focused-group tab keeps the flush armed): an edit
  // that reverts the active buffer to clean must still leave the timer armed to flush
  // another dirty tab.
  autosave_armed_ = !MakeEditorTabService().DirtyGroupTabs().empty();
}

void WorkspaceShell::MaybeArmSessionFlushTimer() {
  // Always-on crash-safety debounce (no editor.autosave gate). Detect a real buffer
  // mutation via the active editable viewport's content revision, mirroring the autosave
  // arm, but WITHOUT the path-backed gate so untitled dirty buffers are covered too.
  const editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr) {
    return;
  }
  // content_revision() is per-viewport; a tab switch changes the sampled value with no
  // edit. Re-baseline against the newly-active viewport WITHOUT resetting the debounce
  // or disarming, so a flush armed by an edit on another tab still fires (SaveSessionState
  // persists every dirty tab, not just the active one).
  if (viewport != session_flush_last_viewport_) {
    session_flush_last_viewport_ = viewport;
    session_flush_last_content_revision_ = viewport->content_revision();
    return;
  }
  const std::uint64_t revision = viewport->content_revision();
  if (revision == session_flush_last_content_revision_) {
    return;  // Navigation/focus only: keep any pending debounce.
  }
  session_flush_last_content_revision_ = revision;
  session_flush_edit_epoch_ms_ = SDL_GetTicks();
  // Arm only when there is unsaved content somewhere to persist (across all groups).
  session_flush_armed_ = !MakeEditorTabService().DirtyGroupTabs().empty();
}

std::optional<Uint32> WorkspaceShell::NextAutosaveDelayMs() const {
  if (!autosave_armed_) {
    return std::nullopt;
  }
  const Uint64 delay = AutosaveDelayMs();
  const Uint64 elapsed = SDL_GetTicks() - autosave_edit_epoch_ms_;
  if (elapsed >= delay) {
    return static_cast<Uint32>(1);  // Deadline passed: wake immediately to save.
  }
  return static_cast<Uint32>(std::max<Uint64>(1, delay - elapsed));
}

SavePreparation WorkspaceShell::PrepareEditorViewportForSave(const std::filesystem::path& path,
                                                             editor::TextViewport& viewport,
                                                             std::string* error_message,
                                                             SaveMode mode) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (path.empty()) {
    return SavePreparation::Ready();
  }

  // Fast-return BEFORE serializing the whole buffer when no save transform can run:
  // no active plugin save participants AND no enabled formatter for this filetype.
  // The common no-plugin/no-formatter save then pays zero preparation serialization
  // (TextViewport::Save does its own single serialize), instead of the previous
  // two-or-three full-buffer passes. (TD-2026-07-16-16.)
  const bool has_save_participants =
      plugin_runtime_.enabled() && !save_participant_registry_.Specs().empty();
  // Every current caller passes viewport.path(), so this is the viewport's own
  // memo — no buffer head scan on the save path. `path` is still the target (it
  // is what save participants receive), so if a save-as ever routes a different
  // one, detect from the DESTINATION's name: saving a Python buffer as `.lua`
  // should pick the lua formatter, not the source buffer's language.
  std::string destination_filetype;
  if (path != viewport.path()) {
    destination_filetype = editor::runtime_syntax::DetectFiletype(path);
  }
  const std::string_view filetype = path == viewport.path()
                                        ? std::string_view(viewport.language_id())
                                        : std::string_view(destination_filetype);
  // Autosave suppresses the formatter so a background write never blocks the UI thread
  // on an external subprocess; explicit saves still format.
  const bool format_on_save = !autosave_suppress_format_on_save_ &&
                              mode != SaveMode::SkipFormatter &&
                              SettingFlagEnabled(GetSettingValue("editor.format_on_save"), true);
  const FormatterSpec* formatter =
      (format_on_save && !filetype.empty()) ? FindFormatter(formatter_registry_, filetype)
                                            : nullptr;
  const bool has_formatter = formatter != nullptr && !formatter->command.empty();
  if (!has_save_participants && !has_formatter) {
    return SavePreparation::Ready();  // nothing transforms the text; skip serializing
  }

  std::string text = SerializeViewportText(viewport);
  const std::string original_text = text;  // to detect whether a transform changed it
  if (has_save_participants &&
      !plugin_runtime_.Host().RunSaveParticipants(path, &text, error_message)) {
    return SavePreparation::Failed();
  }

  if (has_formatter) {
    // The formatter is a subprocess. Bounded by a deadline, yes, but starting node is
    // a few hundred milliseconds and the cap is five seconds — inline on the shell
    // thread that is the whole window frozen, on every save. It runs on
    // SaveFormatterService's worker now.
    constexpr int kFormatterTimeoutMs = 5000;
    // Through the project's launcher: a formatter must run where the file it is
    // formatting lives, or it reformats against the wrong toolchain's config.
    const platform::ProcessLauncher& launcher = context_.current_project_state.launcher();
    SaveFormatterService::Request request{
        .command = formatter->command,
        .cwd = context_.current_project_state.root,
        .text = text,
        .formatter_id = formatter->id,
        .timeout_ms = kFormatterTimeoutMs,
    };
    if (mode == SaveMode::Deferred) {
      // The participants' output (if any) is in `text` and goes with the run, but it
      // is NOT applied to the viewport yet: the completion decides, against the
      // revision, whether the formatter's answer is still about this buffer.
      const std::uint64_t run_id = save_formatter_service_.Begin(
          path.generic_string(), std::move(request), launcher,
          [this](SaveFormatterService::Completion completion) {
            ApplyDeferredSaveFormat(completion);
          });
      if (run_id != 0) {
        return SavePreparation::Deferred(run_id);
      }
    } else {
      // A save whose caller acts on completion — closing a tab, renaming, quitting —
      // still has to have the file on disk when it returns. It pays the same wait the
      // inline path always did, off the shell thread's own stack.
      const SaveFormatterService::Completion completion =
          save_formatter_service_.RunBlocking(std::move(request), launcher);
      if (!completion.ok) {
        ReportSaveFormatterFailure(completion, error_message);
        return SavePreparation::Ready();
      }
      if (!completion.formatted_text.empty()) {
        text = completion.formatted_text;
      }
    }
  }

  // Compare against the snapshot captured before transforms, not a fresh re-serialize:
  // when no participant/formatter changed the text there is nothing to apply. (TD-16.)
  if (text == original_text) {
    return SavePreparation::Ready();
  }

  viewport.ReloadPreservingViewState(text);
  viewport.SetDirty(true);
  return SavePreparation::Ready();
}

bool WorkspaceShell::OpenVirtualDocumentInNewTab(std::string_view uri) {
  if (context_.current_project_state.root.empty() || uri.empty()) {
    return false;
  }

  const VirtualDocumentSpec* document = virtual_document_registry_.GetDocument(std::string(uri));
  if (document == nullptr) {
    return false;
  }
  return MakeEditorTabService().OpenVirtualDocumentInNewTab(std::filesystem::path(document->uri),
                                                            document->content, document->uri);
}

void WorkspaceShell::ReloadVirtualDocumentTabs(std::string_view uri) {
  if (uri.empty()) {
    return;
  }

  const VirtualDocumentSpec* document = virtual_document_registry_.GetDocument(std::string(uri));
  if (document == nullptr) {
    return;
  }
  MakeEditorTabService().ReloadVirtualDocumentTabs(std::filesystem::path(document->uri),
                                                   document->content);
}

bool WorkspaceShell::IsReadOnlyVirtualDocument(const std::filesystem::path& path) const {
  if (path.empty()) {
    return false;
  }
  for (const std::string& uri : virtual_document_registry_.DocumentUris()) {
    if (std::filesystem::path(uri) != path) {
      continue;
    }
    const VirtualDocumentSpec* document = virtual_document_registry_.GetDocument(uri);
    return document != nullptr && !document->editable;
  }
  return false;
}

bool WorkspaceShell::TabIsDirty(std::size_t index) const {
  return const_cast<WorkspaceShell*>(this)->MakeEditorTabService().IsDirty(index);
}

std::string WorkspaceShell::TabDisplayTitle(std::size_t index) const {
  return TabDisplayTitle(context_.current_project_state.focused_group_index, index);
}

namespace {

// The file a tab's labels should name. A compare or merge tab's own `path` is not
// it — those carry the surface's identity, not the document's. Returned by
// reference so the label builders do not copy a path just to read it.
const std::filesystem::path& TabLabelPath(const TabEntry& tab) {
  if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
    return tab.compare->path;
  }
  if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value()) {
    return tab.merge->output_path;
  }
  return tab.path;
}

}  // namespace

const TabEntry* WorkspaceShell::TabForLabels(std::size_t group_index, std::size_t index) const {
  const ProjectWorkspaceState& state = context_.current_project_state;
  if (group_index >= state.editor_groups.size() ||
      index >= state.editor_groups[group_index].open_tabs.size()) {
    return nullptr;
  }
  return &state.editor_groups[group_index].open_tabs[index];
}

WorkspaceTabTextModel WorkspaceShell::TabTextModel(std::size_t group_index,
                                                   std::size_t index) const {
  const TabEntry* tab = TabForLabels(group_index, index);
  if (tab == nullptr) {
    return {};
  }
  return BuildWorkspaceTabTextModel(context_.current_project_state.root, TabLabelPath(*tab),
                                    tab->title, TabCoordinator::TabStateIsDirty(*tab));
}

std::string WorkspaceShell::TabDisplayTitle(std::size_t group_index, std::size_t index) const {
  // NOT `TabTextModel(...).display_title`: that also resolves the tooltip's
  // project-relative label, which the tab strip asks for through its own provider
  // anyway — so every tab paid for it twice (TD-2026-08-06-159).
  const TabEntry* tab = TabForLabels(group_index, index);
  if (tab == nullptr) {
    return {};
  }
  std::string title = BuildWorkspaceTabDisplayTitle(TabLabelPath(*tab), tab->title,
                                                    TabCoordinator::TabStateIsDirty(*tab));
  // Two tabs in the group with the same file name are told apart by their
  // parent folder, "index.ts — src" (VS Code's tab description). Without it both
  // tabs read identically and only the tooltip knew which was which. A linear
  // scan over the group's tabs, once per strip geometry rebuild per tab.
  const std::filesystem::path& path = TabLabelPath(*tab);
  if (path.empty() || !path.has_parent_path()) {
    return title;
  }
  const auto& tabs = context_.current_project_state.editor_groups[group_index].open_tabs;
  const std::filesystem::path filename = path.filename();
  for (std::size_t other = 0; other < tabs.size(); ++other) {
    if (other == index) {
      continue;
    }
    const std::filesystem::path& other_path = TabLabelPath(tabs[other]);
    if (!other_path.empty() && other_path != path && other_path.filename() == filename) {
      const std::string parent = path.parent_path().filename().string();
      if (!parent.empty()) {
        title += " \xe2\x80\x94 ";  // " — "
        title += parent;
      }
      break;
    }
  }
  return title;
}

std::string WorkspaceShell::TabTooltipLabel(std::size_t index) const {
  return TabTooltipLabel(context_.current_project_state.focused_group_index, index);
}

std::string WorkspaceShell::TabTooltipLabel(std::size_t group_index, std::size_t index) const {
  const TabEntry* tab = TabForLabels(group_index, index);
  if (tab == nullptr) {
    return {};
  }
  return BuildWorkspaceTabTooltipLabel(context_.current_project_state.root, TabLabelPath(*tab),
                                       tab->title);
}

std::vector<std::size_t> WorkspaceShell::DirtyEditorTabIndices() const {
  return const_cast<WorkspaceShell*>(this)->MakeEditorTabService().DirtyIndices();
}

std::vector<std::size_t> WorkspaceShell::DirtyEditorTabIndices(
    const ProjectWorkspaceState& state) {
  std::vector<std::size_t> dirty_tabs;
  dirty_tabs.reserve(state.focused_group().open_tabs.size());
  for (std::size_t i = 0; i < state.focused_group().open_tabs.size(); ++i) {
    if (TabCoordinator::TabStateIsDirty(state.focused_group().open_tabs[i])) {
      dirty_tabs.push_back(i);
    }
  }
  return dirty_tabs;
}

std::vector<std::size_t> WorkspaceShell::DirtyEditorTabIndicesForProject(
    std::size_t project_index) const {
  return const_cast<WorkspaceShell*>(this)->MakeEditorTabService().DirtyIndicesForProject(project_index);
}

bool WorkspaceShell::HasDirtyEditorTabForProject(std::size_t project_index) const {
  return const_cast<WorkspaceShell*>(this)->MakeEditorTabService().HasDirtyTabForProject(
      project_index);
}

std::vector<GroupTabRef> WorkspaceShell::DirtyEditorGroupTabs() const {
  return const_cast<WorkspaceShell*>(this)->MakeEditorTabService().DirtyGroupTabs();
}

std::vector<GroupTabRef> WorkspaceShell::DirtyEditorGroupTabsForProject(
    std::size_t project_index) const {
  return const_cast<WorkspaceShell*>(this)
      ->MakeEditorTabService()
      .DirtyGroupTabsForProject(project_index);
}

void WorkspaceShell::ReloadCleanEditorTabsForPath(const std::filesystem::path& path,
                                                  EditorReloadEchoGuard echo_guard) {
  MakeEditorTabService().ReloadCleanEditorTabsForPath(path, echo_guard);
}

bool WorkspaceShell::OpenUntitledTab() {
  return MakeEditorTabService().OpenUntitled();
}

bool WorkspaceShell::SplitEditorGroup(EditorSplitOrientation orientation) {
  return MakeEditorTabService().SplitEditorGroup(orientation);
}

bool WorkspaceShell::FocusOtherEditorGroup() {
  return MakeEditorTabService().FocusOtherGroup();
}

void WorkspaceShell::FocusEditorGroup(std::size_t group_index) {
  ProjectWorkspaceState& state = context_.current_project_state;
  if (group_index >= state.editor_groups.size() || group_index == state.focused_group_index) {
    state.surface.focus = FocusTarget::Editor;
    return;
  }
  state.focused_group_index = group_index;
  state.surface.focus = FocusTarget::Editor;
  RequestChromeRedraw();
  RequestEditorSurfaceRedraw();
}

bool WorkspaceShell::CloseEditorGroup() {
  if (EditorGroupCount() < 2) {
    return false;
  }
  // Closing a pane drops its tabs. A dirty buffer whose only view lives in this
  // pane used to go with them, silently; it gets the same Save / Discard / Cancel
  // as closing its tab (VS Code prompts here too). Route through the tab-close
  // path, which prompts, and collapses the pane once its last tab closes.
  const std::size_t tab_count = context_.current_project_state.focused_group().open_tabs.size();
  for (std::size_t index = 0; index < tab_count; ++index) {
    if (MakeEditorTabService().CloseWouldDiscardEdits(index)) {
      std::vector<std::size_t> indices(tab_count);
      for (std::size_t i = 0; i < tab_count; ++i) {
        indices[i] = i;
      }
      RequestCloseTabs(std::move(indices));
      return true;
    }
  }
  return MakeEditorTabService().CloseEditorGroup();
}

std::size_t WorkspaceShell::EditorGroupCount() const {
  return const_cast<WorkspaceShell*>(this)->MakeEditorTabService().EditorGroupCount();
}

bool WorkspaceShell::OpenFileInNewTab(const std::filesystem::path& path) {
  const bool opened = MakeEditorTabService().OpenFileInNewTab(path);
  if (opened) {
    // Record into the recent-files MRU, scoped to the active project. Resolve to an
    // absolute path so finder/welcome lookups match regardless of the caller's input.
    const std::filesystem::path& root = context_.current_project_state.root;
    if (!root.empty()) {
      const std::filesystem::path absolute = path.is_absolute() ? path : root / path;
      recents_service_.RecordFileOpen(absolute, root);
    }
  }
  return opened;
}

bool WorkspaceShell::MoveActiveTabTo(std::size_t index) {
  // Reordering the open_tabs vector invalidates the cached display_titles /
  // tooltip_labels / widths in TabStripService, which only key on
  // (tab_count, strip_width). Without this drop, the next ComputeVisibleTabs
  // call hits a stale cache and the rendered tab labels stay in the
  // pre-reorder positions even though the underlying tabs have moved — so
  // the tab strip shows the wrong labels while the active editor content
  // already reflects the new order.
  tab_strip_service_.InvalidateTabStripGeometry();
  return MakeEditorTabService().MoveActiveTo(index);
}

std::optional<std::size_t> WorkspaceShell::FindTabIndexBySpecifier(
    std::string_view specifier,
    std::string* error_message) const {
  return const_cast<WorkspaceShell*>(this)->MakeEditorTabService().FindIndexBySpecifier(
      specifier, error_message);
}

void WorkspaceShell::OpenFile(const std::filesystem::path& path) {
  (void)OpenFileInNewTab(path);
}

void WorkspaceShell::OpenFileAtLocation(const std::filesystem::path& path,
                                        std::size_t line,
                                        std::size_t column) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  OpenFile(path);

  editor::TextViewport* viewport = ActiveEditorViewport();
  if (viewport == nullptr || !util::SameAsNormalizedPath(viewport->path(), normalized_path)) {
    for (std::size_t i = 0; i < context_.current_project_state.focused_group().open_tabs.size(); ++i) {
      const auto& tab = context_.current_project_state.focused_group().open_tabs[i];
      if (tab.kind == TabEntry::Kind::Editor && tab.path == normalized_path) {
        ActivateTab(i);
        viewport = ActiveEditorViewport();
        break;
      }
    }
  }

  // Only move the caret once we have actually landed on the requested file. If the
  // open failed (per-group tab cap reached, unreadable file, …) the fallback search
  // finds no matching tab and `viewport` still points at the previously-active tab —
  // relocating its caret would scroll/jump the wrong buffer.
  if (viewport != nullptr && util::SameAsNormalizedPath(viewport->path(), normalized_path)) {
    viewport->JumpCursorTo(line, column);
  }
}

bool WorkspaceShell::ReopenActiveTab() {
  return MakeEditorTabService().ReopenActive();
}

}  // namespace microide::workspace
