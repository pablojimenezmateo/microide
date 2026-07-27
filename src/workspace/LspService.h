#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "editor/TextViewport.h"
#include "workspace/LspPositionEncoding.h"
#include "workspace/WorkspaceCodeActionRegistry.h"
#include "workspace/WorkspaceCompletionRegistry.h"
#include "workspace/WorkspaceLspManager.h"

namespace microide::render {
struct Theme;
}

namespace microide::workspace {

struct WorkspaceContext;
struct ProjectWorkspaceState;
struct CodeActionEdit;
struct WorkspaceResourceOp;

// Host-owned home for the LSP glue that used to live directly on WorkspaceShell:
// per-project server management, document synchronization, diagnostics publishing,
// provider-presence queries, readiness/status strings, and the in-flight request
// indicator. WorkspaceShell keeps thin forwarders; render/menu/plugin TUs are
// unchanged. The shell wiring is injected through the narrow Operations seam.
// Outcome of applying a server WorkspaceEdit to already-open buffers. `any_rejected`
// is set when a resolved buffer's edit group was dropped without applying — a line
// beyond EOF or an overlapping group — so the server-initiated response never reports
// success after silently discarding an open-buffer target.
struct OpenBufferEditResult {
  bool applied_any = false;
  bool any_rejected = false;
};

// A closed-file target for a server-initiated WorkspaceEdit: the file's ranged
// edits plus the server's negotiated position encoding for mapping them. Prepared
// on the main thread (path/encoding resolution needs the shared syntax registry +
// LSP manager); RunClosedFileEdits then loads/applies/saves it off the shell thread
// (TD-2026-07-17-011 / TD-2026-07-16-18).
struct LspClosedFileEditBucket {
  std::filesystem::path path;
  std::vector<std::pair<editor::SelectionRange, std::string>> edits;
  lsp_encoding::PositionEncoding encoding = lsp_encoding::PositionEncoding::Utf8;
};

class LspService {
 public:
  struct Operations {
    std::function<editor::TextViewport*()> active_editable_viewport;
    std::function<void()> refresh_problems_sidebar;
    std::function<void()> request_editor_surface_redraw;
    std::function<void()> request_chrome_redraw;
    std::function<void()> request_bottom_panel_redraw;
    // Apply 0-based LSP edits to already-open buffers in place (the shell's
    // ApplyLspWorkspaceEdit). Used by the server-initiated workspace/applyEdit
    // path; closed files are written on disk by LspService itself. The result
    // distinguishes "something applied" from "some open-buffer target was rejected"
    // (beyond-EOF line, overlapping group) so the server response can report partial
    // failure instead of conflating it with full success.
    std::function<OpenBufferEditResult(const std::vector<CodeActionEdit>&)>
        apply_workspace_edit_to_open_buffers;
    // Run the closed-file disk edits of a SERVER-INITIATED WorkspaceEdit off the
    // shell thread (a server rename can touch thousands of closed files). Bound to a
    // shell background-executor dispatch that calls LspService::RunClosedFileEdits;
    // the on-disk changes are picked up by the file-index watcher. Null in headless
    // setups, where LspService falls back to a synchronous apply (TD-2026-07-17-011).
    std::function<void(std::vector<LspClosedFileEditBucket>)> run_closed_file_edits_async;
    // Resolve a setting value (project-over-user) for LSP feature gating: the lsp.*
    // toggles plus inlay-hint gating (editor.inlay_hints.enabled). Bound to
    // WorkspaceShell::GetSettingValue; null in headless setups (treated as "on").
    std::function<std::optional<std::string>(std::string_view)> get_setting_value;
    // Reconcile shell state after an LSP resource op renamed a path on disk:
    // retarget open tabs (unsaved contents preserved), diagnostics, and plugin
    // decorations. Bound to PathMutationCoordinator::ReconcileAfterExternalRename;
    // null in headless setups (disk-only apply).
    std::function<void(const std::filesystem::path&, const std::filesystem::path&)>
        reconcile_tabs_after_resource_rename;
    // Close tabs + clear diagnostics/decorations for a path an LSP resource op
    // deleted. Bound to PathMutationCoordinator::ReconcileAfterExternalDelete;
    // null in headless setups.
    std::function<void(const std::filesystem::path&)> reconcile_tabs_after_resource_delete;
    // Refresh the file tree / project search once after a resource-op batch
    // mutated paths on disk. Null in headless setups.
    std::function<void(const std::filesystem::path&)> refresh_views_after_resource_ops;
    // Dispose the staged backups of applied delete/overwrite ops off the shell
    // thread (removing a staged directory can be slow). Null → removed
    // synchronously.
    std::function<void(std::vector<std::filesystem::path>)> dispose_staged_paths_async;
  };

