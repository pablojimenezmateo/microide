#include <memory>

#include "workspace/shell/WorkspaceShell.h"
#include "workspace/services/RemoteHostService.h"
#include "workspace/shell/ShellGlueCache.h"
#include "workspace/coordinators/WorkspaceDirtyPromptCoordinator.h"
#include "workspace/coordinators/WorkspacePathMutationCoordinator.h"
#include "workspace/services/PromptSurfaceService.h"

#include "workspace/SettingFlags.h"
#include "workspace/EditorPreferenceSettings.h"
#include "workspace/WorkspaceIndentDetectApply.h"

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
#include "util/ErrorOut.h"
#include "workspace/services/SidebarService.h"

namespace microide::workspace {

namespace {

constexpr std::uintmax_t kAnnounceReadAboveBytes = 48ull * 1024 * 1024;

bool FileIsBigEnoughToAnnounceTheRead(const std::filesystem::path& path) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  return !error && size >= kAnnounceReadAboveBytes;
}

}

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
                lsp_service_.ScheduleBufferOpen(path);
                RequestPreparedFrame();
              },
          .notify_lsp_buffer_close =
              [this](const std::filesystem::path& path) {
                NotifyLspBufferClose(path);
                (void)MakeRemoteHostService().FinishTerminalEdit(path);
              },
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
              [](const TabEntry::EditorTabState& editor_state) {
                return EditorViewPath(editor_state);
              },
          .reveal_selected_tree_sidebar_line = [this]() { MakeSidebarService().RevealSelectedTreeLine(); },
          .reveal_active_compare_selection = [this]() { RevealActiveCompareSelection(); },
          .reveal_active_merge_selection = [this]() { RevealActiveMergeSelection(); },
          .ensure_active_tab_visible = [this]() { tab_strip_chrome_.EnsureActiveTabVisible(); },
          .ensure_all_active_tabs_visible =
              [this]() { tab_strip_chrome_.EnsureActiveTabVisibleForAllGroups(); },
          .reset_caret_blink = [this]() { ResetCaretBlink(); },
          .request_frame = [this]() { RequestPreparedFrame(); },
          .invalidate_tab_strip_geometry =
              [this]() { tab_strip_service_.InvalidateTabStripGeometry(); },
          .editor_group_rects =
              [this]() {
                const auto layout = CurrentWorkspaceLayout();
                return layout.has_value() ? ComputeEditorGroupRectsForState(*layout)
                                          : EditorGroupRectsLayout{};
              },
          .request_automatic_git_sidebar_refresh =
              [this]() { RequestAutomaticGitSidebarRefresh(); },
          .tab_closed = [this](std::uint64_t stable_id) { MakePromptSurfaceService().OnTabClosed(stable_id); },
          .request_external_change_banner =
              [this](const std::filesystem::path& path) {
                SetEditorBanner(context_.current_project_state,
                                EditorBannerState::Kind::ExternalChange, path);
                RequestPreparedFrame();
              },
          .notify_save_failed =
              [this](const std::filesystem::path& path, std::string_view reason) {
                const std::string name =
                    path.empty() ? std::string("file") : path.filename().string();
                std::string message = "Failed to save " + name;
                if (!reason.empty()) {
                  message += ": ";
                  message += reason;
                }
                Notify(NotificationService::Tone::Error, message);
              },
          .begin_async_file_read =
              [this](const std::filesystem::path& path) {
                if (FileIsBigEnoughToAnnounceTheRead(path)) {
                  Notify(NotificationService::Request{
                      .tone = NotificationService::Tone::Info,
                      .key = std::string(kFileOpenInProgressNotificationKey),
                      .message = "Opening " + path.filename().string() + "…",
                      .sticky = true,
                  });
                }
                auto loaded = std::make_shared<editor::TextViewport>();
                const EditorPreferenceSettings settings = ResolveEditorPreferenceSettings(
                    [this](std::string_view id) { return GetSettingValue(id); });
                project::EditorConfigProperties editor_config;
                if (settings.editorconfig_enabled) {
                  context_.current_project_state.editor_config.EnsureProjectRoot(
                      context_.current_project_state.root);
                  editor_config = context_.current_project_state.editor_config.Resolve(path);
                }
                const EditorPreferences preferences = context_.current_project_state.editor_preferences;
                std::optional<std::string> detect_on_open = GetSettingValue("editor.indent.detect_on_open");
                return file_read_service_.Begin({
                    .path = path,
                    .on_worker =
                        [loaded, path, editor_config = editor_config, preferences,
                         detect_on_open = std::move(detect_on_open)](std::string& bytes) {
                          if (!loaded->AdoptFileContent(path, std::move(bytes))) {
                            return;
                          }
                          if (preferences.soft_wrap) {
                            return;
                          }
                          loaded->SetTabSize(editor_config.tab_size.value_or(preferences.tab_size));
                          loaded->SetIndentWidth(
                              editor_config.indent_width.value_or(preferences.indent_width));
                          loaded->SetSoftTabs(editor_config.soft_tabs.value_or(preferences.soft_tabs));
                          ApplyDetectedIndentAfterPreferences(
                              *loaded, [&](std::string_view) { return detect_on_open; },
                              editor_config);
                          loaded->PrebuildLineWidths();
                        },
                    .on_complete =
                        [this, loaded](project::FileReadService::Completion completion) {
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
  util::SetError(error_message, "formatter '" + completion.formatter_id + "' " + what);

  static constexpr std::string_view kFormatterChannelId = "formatter";
  static constexpr std::string_view kFormatterChannelLabel = "Formatter";
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

  std::string message =
      "Formatter '" + completion.formatter_id + "' " + what + "; saved unformatted";
  if (!first_line.empty()) {
    message += " — " + first_line;
  }
  NotificationService::Request request{.tone = NotificationService::Tone::Warning,
                                       .message = std::move(message)};
  if (!completion.error_text.empty()) {
    request.actions.push_back(NotificationAction{
        .label = "Show Output",
        .id = ActionId::ShowOutput,
        .args = {std::string(kFormatterChannelId)},
    });
  }
  Notify(std::move(request));
}

void WorkspaceShell::ApplyDeferredSaveFormat(
    const SaveFormatterService::Completion& completion) {
  if (completion.id == 0) {
    return;
  }
  for (std::size_t group_index = 0;
       group_index < context_.current_project_state.editor_groups.size(); ++group_index) {
    EditorGroup& group = context_.current_project_state.editor_groups[group_index];
    for (std::size_t tab_index = 0; tab_index < group.open_tabs.size(); ++tab_index) {
      TabEntry& tab = group.open_tabs[tab_index];
      editor::AsyncBufferWork* work = nullptr;
      editor::TextViewport* view = nullptr;
      if (tab.kind == TabEntry::Kind::Editor && tab.editor_state.has_value()) {
        work = &tab.editor_state->pending_format_save;
        view = &tab.editor_state->viewport;
      } else if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
        work = &tab.compare->pending_format_save;
        view = &tab.compare->right_viewport;
      } else if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value()) {
        work = &tab.merge->pending_format_save;
        view = &tab.merge->result_viewport;
      }
      if (work == nullptr || !work->Holds(completion.id)) {
        continue;
      }
      const editor::AsyncBufferWork::Claim claim =
          work->Resolve(completion.id, view->content_revision());
      const std::uint64_t tab_id = tab.stable_id;
      EditorTabService& editor_tabs = MakeEditorTabService();
      PromptSurfaceService& prompt_surfaces = MakePromptSurfaceService();
      if (completion.cancel_save) {
        Notify(NotificationService::Tone::Error,
               completion.error_text.empty()
                   ? std::string("A save participant failed; not saved")
                   : "A save participant failed; not saved: " + completion.error_text);
        MakeDirtyPromptCoordinator(editor_tabs, prompt_surfaces).SettleSave(tab_id, false);
        RequestPreparedFrame();
        return;
      }
      if (completion.participants_timed_out) {
        Notify(NotificationService::Tone::Warning,
               "save participants timed out; saved without plugin transforms");
      }
      if (!completion.ok && !completion.formatter_id.empty()) {
        ReportSaveFormatterFailure(completion, nullptr);
      }
      if (!completion.formatted_text.empty()) {
        if (claim == editor::AsyncBufferWork::Claim::Current) {
          (void)view->ApplyFormattedText(completion.formatted_text);
          view->SetDirty(true);
        } else {
          Notify(NotificationService::Tone::Info,
                 "Buffer changed while formatting; saved unformatted");
        }
      }
      const bool saved = SaveGroupTab(group_index, tab_index, SaveMode::SkipTransforms);
      MakeDirtyPromptCoordinator(editor_tabs, prompt_surfaces).SettleSave(tab_id, saved);
      RequestPreparedFrame();
      RequestPreparedFrame();
      return;
    }
  }
  EditorTabService& editor_tabs = MakeEditorTabService();
  PromptSurfaceService& prompt_surfaces = MakePromptSurfaceService();
  MakeDirtyPromptCoordinator(editor_tabs, prompt_surfaces).SettleOrphanedRun(completion.id);
}

void WorkspaceShell::ApplyAsyncFileRead(project::FileReadService::Completion completion,
                                       editor::TextViewport loaded) {
  if (completion.id == 0) {
    return;
  }
  for (std::size_t group_index = 0;
       group_index < context_.current_project_state.editor_groups.size(); ++group_index) {
    EditorGroup& group = context_.current_project_state.editor_groups[group_index];
    for (TabEntry& tab : group.open_tabs) {
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value() ||
          !tab.editor_state->pending_load.Holds(completion.id)) {
        continue;
      }
      auto& editor_state = *tab.editor_state;
      (void) editor_state.pending_load.Resolve(completion.id,
                                               editor_state.viewport.content_revision());
      if (editor_state.content != TabEntry::EditorTabState::Content::Loading) {
        return;
      }
      if (completion.status == project::FileReadService::Status::Cancelled) {
        return;
      }
      const std::filesystem::path path = completion.path;
      if (!completion.ok()) {
        editor_state.content = TabEntry::EditorTabState::Content::Failed;
        const std::string name = path.empty() ? std::string("file") : path.filename().string();
        std::string reason = completion.status == project::FileReadService::Status::TooLarge
                                 ? "Cannot open " + name + ": file is too large"
                                 : "Could not read " + name;
        Notify(NotificationService::Tone::Error, reason);
        SetEditorBanner(context_.current_project_state, EditorBannerState::Kind::OpenFailed, path,
                        std::move(reason));
        RequestPreparedFrame();
        return;
      }
      if (const editor::TextViewport* live =
              LiveBufferViewOfPath(context_.current_project_state.editor_groups, path);
          live != nullptr) {
        editor_state.viewport = *live;
      } else {
        editor_state.viewport = std::move(loaded);
      }
      ApplyEditorPreferences(editor_state.viewport);
      ApplyDetectedIndentOnOpen(editor_state.viewport);
      editor_state.restore.ApplyViewStateTo(editor_state.viewport);
      if (editor_state.center_cursor_on_load) {
        editor_state.center_cursor_on_load = false;
        editor_state.viewport.CenterLine(editor_state.viewport.cursor_line());
      }
      editor_state.content = TabEntry::EditorTabState::Content::Ready;
      editor_state.folding_model->Clear();
      NotifyPluginBufferOpen(path);
      const bool is_active = group_index == context_.current_project_state.focused_group_index &&
                             &tab == &group.active_tab();
      if (is_active) {
        SyncActiveEditorTabMetadata();
        lsp_service_.ScheduleBufferOpen(path);
      }
      ShareLoadedBufferWithWaitingTabs(path, editor_state.viewport);
      RequestPreparedFrame();
      RequestPreparedFrame();
      return;
    }
  }
}

