#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/TextViewport.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceLanguageContract.h"
#include "workspace/WorkspaceOutputChannels.h"
#include "workspace/WorkspacePluginRuntime.h"
#include "workspace/lsp/LspPositionEncoding.h"
#include "workspace/lsp/WorkspaceLspManager.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/state/WorkspaceTabState.h"

namespace microide::workspace {

class WorkspaceEditService;

// What every assist feature (completion, snippets, code actions, navigation,
// refactoring, signature help) shares: the shell's callback seam, the workspace
// state it reads, and the handful of helpers each feature used to re-spell — the
// staleness test, the "no active file" / "no language server" prologues, and the
// side effects of an assist edit. Owned by AssistService; each feature holds a
// reference to it.
class AssistHost {
 public:
  // The shell's callback seam every feature shares: settings, the active views,
  // the LSP request plumbing, overlays and the redraws an assist edit owes. A
  // callback only one feature reads belongs in that feature's own Port
  // (CompletionAssist::Port, ...), so each seam says what its feature needs.
  struct Operations {
    std::function<std::optional<std::string>(std::string_view)> get_setting_value;
    std::function<editor::TextViewport*()> active_editable_viewport;
    std::function<editor::TextViewport*()> active_editor_viewport;
    std::function<TabEntry::EditorTabState*()> active_editor_tab;
    std::function<CompareTabState*()> active_compare_tab;
    std::function<MergeTabState*()> active_merge_tab;
    std::function<LspClient*(const editor::TextViewport&, std::string*)> lsp_client_for_viewport;
    std::function<LspManager&()> current_lsp_manager;
    std::function<void(const editor::TextViewport&, LspClient&, std::string_view)>
        ensure_lsp_document_open;
    std::function<void()> begin_tracked_lsp_request;
    std::function<void()> finish_tracked_lsp_request;
    std::function<void(OverlayMode)> show_overlay;
    std::function<void(bool)> dismiss_overlay;
    std::function<void()> request_overlay_redraw;
    std::function<void()> reset_caret_blink;
    std::function<void()> request_focused_editor_redraw;
    std::function<void()> request_active_editable_last_change_redraw;
    std::function<void(std::size_t, std::size_t)> request_active_editable_blame_neighborhood_redraw;
    std::function<void(CompareTabState&)> refresh_compare_tab_derived_state;
    std::function<void(CompareTabState&, bool)> sync_compare_selection_from_viewport;
    std::function<void(MergeTabState&)> update_merge_tracking_after_viewport_edit;
    // A short message where the user is looking (an info toast): a navigation that
    // found nothing says so, as VS Code's "No definition found" does, instead of
    // writing only to an output channel no one has open.
    std::function<void(std::string)> notify_info;
  };

  void Configure(WorkspaceContext& context,
                 WorkspacePluginRuntime& plugin_runtime,
                 WorkspaceOutputChannels& output_channels,
                 WorkspaceLanguageContract& language_contract,
                 WorkspaceEditService& edits,
                 Operations operations);

  const Operations& ops() const { return operations_; }
  // Where a code action's, a rename's and a formatting result's edits are applied.
  WorkspaceEditService& edits() const { return *edits_; }
  WorkspaceContext& context() const { return *context_; }
  WorkspacePluginRuntime& plugins() const { return *plugin_runtime_; }
  WorkspaceOutputChannels& output() const { return *output_channels_; }
  WorkspaceLanguageContract& languages() const { return *language_contract_; }

  // An async result for `request_path` is stale when the active editable buffer
  // has gone away or changed path since the request was issued.
  bool IsStale(const std::filesystem::path& request_path) const;
  bool SnippetsEnabled() const;

  // The prologue of every explicit request: the active editable viewport, or
  // nullptr after writing "No active file" to `error_message`.
  editor::TextViewport* RequireActiveEditableViewport(std::string* error_message) const;
  // Resolve the LSP client for `viewport`, open its document, and begin a tracked
  // request. Returns nullptr after logging/recording an unavailable-server
  // message when no client exists.
  LspClient* PrepareLspRequest(editor::TextViewport& viewport, std::string* error_message) const;
  // Log a genuine language-server startup failure to the LSP log. No-op when a
  // server served the buffer or none is configured (avoids per-request noise).
  void MaybeLogLspUnavailable(const std::string& language_id, bool lsp_authoritative) const;
  // A message for the user where they are looking and in `channel`.
  void Report(const char* channel_id, const char* channel_title, const std::string& message) const;

  // An assist edit's side effects on the shell: the compare/merge views that
  // track the edited viewport, the caret blink, and the redraws.
  struct EditSnapshot {
    bool was_dirty = false;
    std::size_t cursor_before_line = 0;
  };
  static EditSnapshot CaptureEditSnapshot(const editor::TextViewport& viewport);
  void ApplyEditSideEffects(editor::TextViewport& viewport, const EditSnapshot& snapshot) const;
  // A caret/selection change in the focused editor: blink, last-change marker, repaint.
  void RedrawAfterCaretChange() const;

 private:
  WorkspaceContext* context_ = nullptr;
  WorkspacePluginRuntime* plugin_runtime_ = nullptr;
  WorkspaceOutputChannels* output_channels_ = nullptr;
  WorkspaceLanguageContract* language_contract_ = nullptr;
  WorkspaceEditService* edits_ = nullptr;
  Operations operations_{};
};

namespace assist {

// An async assist result (plugin/LSP completion or code action) is stale when
// the active editable buffer has gone away or changed path since the request
// was issued; writing it would clobber a newer session or land across a
// file/project switch. Pure so the drop decision is unit-testable without the
// subprocess-backed LSP client.
bool ResultIsStale(const editor::TextViewport* active_editable,
                   const std::filesystem::path& request_path);

// One line zero-copy (TD-2026-07-17-072): never materialize the document to index it.
std::string_view LineViewAt(const editor::TextViewport& viewport, std::size_t index);

// The identifier run ending at the caret: what a completion or a snippet prefix
// replaces.
editor::SelectionRange CompletionReplacementRange(const editor::TextViewport& viewport);

// An LSP TextEdit as the shared edit record: 0-based LSP coordinates (the apply
// path maps columns through the server's position encoding), negatives clamped
// to 0 so a malformed `line: -1` cannot wrap to a huge size_t.
CodeActionEdit ToCodeActionEdit(const std::filesystem::path& path,
                                const LspClient::Range& range,
                                const std::string& new_text);

}  // namespace assist

}  // namespace microide::workspace
