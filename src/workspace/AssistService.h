#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/TextViewport.h"
#include "plugin/PluginHost.h"
#include "workspace/AssistProviderMerge.h"
#include "workspace/LspPositionEncoding.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceLanguageContract.h"
#include "workspace/WorkspaceLspManager.h"
#include "workspace/WorkspaceOutputChannels.h"
#include "workspace/WorkspacePluginRuntime.h"
#include "workspace/WorkspaceProjectState.h"
#include "workspace/WorkspaceTabState.h"

namespace microide::workspace {

class AssistService {
 public:
  // Transitional callback seam for shell integration.
  // Keep this narrow: do not grow it into a generic shell callback bag.
  // Future assist refactors should split this into smaller ports
  // (ActiveEditor, LspAssist, Overlay, CommandExecution, FileOpen,
  // MergeTracking, CompareSync) as ownership moves out.
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
    // Hand a prebuilt (signature, documentation) pair to the host's caret-anchored
    // signature-help popup. Empty signature clears it.
    std::function<void(std::string, std::string)> show_signature_help;
    std::function<bool(std::string_view, const std::vector<std::string>&, std::string*)>
        execute_command_name;
    // Collect the diagnostics for the viewport's file that overlap `range`,
    // converted to LSP wire form, to populate a codeAction request `context`.
    std::function<std::vector<LspClient::Diagnostic>(const editor::TextViewport&,
                                                     const editor::SelectionRange&)>
        collect_lsp_context_diagnostics;
    // Apply a code action's inline WorkspaceEdit (0-based edits) directly to the
    // open buffers. Returns false if no target buffer resolved / edit was stale.
    std::function<bool(const std::vector<CodeActionEdit>&)> apply_lsp_workspace_edit;
    // Apply an LSP rename result: applies in place when every affected file is open,
    // or confirms + opens + saves when some are closed. `new_name` drives the prompt.
    std::function<void(const std::string&, const std::vector<CodeActionEdit>&)>
        apply_rename_workspace_edit;
    std::function<bool(const std::filesystem::path&)> open_file_in_new_tab;
    std::function<void()> reset_caret_blink;
    std::function<void()> request_focused_editor_redraw;
    std::function<void()> request_active_editable_last_change_redraw;
    std::function<void(std::size_t, std::size_t)> request_active_editable_blame_neighborhood_redraw;
    std::function<void()> request_tab_strip_redraw;
    std::function<void(CompareTabState&)> refresh_compare_tab_derived_state;
    std::function<void(CompareTabState&, bool)> sync_compare_selection_from_viewport;
    std::function<void(MergeTabState&, const std::vector<std::string>&,
                       const std::optional<editor::SelectionRange>&, const editor::TextPosition&)>
        update_merge_tracking_after_viewport_edit;
  };

  AssistService() = default;

  void Configure(WorkspaceContext& context,
                 WorkspacePluginRuntime& plugin_runtime,
                 WorkspaceOutputChannels& output_channels,
                 WorkspaceLanguageContract& language_contract,
                 Operations operations);

  // An async assist result (plugin/LSP completion or code action) is stale when
  // the active editable buffer has gone away or changed path since the request
  // was issued; writing it would clobber a newer session or land across a
  // file/project switch. Pure so the drop decision is unit-testable without the
  // subprocess-backed LSP client.
  static bool ResultIsStale(const editor::TextViewport* active_editable,
                            const std::filesystem::path& request_path);