void WorkspaceShell::ShareLoadedBufferWithWaitingTabs(const std::filesystem::path& path,
                                                      const editor::TextViewport& loaded) {
  for (EditorGroup& group : context_.current_project_state.editor_groups) {
    for (TabEntry& tab : group.open_tabs) {
      if (tab.kind != TabEntry::Kind::Editor || !tab.editor_state.has_value()) {
        continue;
      }
      auto& waiting = *tab.editor_state;
      if (!waiting.content_pending() || &waiting.viewport == &loaded ||
          waiting.restore.path.lexically_normal() != path) {
        continue;
      }
      if (waiting.pending_load.armed()) {
        file_read_service_.Cancel(waiting.pending_load.id());
        waiting.pending_load.Disarm();
      }
      waiting.viewport = loaded;
      waiting.restore.ApplyViewStateTo(waiting.viewport);
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
  for (const GroupTabRef& ref : MakeEditorTabService().DirtyGroupTabs()) {
    SaveGroupTab(ref.group_index, ref.tab_index, SaveMode::Autosave);
  }
}

Uint64 WorkspaceShell::AutosaveDelayMs() const {
  const int parsed = util::ParseIntOr(GetSettingValue("editor.autosave.delay_ms"), 1000);
  return static_cast<Uint64>(std::clamp(parsed, 200, 60000));
}

void WorkspaceShell::MaybeArmAutosaveTimer() {
  if (GetSettingValue("editor.autosave").value_or("off") != "after_delay") {
    autosave_armed_ = false;
    autosave_last_viewport_ = nullptr;
    return;
  }
  const editor::TextViewport* viewport = ActiveEditableViewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return;
  }
  if (viewport != autosave_last_viewport_) {
    autosave_last_viewport_ = viewport;
    autosave_last_content_revision_ = viewport->content_revision();
    return;
  }
  const std::uint64_t revision = viewport->content_revision();
  if (revision == autosave_last_content_revision_) {
    return;
  }
  autosave_last_content_revision_ = revision;
  autosave_edit_epoch_ms_ = SDL_GetTicks();
  autosave_armed_ = !MakeEditorTabService().DirtyGroupTabs().empty();
}

void WorkspaceShell::MaybeArmSessionFlushTimer() {
  std::uint64_t fingerprint = 1469598103934665603ull;
  bool any_dirty = false;
  const auto mix = [&fingerprint](std::uint64_t value) {
    fingerprint = (fingerprint ^ value) * 1099511628211ull;
  };
  for (const EditorGroup& group : context_.current_project_state.editor_groups) {
    for (const TabEntry& tab : group.open_tabs) {
      const editor::TextViewport* viewport =
          tab.kind == TabEntry::Kind::Merge && tab.merge.has_value() ? &tab.merge->result_viewport
                                                                      : LoadedBufferViewOf(tab);
      if (viewport == nullptr) {
        continue;
      }
      mix(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(viewport->document_identity())));
      mix(viewport->content_revision());
      any_dirty = any_dirty || viewport->dirty();
    }
  }
  if (fingerprint == session_flush_content_fingerprint_) {
    return;
  }
  session_flush_content_fingerprint_ = fingerprint;
  if (any_dirty) {
    session_flush_.Note(SDL_GetTicks());
  } else {
    session_flush_.Touch(SDL_GetTicks());
  }
}