  LspService() = default;
  // Blocks (bounded) on any still-retiring clients — app teardown only. Moving the
  // block here (instead of the per-project ~LspManager) is the point of the
  // host-owned retirement pool: project switches no longer stall (TD-2026-07-17-091).
  ~LspService();

  void Configure(WorkspaceContext& context, CompletionRegistry& completion_registry,
                 CodeActionRegistry& code_action_registry, Operations operations);
  void SetWakeEventType(Uint32 event_type);
  // Live pointer to the host-owned active theme used to bake semantic-token colors
  // (the address is stable; a theme switch mutates it in place). Optional: when
  // null, semantic-token publishing is skipped.
  void SetTheme(const render::Theme* theme);

  // Reconcile running state to the current `lsp.*` feature settings. Called from
  // WorkspaceShell::ApplyLiveSettings when the settings revision changes. When the
  // master switch is off it stops the active project's servers and drops all
  // LSP-owned diagnostics/semantic decorations; when on it clears or re-requests the
  // two decoration-backed features (diagnostics, semantic tokens) to match. Idempotent.
  void ReconcileFeatureSettings();

  // Provider-presence queries for the active editable viewport.
  std::string ActiveLanguageIdForProvider() const;
  bool HasActiveCompletionProvider() const;
  bool HasActiveCodeActionProvider() const;
  bool HasActiveDefinitionProvider() const;
  bool HasActiveReferencesProvider() const;

  // Per-project server manager access and main-thread callback pump.
  LspManager& CurrentLspManager();
  const LspManager& CurrentLspManager() const;
  LspManager& EnsureProjectLspManager(ProjectWorkspaceState& state);
  void ConsumeLspCallbacks();

  // Begin async shutdown of the CURRENT project's LSP servers and move the retiring
  // clients into the host-owned pool, so a following project-state teardown never
  // blocks the shell thread on their shutdown handshake. The pool drains each frame
  // via ConsumeLspCallbacks and only blocks (bounded) at app teardown (~LspService).
  void RetireCurrentProjectServers();

  // Tests: number of clients still draining in the host-owned retirement pool.
  std::size_t RetiringClientCountForTesting() const { return retiring_clients_.size(); }

  // Semantic tone of the active server's status, derived from typed readiness
  // state rather than substring-matching the label. `Idle` = calm/ready/off,
  // `Busy` = starting/indexing/request-in-flight, `Error` = failed to start.
  enum class LspStatusSeverity : std::uint8_t { Idle, Busy, Error };

  // Status-bar readiness for the active server.
  LspClient::ReadinessSnapshot ActiveLspReadinessSnapshot(bool ensure_started = true);
  // When `severity` is non-null it receives the typed tone for the produced
  // status text, so callers never re-derive tone from the label text.
  void ActiveLspStatusStrings(bool ensure_started, std::string& text, std::string& tooltip,
                              LspStatusSeverity* severity = nullptr);

  // In-flight request indicator (drives the "LSP: working..." status segment).
  void BeginTrackedLspRequest();
  void FinishTrackedLspRequest();
  void ExpireTrackedLspRequestIfNeeded();

  // Deferred hydration for a newly-activated editor document. `ScheduleBufferOpen`
  // records the path (latest wins) without touching the LSP client;
  // `ConsumeDeferredBufferOpen` runs the actual didOpen + semantic-token/inlay-hint
  // requests, but only if the recorded path is still the active editable buffer
  // (a rapid second switch supersedes it). The host drains this after the
  // tab-switch frame is presented so a large file's hydration never blocks the tab
  // becoming visible (TD-2026-07-17A-033). Returns true when hydration ran.
  void ScheduleBufferOpen(const std::filesystem::path& path);
  bool ConsumeDeferredBufferOpen();
  bool HasPendingBufferOpen() const { return pending_buffer_open_.has_value(); }