  bool EditorSnippetsSettingEnabled() const;
  bool ShowCompletionOverlay(std::string* error_message = nullptr);
  bool ApplySelectedCompletion();
  bool ShowInsertSnippetOverlay(std::string* error_message = nullptr);
  bool TrySnippetTabInEditor(bool shift_tab);
  bool TrySnippetEscapeInEditor();
  void NotifySnippetSessionCaretMoved();
  void ClearActiveSnippetSessionAfterUndo();
  bool TrySnippetInsertTextInEditor(editor::TextViewport* viewport, std::string_view text);
  bool TrySnippetBackspaceInEditor(editor::TextViewport* viewport);
  bool TrySnippetDeleteForwardInEditor(editor::TextViewport* viewport);
  // `explicit_range`, when set, targets code actions at that document range
  // (e.g. a diagnostic's range from the hover "Quick Fix" affordance) instead of
  // the current cursor/selection.
  bool ShowCodeActionsOverlay(std::string* error_message = nullptr,
                              const editor::SelectionRange* explicit_range = nullptr);
  bool ExecuteSelectedCodeAction();
  bool GoToLspDefinition(std::string* error_message = nullptr);
  // LSP-only navigation to the type/interface, implementation(s), or declaration of
  // the symbol under the cursor (textDocument/typeDefinition|implementation|
  // declaration). Jumps to the first location returned.
  bool GoToLspTypeDefinition(std::string* error_message = nullptr);
  bool GoToLspImplementation(std::string* error_message = nullptr);
  bool GoToLspDeclaration(std::string* error_message = nullptr);
  bool FindLspReferences(std::string* error_message = nullptr);
  // Format the active editable buffer via the language server's
  // textDocument/formatting and apply the returned edits to the open buffer.
  bool FormatActiveDocument(std::string* error_message = nullptr);
  // Prepare-rename probe for the symbol under the cursor: resolves the LSP client,
  // fires textDocument/prepareRename, and delivers the result to `callback` on the
  // main-thread drain. `callback(can_rename, placeholder)` — placeholder is the
  // server's suggested seed (may be empty). Never invoked when no server serves the
  // buffer (the caller keeps its heuristic seed and validates on rename).
  void PrepareRenameForCursor(std::function<void(bool, std::string)> callback);
  // The identifier under the cursor in the active editable buffer (word chars:
  // alnum + '_'), or empty when the cursor is not on one. Used to prefill the
  // rename prompt.
  std::string SymbolAtCursor() const;
  // Rename the symbol under the cursor to `new_name` via the language server's
  // textDocument/rename, applying the returned workspace edit across open buffers.
  bool RenameSymbol(const std::string& new_name, std::string* error_message = nullptr);
  bool ShowSignatureHelp(std::string* error_message = nullptr);
  // Project-wide symbol search via workspace/symbol; renders navigable results into
  // the "lsp.workspaceSymbols" output channel.
  bool ShowWorkspaceSymbols(const std::string& query, std::string* error_message = nullptr);

 private:
  struct EditSideEffectsSnapshot {
    bool was_dirty = false;
    std::size_t cursor_before_line = 0;
    std::vector<std::string> before_lines;
    std::optional<editor::SelectionRange> selection_before;
    std::optional<editor::TextPosition> cursor_before;
  };

  EditSideEffectsSnapshot CaptureEditSnapshot(editor::TextViewport& viewport) const;
  void ApplyEditSideEffects(editor::TextViewport& viewport,
                            const EditSideEffectsSnapshot& snapshot) const;

  // Apply a textDocument/formatting or rangeFormatting result to the active buffer
  // (shared by whole-document and range formatting). Drops superseded results.
  void ApplyFormattingResult(const std::filesystem::path& request_path,
                             std::optional<std::vector<LspClient::TextEdit>> edits);

  // Per-request state for the LSP-primary *concurrent* provider model: a plugin
  // worker and the language server are queried at once, then their results are
  // merged (ranked, de-duplicated) or the navigation source is chosen. Both
  // callbacks run on the UI mailbox drain, so no locking is needed.
  struct CompletionMerge {
    assist_merge::TwoSourceState sources;
    std::string language_id;
    std::vector<CompletionSessionItem> lsp_items;
    std::vector<CompletionSessionItem> plugin_items;
  };
  struct CodeActionMerge {
    assist_merge::TwoSourceState sources;
    std::string language_id;
    std::vector<CodeActionSessionItem> lsp_items;
    std::vector<CodeActionSessionItem> plugin_items;
  };
  struct NavigationMerge {
    assist_merge::TwoSourceState sources;
    lsp_encoding::PositionEncoding lsp_encoding = lsp_encoding::PositionEncoding::Utf8;
    std::vector<plugin::PluginHost::LocationResult> plugin_locations;
    std::vector<LspClient::Location> lsp_locations;
    bool acted = false;
  };
  // Signature help is a single caret-anchored popup, so the two sources are
  // *chosen* between (LSP-primary, like navigation) rather than unioned. Each
  // source lowers its raw result into the display (signature, documentation) pair
  // as it arrives; the resolver shows exactly one, once.
  struct SignatureHelpMerge {
    assist_merge::TwoSourceState sources;
    bool lsp_has = false;
    std::string lsp_signature;
    std::string lsp_documentation;
    bool plugin_has = false;
    std::string plugin_signature;
    std::string plugin_documentation;
    bool acted = false;
  };