std::optional<Uint32> WorkspaceShell::NextAutosaveDelayMs() const {
  if (!autosave_armed_) {
    return std::nullopt;
  }
  const Uint64 delay = AutosaveDelayMs();
  const Uint64 elapsed = SDL_GetTicks() - autosave_edit_epoch_ms_;
  if (elapsed >= delay) {
    return static_cast<Uint32>(1);
  }
  return static_cast<Uint32>(std::max<Uint64>(1, delay - elapsed));
}

SavePreparation WorkspaceShell::PrepareEditorViewportForSave(const std::filesystem::path& path,
                                                             editor::TextViewport& viewport,
                                                             std::string* error_message,
                                                             SaveMode mode) {
  util::ClearError(error_message);
  if (path.empty()) {
    return SavePreparation::Ready();
  }

  if (mode == SaveMode::SkipTransforms || mode == SaveMode::Autosave) {
    return SavePreparation::Ready();
  }
  const bool has_save_participants =
      plugin_runtime_.enabled() && !save_participant_registry_.Specs().empty();
  std::string destination_filetype;
  if (path != viewport.path()) {
    destination_filetype = editor::runtime_syntax::DetectFiletype(path);
  }
  const std::string_view filetype = path == viewport.path()
                                        ? std::string_view(viewport.language_id())
                                        : std::string_view(destination_filetype);
  const bool format_on_save = SettingFlagEnabled(GetSettingValue("editor.format_on_save"), true);
  const FormatterSpec* formatter =
      (format_on_save && !filetype.empty()) ? FindFormatter(formatter_registry_, filetype)
                                            : nullptr;
  const bool has_formatter = formatter != nullptr && !formatter->command.empty();
  if (!has_save_participants && !has_formatter) {
    return SavePreparation::Ready();
  }

  std::string text = viewport.SerializeText();
  const platform::ProcessLauncher& launcher = context_.current_project_state.launcher();
  if (mode == SaveMode::Deferred) {
    constexpr int kFormatterTimeoutMs = 5000;
    SaveFormatterService::Request request{
        .command = has_formatter ? formatter->command : std::vector<std::string>{},
        .cwd = context_.current_project_state.root,
        .text = has_save_participants ? text : std::move(text),
        .formatter_id = has_formatter ? formatter->id : std::string(),
        .timeout_ms = kFormatterTimeoutMs,
        .participants = has_save_participants
                            // NOLINTNEXTLINE(bugprone-use-after-move): .text copied it.
                            ? plugin_runtime_.Host().PrepareSaveParticipants(path, std::move(text))
                            : nullptr,
    };
    const std::uint64_t run_id = save_formatter_service_.Begin(
        path.generic_string(), std::move(request), launcher,
        [this](SaveFormatterService::Completion completion) { ApplyDeferredSaveFormat(completion); });
    return run_id != 0 ? SavePreparation::Deferred(run_id) : SavePreparation::Ready();
  }

  const std::string original_text = text;
  if (has_save_participants &&
      !plugin_runtime_.Host().RunSaveParticipants(path, &text, error_message)) {
    return SavePreparation::Failed();
  }

  if (has_formatter) {
    constexpr int kFormatterTimeoutMs = 5000;
    const SaveFormatterService::Completion completion = save_formatter_service_.RunBlocking(
        SaveFormatterService::Request{
            .command = formatter->command,
            .cwd = context_.current_project_state.root,
            .text = text,
            .formatter_id = formatter->id,
            .timeout_ms = kFormatterTimeoutMs,
        },
        launcher);
    if (!completion.ok) {
      ReportSaveFormatterFailure(completion, error_message);
      return SavePreparation::Ready();
    }
    if (!completion.formatted_text.empty()) {
      text = completion.formatted_text;
    }
  }

  if (text == original_text) {
    return SavePreparation::Ready();
  }

  (void)viewport.ApplyFormattedText(text);
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

const std::filesystem::path& TabLabelPath(const TabEntry& tab) {
  if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
    return tab.compare->plain_compare ? tab.compare->right_path : tab.compare->path;
  }
  if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value()) {
    return tab.merge->output_path;
  }
  return tab.path;
}