  // Document lifecycle / synchronization.
  LspClient* LspClientForViewport(const editor::TextViewport& viewport, std::string* language_id);
  // `precomputed_uri`, when non-empty, is used verbatim instead of re-deriving the
  // document URI from the viewport path (FileUriForPath does path normalization +
  // percent-encoding). The per-keystroke sync path already computes the URI once and
  // threads it in so the encode does not run twice per keystroke.
  void EnsureLspDocumentOpen(const editor::TextViewport& viewport, LspClient& client,
                             std::string_view language_id,
                             std::string_view precomputed_uri = {});
  void PublishLspDiagnostics(ProjectWorkspaceState& state, std::string uri,
                             lsp_encoding::PositionEncoding encoding,
                             std::vector<LspClient::Diagnostic> diagnostics);
  // Request textDocument/semanticTokens/full for `viewport` and publish the
  // recolor decorations under owner "lsp:semantic" when the response arrives.
  // Each request bumps a per-URI generation captured in the response closure so a
  // response superseded by a newer request (the buffer changed meanwhile) is
  // dropped in PublishLspSemanticTokens instead of painting stale colors.
  void RequestLspSemanticTokens(const editor::TextViewport& viewport, LspClient& client);
  void PublishLspSemanticTokens(ProjectWorkspaceState& state, std::string uri,
                                std::uint64_t request_generation,
                                lsp_encoding::PositionEncoding encoding,
                                std::vector<std::string> legend,
                                std::vector<LspClient::SemanticToken> tokens);
  // Request textDocument/inlayHint for `viewport`'s document and publish the
  // resulting mid-line virtual text under owner "lsp:inlay". Gated on
  // editor.inlay_hints.enabled and the server's inlayHintProvider. Generation-
  // guarded exactly like semantic tokens so a response the buffer has moved past
  // is dropped rather than positioned against stale text.
  void RequestLspInlayHints(const editor::TextViewport& viewport, LspClient& client);
  void PublishLspInlayHints(ProjectWorkspaceState& state, std::string uri,
                            std::uint64_t request_generation,
                            lsp_encoding::PositionEncoding encoding,
                            std::vector<LspClient::InlayHint> hints);

  // Request textDocument/codeLens for `viewport`'s document and publish the results
  // as "lsp:codelens" CodeLensDecorations (rendered end-of-line, or as above-line
  // strips under `plugins.code_lens_above`). Pulled on the same triggers as inlay
  // hints — didOpen, save, and a clean-landing undo/redo — never per keystroke.
  // Lenses the server returned without a command are filled in through
  // codeLens/resolve first, so servers that only answer that way (rust-analyzer,
  // typescript-language-server) are not silently blank; the whole document
  // publishes once, after the last resolve lands.
  void RequestLspCodeLenses(const editor::TextViewport& viewport, LspClient& client);
  void PublishLspCodeLenses(ProjectWorkspaceState& state, std::string uri,
                            std::uint64_t request_generation, std::string language_id,
                            lsp_encoding::PositionEncoding encoding,
                            std::vector<LspClient::CodeLens> lenses);
  // Run the server command behind a clicked code lens (workspace/executeCommand).
  // A handle from a superseded publish resolves to nothing and is ignored — the
  // lens the click landed on no longer exists.
  void ActivateCodeLens(std::uint64_t payload);
  // Drop the "lsp:codelens" decorations for `viewport`'s file. Same reasoning as
  // the inlay overlay: the lens line numbers are absolute, so any content edit
  // invalidates them until the next clean pull.
  void ClearLspCodeLensesForFile(const editor::TextViewport& viewport);