  // Transform a source's raw results into the shared overlay item type.
  std::vector<CompletionSessionItem> TransformPluginCompletions(
      const std::vector<plugin::PluginHost::CompletionCandidate>& items) const;
  std::vector<CompletionSessionItem> TransformLspCompletions(
      const std::optional<std::vector<LspClient::CompletionItem>>& items,
      lsp_encoding::PositionEncoding encoding) const;
  std::vector<CodeActionSessionItem> TransformPluginCodeActions(
      const std::vector<plugin::PluginHost::CodeActionCandidate>& items) const;
  std::vector<CodeActionSessionItem> TransformLspCodeActions(
      const std::optional<std::vector<LspClient::CodeAction>>& actions) const;

  // Merge both sources into the (already-open) overlay session. Called on each
  // source's arrival; publishes the ranked union so results appear as soon as
  // either source answers, LSP-first when a server serves the language.
  void PublishCompletionMerge(const std::shared_ptr<CompletionMerge>& merge,
                              const std::filesystem::path& request_path);
  void PublishCodeActionMerge(const std::shared_ptr<CodeActionMerge>& merge,
                              const std::filesystem::path& request_path);
  // Navigate once the preferred source resolves (go-to-definition). Waits for the
  // language server when it serves the buffer, else uses the plugin result.
  void ResolveDefinitionNavigation(const std::shared_ptr<NavigationMerge>& merge,
                                   const std::filesystem::path& request_path);
  // Render the merged reference set (both sources, de-duplicated by location)
  // into the References output channel once both sources have resolved.
  void PublishReferenceMerge(const std::shared_ptr<NavigationMerge>& merge,
                             const std::filesystem::path& request_path);
  // Show the chosen signature-help popup once the preferred source resolves
  // (LSP-primary; plugin used only when the server serves nothing).
  void ResolveSignatureHelp(const std::shared_ptr<SignatureHelpMerge>& merge,
                            const std::filesystem::path& request_path);
  // Log a genuine language-server startup failure to the LSP log. No-op when a
  // server served the buffer or none is configured (avoids per-request noise).
  void MaybeLogLspUnavailable(const std::string& language_id, bool lsp_authoritative);
  // Expand a snippet whose `prefix` matches the identifier immediately left of the
  // caret (Tab with no active session). Returns false on no/ambiguous match so the
  // caller falls through to inserting a literal tab.
  bool TrySnippetPrefixExpansion(TabEntry::EditorTabState& tab, editor::TextViewport& viewport);

  // Open a plugin-provided navigation target (1-based line/column) in a new tab
  // and move the caret there. Shared by go-to-definition and the outline view.
  void NavigateToPluginLocation(const plugin::PluginHost::LocationResult& location);
  // Open an LSP Location (0-based, server position encoding) in a new tab and move
  // the caret there. Shared by definition and the type/impl/decl navigations.
  void NavigateToLspLocation(const LspClient::Location& location,
                             lsp_encoding::PositionEncoding encoding);

  enum class LspNavigationKind { TypeDefinition, Implementation, Declaration };
  bool GoToLspNavigation(LspNavigationKind kind, std::string* error_message);

  // Shared go-to-definition / find-references prologue. Returns the active
  // editable viewport, or nullptr after writing "No active file" to
  // `error_message`.
  editor::TextViewport* RequireActiveEditableViewport(std::string* error_message) const;
  // Resolve the LSP client for `viewport`, open its document, and begin a tracked
  // request. Returns nullptr after logging/recording an unavailable-server
  // message when no client exists.
  LspClient* PrepareLspRequest(editor::TextViewport& viewport, std::string* error_message);
  // Emit one reference entry (file:line:column header + the ±1-line context block,
  // plus a trailing blank when `append_separator`) into the given channel,
  // caching file contents in `file_line_cache`. `line`/`column` are 1-based.
  void EmitReferenceEntry(const char* channel_id, const char* channel_title,
                          const std::filesystem::path& path, std::size_t line, std::size_t column,
                          bool append_separator,
                          std::map<std::filesystem::path, std::vector<std::string>>&
                              file_line_cache) const;

  WorkspaceContext* context_ = nullptr;
  WorkspacePluginRuntime* plugin_runtime_ = nullptr;
  WorkspaceOutputChannels* output_channels_ = nullptr;
  WorkspaceLanguageContract* language_contract_ = nullptr;
  Operations operations_{};
};

}  // namespace microide::workspace