std::string_view TabLabelFallback(const TabEntry& tab) {
  std::string_view title = tab.title;
  if (tab.kind == TabEntry::Kind::Compare || tab.kind == TabEntry::Kind::Merge) {
    for (const std::string_view prefix : {std::string_view("compare: "), std::string_view("merge: ")}) {
      if (title.starts_with(prefix)) {
        title.remove_prefix(prefix.size());
      }
    }
  }
  return title;
}

}

const TabEntry* WorkspaceShell::TabForLabels(std::size_t group_index, std::size_t index) const {
  const ProjectWorkspaceState& state = context_.current_project_state;
  if (group_index >= state.editor_groups.size() ||
      index >= state.editor_groups[group_index].open_tabs.size()) {
    return nullptr;
  }
  return &state.editor_groups[group_index].open_tabs[index];
}

std::string WorkspaceShell::TabDisplayTitle(std::size_t group_index, std::size_t index) const {
  const TabEntry* tab = TabForLabels(group_index, index);
  if (tab == nullptr) {
    return {};
  }
  std::string title = BuildWorkspaceTabDisplayTitle(TabLabelPath(*tab), TabLabelFallback(*tab),
                                                    TabCoordinator::TabStateIsDirty(*tab));
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
        title += " \xe2\x80\x94 ";
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

bool WorkspaceShell::ReloadCleanEditorTabsForPath(const std::filesystem::path& path,
                                                  EditorReloadEchoGuard echo_guard) {
  return MakeEditorTabService().ReloadCleanEditorTabsForPath(path, echo_guard);
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
  RequestPreparedFrame();
}

bool WorkspaceShell::CloseEditorGroup() {
  if (EditorGroupCount() < 2) {
    return false;
  }
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
  if (glue_->remote_host_service != nullptr &&
      glue_->remote_host_service->OpenWhenFetched(
          path, [this](const std::filesystem::path& fetched) { (void)OpenFileInNewTab(fetched); })) {
    return false;
  }
  const bool host_copy =
      glue_->remote_host_service != nullptr && glue_->remote_host_service->IsHostFileCopy(path);
  if (!MakeEditorTabService().OpenFileInNewTab(path)) {
    return false;
  }
  editor::TextViewport* viewport = ActiveEditorViewport();
  if (viewport == nullptr) {
    return true;
  }
  if (host_copy) {
    viewport->SetReadOnly(true);
  }
  if (const std::filesystem::path& root = context_.current_project_state.root; !root.empty()) {
    recents_service_.RecordFileOpen(viewport->path(), root);
  }
  return true;
}

bool WorkspaceShell::MoveActiveTabTo(std::size_t index) {
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

  if (viewport != nullptr && util::SameAsNormalizedPath(viewport->path(), normalized_path)) {
    viewport->JumpCursorTo(line, column);
  }
}

}