  // Keep the caret's semantic occurrence highlights fresh. Drained once per
  // presented frame (WorkspaceShell::OnFramePresented), because a caret can move
  // through mouse, keyboard, search, go-to-definition and folding paths and this is
  // the one place all of them converge. The steady state is four integer compares
  // against the last request's identity — a request only goes out when the active
  // buffer, its content revision, or the caret position actually changed, so
  // holding a cursor still costs nothing. Suppressed while the caret is being
  // driven by active typing, which is exactly when the render path suppresses
  // occurrence highlighting anyway.
  void MaybeRequestDocumentHighlights();
  // Store a documentHighlight response as the active buffer's semantic occurrence
  // set (converting server positions to byte columns) and repaint. Drops a
  // response superseded by a newer request or by a content edit.
  void PublishLspDocumentHighlights(ProjectWorkspaceState& state,
                                    const std::filesystem::path& path,
                                    std::uint64_t request_generation,
                                    std::uint64_t content_revision,
                                    lsp_encoding::PositionEncoding encoding,
                                    std::size_t caret_line, std::size_t caret_column,
                                    std::vector<LspClient::DocumentHighlight> highlights);
  void SyncLspForActiveEditableChange(const std::vector<std::string>& before_lines,
                                      const std::vector<std::string>& after_lines);
  // Compact description of a bulk buffer change, replacing the two full
  // before/after line-vector snapshots callers used to materialize (TD-2026-07-17A-
  // 015/016). The AFTER content is read live from the viewport (streamed for
  // didChange); the diagnostic-shift only needs the pre-change line count plus the
  // first line whose content differs, both cheap to compute at the change site.
  struct BufferChangeDelta {
    std::size_t before_line_count = 0;
    std::size_t first_changed_line = 0;  // 0-based; ignored when line counts match
  };
  // Full-document re-sync for an arbitrary edited buffer (not necessarily the
  // active one). Used by multi-buffer workspace edits so every edited buffer's
  // server mirror + stored diagnostics stay in sync, not just the active tab's.
  // Reads the post-change text straight from `viewport` (no after-snapshot).
  void SyncLspForBufferChange(const editor::TextViewport& viewport, BufferChangeDelta delta);
  void SyncLspForActiveEditableLastChange();

  // Outcome of a silent on-disk WorkspaceEdit application (VSCode-style rename
  // across unopened files), for a preview/feedback summary.
  struct DiskEditResult {
    std::size_t files_written = 0;
    std::size_t edits_applied = 0;
    bool any_failed = false;
  };
  // Apply `edits` to files that are NOT open in any editor, directly on disk: each
  // target file is loaded into a scratch viewport (so line-ending / BOM / encoding
  // detection and the atomic, permission-preserving save path are reused), its
  // ranged edits are applied with the server's negotiated position encoding
  // (highest-position-first), then it is saved — no tab is opened, no active editor
  // state is touched. `is_open(normalized_path)` returns true for a path the caller
  // edits in place (skipped here); an empty edit path (the active buffer) is always
  // skipped. Reuses the per-file position-encoding mapping of ApplyLspWorkspaceEdit.
  DiskEditResult ApplyLspEditsToClosedFilesOnDisk(
      const std::vector<CodeActionEdit>& edits,
      const std::function<bool(const std::filesystem::path&)>& is_open);

  // Load/apply/save each prepared closed-file bucket (highest-position-first, atomic
  // save). Pure — touches only its arguments + the filesystem (each file via a
  // private scratch viewport), so it is safe to run on a background thread. Static.
  static DiskEditResult RunClosedFileEdits(const std::vector<LspClosedFileEditBucket>& buckets);

  // Outcome of applying a WorkspaceEdit's file resource ops.
  struct ResourceOpsResult {
    bool ok = true;           // every op applied (or was skipped per its ignore option)
    bool any_applied = false; // at least one op mutated the filesystem
    std::string error;        // first failure, for the log / notification
  };
  // Validate + apply `ops` in order with rollback-safe staging (TD-2026-07-17-011):
  // every op's preconditions are checked against a simulated overlay BEFORE anything
  // mutates, deletes/overwrites move the old content aside (a same-directory rename,
  // not a removal) so a mid-flight I/O failure rolls every completed op back, and
  // only a fully-applied batch disposes its staged backups (off the shell thread).
  // Every target must live inside the project root — an op that escapes it fails
  // the whole batch. On success, open tabs/diagnostics are reconciled per op and
  // the project views refresh once. Main thread only.
  ResourceOpsResult ApplyWorkspaceResourceOps(const std::vector<WorkspaceResourceOp>& ops);

  // Apply a client-initiated WorkspaceEdit that carries resource ops: the ops run
  // first (validate-first + rollback-safe), then the text edits — open buffers in
  // place (via the Operations hook), closed files silently on disk (an
  // ops-carrying edit typically fills the file it just created, which no buffer
  // has open). Returns false when the op batch failed (nothing left applied);
  // after the ops land, text-edit failures follow the abort-at-first-failure
  // contract for resource-carrying edits. Main thread only.
  bool ApplyFullWorkspaceEdit(const std::vector<CodeActionEdit>& edits,
                              const std::vector<WorkspaceResourceOp>& resource_ops);

  // True when every versioned TextDocumentEdit in `edit` matches the version
  // `client` currently tracks for that document (documents not open on the client
  // pass — there is no local version to conflict with). False means the server
  // computed the edit against text the user has since changed; the edit must not
  // be applied (LSP: the client fails the request on a version mismatch).
  static bool WorkspaceEditVersionsCurrent(const LspClient& client,
                                           const LspClient::WorkspaceEdit& edit);

