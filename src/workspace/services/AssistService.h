#pragma once

#include <algorithm>
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
#include "workspace/lsp/LspPositionEncoding.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceLanguageContract.h"
#include "workspace/lsp/WorkspaceLspManager.h"
#include "workspace/WorkspaceOutputChannels.h"
#include "workspace/WorkspacePluginRuntime.h"
#include "workspace/state/WorkspaceProjectState.h"
#include "workspace/state/WorkspaceTabState.h"

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
    // converted to LSP wire form (columns in the server's position encoding), to
    // populate a codeAction request `context`. The encoding must match the request
    // Range's, or clangd (UTF-16) fails to match diagnostics on non-ASCII lines.
    std::function<std::vector<LspClient::Diagnostic>(const editor::TextViewport&,
                                                     const editor::SelectionRange&,
                                                     lsp_encoding::PositionEncoding)>
        collect_lsp_context_diagnostics;
    // Apply a code action's inline WorkspaceEdit (0-based edits) directly to the
    // open buffers. Returns false if no target buffer resolved / edit was stale.
    std::function<bool(const std::vector<CodeActionEdit>&)> apply_lsp_workspace_edit;
    // Apply a WorkspaceEdit that carries file resource ops (create/rename/delete):
    // ops first (validate-first + rollback-safe staging), then the text edits —
    // open buffers in place AND closed files silently on disk (an ops-carrying
    // action's edits typically fill the file it just created, which no buffer has
    // open). Returns false when the op batch failed (nothing left applied).
    std::function<bool(const std::vector<CodeActionEdit>&,
                       const std::vector<WorkspaceResourceOp>&)>
        apply_full_lsp_workspace_edit;
    // Apply an LSP rename result: applies in place when every affected file is open,
    // or confirms + opens + saves when some are closed (resource ops — e.g. a
    // rust-analyzer module rename renaming the file — always confirm, and run
    // before the text edits). `new_name` drives the prompt.
    std::function<void(const std::string&, const std::vector<CodeActionEdit>&,
                       const std::vector<WorkspaceResourceOp>&)>
        apply_rename_workspace_edit;
    std::function<bool(const std::filesystem::path&)> open_file_in_new_tab;
    std::function<void()> reset_caret_blink;
    std::function<void()> request_focused_editor_redraw;
    std::function<void()> request_active_editable_last_change_redraw;
    std::function<void(std::size_t, std::size_t)> request_active_editable_blame_neighborhood_redraw;
    std::function<void()> request_tab_strip_redraw;
    std::function<void(CompareTabState&)> refresh_compare_tab_derived_state;
    std::function<void(CompareTabState&, bool)> sync_compare_selection_from_viewport;
    std::function<void(MergeTabState&, const std::optional<editor::SelectionRange>&,
                       const editor::TextPosition&)>
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

  // Convert LSP code actions into overlay session items, materializing each
  // action's inline WorkspaceEdit under a SHARED aggregate edit/byte budget so a
  // server returning many large (but individually capped) fixes cannot make the
  // overlay hold the sum of every action's edit payload before the user selects
  // one. Past the budget an action's inline fix is dropped (edits_truncated set).
  // Static + free of member state so the budget is unit-testable. TD-2026-07-17A-057.
  static std::vector<CodeActionSessionItem> TransformLspCodeActions(
      const LspResult<std::vector<LspClient::CodeAction>>& actions);

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

  // Render the call hierarchy of the symbol under the caret into the
  // `lsp.callHierarchy` output channel: who calls it (`incoming`) or what it calls.
  // Two round-trips — textDocument/prepareCallHierarchy resolves the symbol, then
  // callHierarchy/{incoming,outgoing}Calls walks one level of edges. One level, not
  // a tree: the channel is a flat navigable list, and an expandable tree would need
  // a surface this shell does not have. Entries reuse the references formatter, so
  // each is a clickable file:line:col with context.
  bool ShowCallHierarchy(bool incoming, std::string* error_message = nullptr);

 private:
  struct EditSideEffectsSnapshot {
    bool was_dirty = false;
    std::size_t cursor_before_line = 0;
    std::optional<editor::SelectionRange> selection_before;
    std::optional<editor::TextPosition> cursor_before;
  };

  EditSideEffectsSnapshot CaptureEditSnapshot(editor::TextViewport& viewport) const;
  void ApplyEditSideEffects(editor::TextViewport& viewport,
                            const EditSideEffectsSnapshot& snapshot) const;

  // Apply a textDocument/formatting or rangeFormatting result to the active buffer
  // (shared by whole-document and range formatting). Drops superseded results.
  void ApplyFormattingResult(const std::filesystem::path& request_path,
                             LspResult<std::vector<LspClient::TextEdit>> edits);

  // Per-request state for the LSP-primary *concurrent* provider model: a plugin
  // worker and the language server are queried at once, then their results are
  // merged (ranked, de-duplicated) or the navigation source is chosen. Both
  // callbacks run on the UI mailbox drain, so no locking is needed.
  struct CompletionMerge {
    assist_merge::TwoSourceState sources;
    std::string language_id;
    std::vector<CompletionSessionItem> lsp_items;
    std::vector<CompletionSessionItem> plugin_items;
    // Request generation: a callback for an OLDER request (slower to return) must not
    // overwrite the items a newer same-file request already published. (TD-16-65.)
    std::uint64_t generation = 0;
  };
  struct CodeActionMerge {
    assist_merge::TwoSourceState sources;
    std::string language_id;
    std::vector<CodeActionSessionItem> lsp_items;
    std::vector<CodeActionSessionItem> plugin_items;
    std::uint64_t generation = 0;
  };
  struct NavigationMerge {
    assist_merge::TwoSourceState sources;
    lsp_encoding::PositionEncoding lsp_encoding = lsp_encoding::PositionEncoding::Utf8;
    std::vector<plugin::PluginHost::LocationResult> plugin_locations;
    std::vector<LspClient::Location> lsp_locations;
    bool acted = false;
    // Per-surface request generation (navigation vs references — see the
    // *_request_generation_ counters). A callback carries the generation it was
    // dispatched under and drops its result when a newer same-surface request (e.g.
    // a second Go-To-Definition at a different caret in the same file before the
    // first response lands) has bumped the counter. TD-2026-07-17A-030.
    std::uint64_t generation = 0;
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
    std::uint64_t generation = 0;  // see signature_request_generation_ (TD-2026-07-17A-030)
  };

  // Transform a source's raw results into the shared overlay item type.
  std::vector<CompletionSessionItem> TransformPluginCompletions(
      const std::vector<plugin::PluginHost::CompletionCandidate>& items) const;
  std::vector<CompletionSessionItem> TransformLspCompletions(
      const LspResult<std::vector<LspClient::CompletionItem>>& items,
      lsp_encoding::PositionEncoding encoding) const;
  std::vector<CodeActionSessionItem> TransformPluginCodeActions(
      const std::vector<plugin::PluginHost::CodeActionCandidate>& items) const;

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
  // The column of a reference entry, in whichever units its source spoke. A plugin
  // provider reports a 1-based character column; a language server reports a
  // 0-based offset in its negotiated position encoding (bytes for clangd and
  // rust-analyzer, UTF-16 units by default), which is only a character column on
  // an ASCII line. The emitter resolves both through the target line's text, so
  // the printed `path:line:col` names the character the Output click handler
  // then jumps to.
  struct ReferenceColumn {
    std::size_t value = 0;
    bool lsp_units = false;
    lsp_encoding::PositionEncoding encoding = lsp_encoding::PositionEncoding::Utf8;

    static ReferenceColumn Character1Based(std::size_t column) {
      return ReferenceColumn{.value = column, .lsp_units = false};
    }
    static ReferenceColumn LspCharacter(int character, lsp_encoding::PositionEncoding encoding) {
      return ReferenceColumn{.value = static_cast<std::size_t>(std::max(0, character)),
                             .lsp_units = true,
                             .encoding = encoding};
    }
  };
  // Emit one reference entry (file:line:column header + the ±1-line context block,
  // plus a trailing blank when `append_separator`) into the given channel,
  // caching file contents in `file_line_cache`. `line` is 1-based.
  void EmitReferenceEntry(const char* channel_id, const char* channel_title,
                          const std::filesystem::path& path, std::size_t line,
                          ReferenceColumn column, bool append_separator,
                          std::map<std::filesystem::path, std::vector<std::string>>&
                              file_line_cache) const;

  WorkspaceContext* context_ = nullptr;
  WorkspacePluginRuntime* plugin_runtime_ = nullptr;
  WorkspaceOutputChannels* output_channels_ = nullptr;
  WorkspaceLanguageContract* language_contract_ = nullptr;
  Operations operations_{};
  // Monotonic per-kind request generations. A new request bumps its counter; an async
  // callback carries the generation it was dispatched under and drops its result when a
  // newer request has since superseded it, so a slower older same-file request cannot
  // overwrite or apply against newer state. (TD-2026-07-16-65.)
  std::uint64_t completion_request_generation_ = 0;
  std::uint64_t code_action_request_generation_ = 0;
  // Cursor-jump navigation (definition / type-def / impl / declaration): one
  // counter, since the user only ever jumps once — any new navigation supersedes a
  // pending one. References and signature help are independent surfaces.
  std::uint64_t navigation_request_generation_ = 0;
  std::uint64_t references_request_generation_ = 0;
  std::uint64_t signature_request_generation_ = 0;
  // workspace/symbol has no active-file cursor to anchor a staleness check on (it is
  // project-wide), so a slower response for an OLDER query could otherwise clear the
  // channel and render its results over the newer query's. Gate it on a generation
  // token like completions/code actions do (TD-2026-07-17A-034).
  std::uint64_t workspace_symbol_request_generation_ = 0;
  // Call hierarchy spans two chained round-trips, so a second invocation while the
  // first is mid-chain would otherwise let the older chain finish last and render
  // over the newer one. Both hops carry this token.
  std::uint64_t call_hierarchy_request_generation_ = 0;
};

}  // namespace microide::workspace
