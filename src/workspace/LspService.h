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
    // Resolve a setting value (project-over-user) for LSP feature gating: the lsp.*
    // toggles plus inlay-hint gating (editor.inlay_hints.enabled). Bound to
    // WorkspaceShell::GetSettingValue; null in headless setups (treated as "on").
    std::function<std::optional<std::string>(std::string_view)> get_setting_value;
  };

  LspService() = default;

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
  std::string ActiveLspStatusText(bool ensure_started = true);
  std::string ActiveLspStatusTooltip(bool ensure_started = true);

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

 private:
  // Apply a server-initiated WorkspaceEdit on the main thread: open buffers in
  // place (via the Operations hook), closed files silently on disk. Returns true
  // when at least one edit was applied. Bound as the LspClient apply-edit handler.
  bool ApplyServerWorkspaceEdit(LspClient::WorkspaceEdit edit);
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

  // Last feature-enablement state applied by ReconcileFeatureSettings, so it acts
  // only on actual transitions (shut down / clear / re-request) instead of redoing
  // work on every unrelated settings mutation.
  struct FeatureEnablement {
    bool master = false;
    bool diagnostics = false;
    bool semantic = false;
  };
  std::optional<FeatureEnablement> last_feature_enablement_;
};

}  // namespace microide::workspace