 private:
  // Group `edits` by closed-file path (dropping open paths + the active buffer) and
  // resolve each file's server position encoding on the main thread (path-only
  // filetype detection + started-server lookup — both touch shared state that must
  // stay main-thread). The heavy load/apply/save is deferred to RunClosedFileEdits.
  std::vector<LspClosedFileEditBucket> PrepareClosedFileBuckets(
      const std::vector<CodeActionEdit>& edits,
      const std::function<bool(const std::filesystem::path&)>& is_open);
  // Apply a server-initiated WorkspaceEdit on the main thread: versioned edits are
  // gated against `client`'s tracked document versions (stale → whole edit fails),
  // resource ops run first (rollback-safe), then open buffers edit in place (via
  // the Operations hook) and closed files write silently on disk. Returns true
  // when the edit was accepted. Bound as the LspClient apply-edit handler.
  bool ApplyServerWorkspaceEdit(LspClient& client, LspClient::WorkspaceEdit edit);
  // True when `normalized` (a lexically-normal path) is open in a hydrated editor
  // tab of the current project.
  bool IsPathOpenInProject(const std::filesystem::path& normalized) const;

  ProjectWorkspaceState& CurrentProjectState();
  const ProjectWorkspaceState& CurrentProjectState() const;

  // Shared preamble for the two didChange paths: resolve the server for `viewport`,
  // compute its document URI once, capture whether the document was already open
  // (BEFORE opening it — a fresh didOpen carries the current buffer text, so the
  // caller must skip the following didChange or it double-applies and desyncs the
  // server), then ensure it is open. Returns nullopt when no server serves the
  // buffer. Allocation-free beyond the URI string (no std::function on the hot path).
  struct BufferSyncTarget {
    LspClient* client = nullptr;
    std::string uri;
    std::string language_id;
    bool was_open = false;
  };
  std::optional<BufferSyncTarget> ResolveOpenDocumentForSync(const editor::TextViewport& viewport);

  // Shared publish scaffolding for the generation-guarded absolute-position overlays
  // (semantic tokens, inlay hints): drop a superseded response, resolve the file's
  // path + open viewport, let `build` produce the decoration payload from `items`
  // (the only per-overlay part), then replace it under `owner_key` and redraw. The
  // `build` callable is `(const editor::TextViewport*, lsp_encoding::PositionEncoding,
  // std::vector<Item>&) -> editor::PluginDecorationData`. Defined in LspService.cpp
  // (only instantiated there).
  template <typename Item, typename Build>
  void PublishGuardedOverlay(ProjectWorkspaceState& state, std::string_view owner_key,
                             std::unordered_map<std::string, std::uint64_t>& generations,
                             const std::string& uri, std::uint64_t request_generation,
                             lsp_encoding::PositionEncoding encoding, std::vector<Item> items,
                             Build build);

  // Per-URI overlay generation guard, shared by the semantic-token and inlay-hint
  // overlays (each owns its own generation map). NextOverlayGeneration bumps and
  // returns the current value (captured in a request's response closure);
  // OverlayGenerationCurrent reports whether a captured value is still the latest,
  // i.e. not superseded by a later request or a clear.
  static std::uint64_t NextOverlayGeneration(
      std::unordered_map<std::string, std::uint64_t>& generations, const std::string& uri);
  static bool OverlayGenerationCurrent(
      const std::unordered_map<std::string, std::uint64_t>& generations, const std::string& uri,
      std::uint64_t generation);

  // Apply `transform` to every stored "lsp" diagnostic range for `viewport`'s file
  // (single-sources the owner tag + path). Instantiated only in LspService.cpp.
  template <typename Transform>
  void TransformLspDiagnostics(const editor::TextViewport& viewport, Transform&& transform);

  // Keep stored "lsp" diagnostics positioned as the buffer changes, before the
  // server republishes. The single-edit path maps positions precisely through the
  // viewport's last applied edit; the bulk path (paste/undo/format) shifts
  // diagnostics below the first changed line by the net line delta.
  void ShiftLspDiagnosticsForAppliedEdit(const editor::TextViewport& viewport);
  // `after_line_count` is read from the viewport; only the pre-change line count
  // and the first differing line (both supplied by the change site) are needed to
  // slide diagnostics below the edit by the net line delta.
  void ShiftLspDiagnosticsForBulkChange(const editor::TextViewport& viewport,
                                        std::size_t before_line_count,
                                        std::size_t first_changed_line);

  // Drop the absolute-positioned "lsp:semantic" recolor overlay for `viewport`'s
  // file. The overlay is invalid the moment the buffer's line/column geometry
  // shifts, so any content edit clears it (the lexical highlighter keeps painting
  // until a fresh semantic response lands); a stale overlay would otherwise paint
  // wrong colors once the buffer returns to a clean, render-visible state.
  void ClearLspSemanticTokensForFile(const editor::TextViewport& viewport);

  // Drop the "lsp:inlay" mid-line virtual text for `viewport`'s file (mirrors the
  // semantic overlay: any content edit invalidates the absolute hint positions).
  void ClearLspInlayHintsForFile(const editor::TextViewport& viewport);

  WorkspaceContext* context_ = nullptr;
  CompletionRegistry* completion_registry_ = nullptr;
  CodeActionRegistry* code_action_registry_ = nullptr;
  const render::Theme* theme_ = nullptr;
  Operations operations_{};
  Uint32 wake_event_type_ = 0;

  // Path of an editor document whose LSP hydration was scheduled by tab activation
  // and not yet run. Drained post-frame by ConsumeDeferredBufferOpen so the didOpen
  // + token/inlay requests do not block the tab switch (TD-2026-07-17A-033).
  std::optional<std::filesystem::path> pending_buffer_open_;

  // Per-URI monotonic generation for semantic-token requests. Bumped on every
  // request; the response closure captures its value and PublishLspSemanticTokens
  // drops any response whose captured generation is no longer current (superseded
  // by a later request because the buffer changed).
  std::unordered_map<std::string, std::uint64_t> semantic_token_generation_;
  // Per-URI monotonic generation for inlay-hint requests (same stale-response
  // guard as semantic tokens). Shares NextSemanticGeneration's helper pattern via
  // a dedicated map so the two overlays never invalidate each other.
  std::unordered_map<std::string, std::uint64_t> inlay_hint_generation_;

  // Per-URI generation guard for code-lens requests (same stale-response rule as
  // the two overlays above).
  std::unordered_map<std::string, std::uint64_t> code_lens_generation_;
  // Executable payloads behind the published lenses. A CodeLensDecoration cannot
  // carry a server command with JSON arguments, so it carries a handle into here
  // instead; a click resolves the handle back to something runnable. Rebuilt per
  // publish (each publish drops the previous entries for that URI), so it stays
  // proportional to what is currently on screen rather than growing forever.
  struct CodeLensCommand {
    std::string uri;
    std::string language_id;  // which server to run the command on
    std::string command;
    std::vector<util::JsonValue> arguments;
  };
  std::unordered_map<std::uint64_t, CodeLensCommand> code_lens_commands_;
  std::uint64_t next_code_lens_payload_ = 0;

  // Identity of the last documentHighlight request, so MaybeRequestDocumentHighlights
  // fires only on a real change. A single counter (not a per-URI map like the two
  // overlays above) because only one caret is ever live: a newer request always
  // supersedes the outstanding one, whatever buffer it was for.
  std::uint64_t document_highlight_generation_ = 0;
  std::filesystem::path document_highlight_request_path_;
  std::uint64_t document_highlight_request_revision_ = 0;
  std::size_t document_highlight_request_line_ = 0;
  std::size_t document_highlight_request_column_ = 0;
  bool document_highlight_request_valid_ = false;

  // Last feature-enablement state applied by ReconcileFeatureSettings, so it acts
  // only on actual transitions (shut down / clear / re-request) instead of redoing
  // work on every unrelated settings mutation.
  struct FeatureEnablement {
    bool master = false;
    bool diagnostics = false;
    bool semantic = false;
  };
  std::optional<FeatureEnablement> last_feature_enablement_;

  // Host-owned graveyard of clients whose servers are shutting down. Outlives every
  // per-project LspManager, so a project switch hands its retiring clients here and
  // is not blocked by their shutdown handshake. Drained (non-blocking reap of
  // completed clients) each frame by DrainRetiringClients (TD-2026-07-17-091).
  std::vector<std::unique_ptr<LspClient>> retiring_clients_;
  void RetireClients(std::vector<std::unique_ptr<LspClient>> clients);
  void DrainRetiringClients();
};

}  // namespace microide::workspace
