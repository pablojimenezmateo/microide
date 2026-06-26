#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/DiagnosticsStore.h"
#include "editor/PluginDecorationStore.h"
#include "editor/PluginSurfaceStore.h"
#include "platform/SubprocessSandbox.h"
#include "util/JsonValue.h"

namespace microide::plugin {

class PluginThread;

class PluginHost {
 public:
  struct OpenFileRequest {
    std::filesystem::path path;
    std::size_t line = 0;
    std::size_t column = 0;
  };

  struct ActiveBuffer {
    std::filesystem::path path;
    std::size_t line = 0;
    std::size_t column = 0;
  };

  struct SidebarProviderInfo {
    std::string id;
    std::string label;
    std::string plugin_id;
  };

  struct SidebarItem {
    std::string label;
    std::string detail;
    std::filesystem::path path;
    std::size_t line = 0;
    std::size_t column = 0;
    // Tree-view fields. A plugin sidebar snapshot may return a flattened tree:
    // the plugin owns expand/collapse state and returns only the currently
    // visible rows, each tagged with its `depth` (0 = root). `collapsible`
    // rows draw a host-owned twisty; toggling one routes back through the
    // provider's `on_toggle(item)` callback (keyed by `id`) so the plugin can
    // re-shape its next snapshot. Flat lists simply leave these at defaults.
    std::string id;
    int depth = 0;
    bool collapsible = false;
    bool collapsed = false;
  };

  struct HoverResult {
    std::string title;
    std::string content;

    bool operator==(const HoverResult&) const = default;
  };

  struct ContributedMenuEntry {
    std::string id;
    std::string menu;           // "file", "edit", "view", "search"
    std::string action;         // built-in command name or plugin command name
    std::string label;
    std::string accelerator;
    bool separator_before = false;
    std::string plugin_id;
  };

  struct ContributedKeybinding {
    std::string id;
    std::string action;         // command name
    std::string key_chord;      // "Ctrl+S" format
    std::string context;        // "global", "editor", "sidebar", "terminal"
    std::string plugin_id;
  };

  struct ContributedSettingSpec {
    std::string id;
    std::string label;
    std::string description;
    std::string type;           // "bool", "int", "float", "string", "enum"
    std::string scope;          // "user", "project"
    std::string default_value;  // string-serialised default
    std::vector<std::string> enum_values;
    std::string plugin_id;
  };

  struct ContributedStatusItem {
    std::string id;
    std::string text;
    std::string tooltip;
    std::string alignment;      // "left", "right"
    int priority = 0;
    // Phase D enrichments. `icon` is a gutter-icon name resolved to a shape at
    // the workspace boundary; `tone` is one of "default"/"info"/"warning"/"error";
    // `command` (when non-empty) runs on click; `progress` < 0 means "no bar",
    // otherwise it is clamped to [0, 1] and drawn as a sub-bar.
    std::string icon;
    std::string tone;
    std::string command;
    float progress = -1.0f;
    std::string plugin_id;
  };

  struct ContributedFormatter {
    std::string id;
    std::string language_id;
    std::string label;
    std::vector<std::string> command;
    std::string plugin_id;
  };

  struct ContributedSaveParticipant {
    std::string id;
    std::string plugin_id;
  };

  struct ContributedCompletion {
    std::string id;
    std::string language_id;
    std::string trigger_characters;
    std::string plugin_id;
  };

  struct ContributedCodeAction {
    std::string id;
    std::string language_id;
    std::string plugin_id;
  };

  struct ContributedLanguageServer {
    std::string id;
    // One server process serves every language id in this list (e.g. clangd
    // covers c / c++ / objective-c). The host shares a single subprocess across
    // them via LspManager aliasing.
    std::vector<std::string> language_ids;
    std::vector<std::string> command;
    std::string plugin_id;
    // Forwarded verbatim as LSP `initializationOptions` (object) or Null.
    util::JsonValue initialization_options;
    // Answers server `workspace/configuration` requests (object) or Null.
    util::JsonValue settings;
    // Kernel-confinement descriptor resolved at registration from the plugin's project root,
    // data dir, and network capability; applied to the spawned server child on Linux.
    platform::SubprocessSandbox sandbox;
  };

  struct ContributedDebugAdapter {
    std::string id;
    // DAP adapter type id matched by a LaunchConfig's `type` (mirrors how an
    // LSP `language_id` selects a server). A single project may contribute
    // several adapters (debugpy, lldb, ...), each with its own type.
    std::string type;
    std::vector<std::string> command;
    std::string plugin_id;
    // Kernel-confinement descriptor resolved at registration. Debug adapters
    // typically need ptrace + broad fs access, so the resolved sandbox is more
    // permissive than the LSP/formatter default.
    platform::SubprocessSandbox sandbox;
  };

  // A launch/attach configuration a plugin contributes via ctx.debug.addConfig.
  // `type` selects a ContributedDebugAdapter; `arguments_json` is the verbatim
  // launch/attach request body serialized as JSON text (the host parses it into
  // a LaunchConfig). Persisted per-project alongside breakpoints.
  struct ContributedLaunchConfig {
    std::string id;
    std::string name;
    std::string type;
    std::string request;  // "launch" or "attach"
    std::string arguments_json;
    std::string plugin_id;
  };

  struct ContributedTask {
    std::string id;
    std::string label;
    std::string group;
    std::vector<std::string> command;
    std::string cwd;
    bool run_in_shell = false;
    std::string plugin_id;
  };

  struct ContributedTool {
    std::string id;
    std::string label;
    std::string platform;
    std::string download_url;
    std::string sha256;
    std::string install_dir;
    std::string plugin_id;
  };

  struct ContributedTestProvider {
    std::string id;
    std::string language_id;
    std::string plugin_id;
  };

  struct CompletionCandidate {
    std::string label;
    std::string detail;
    std::string documentation;
    std::string insert_text;
    bool is_snippet = false;
  };

  struct CodeActionCandidate {
    std::string title;
    std::string command;
    std::vector<std::string> arguments;
  };

  // One ranged text edit inside a WorkspaceEditRequest. All positions are 1-based
  // (matching the rest of the plugin position vocabulary); `end_*` is exclusive.
  // The host validates and clamps every edit against the live buffer before
  // applying, so out-of-range values are corrected, never trusted.
  struct EditRequest {
    std::size_t start_line = 0;
    std::size_t start_column = 0;
    std::size_t end_line = 0;
    std::size_t end_column = 0;
    std::string text;
  };

  // A host-owned text-edit request from a plugin (`ctx.editor.apply_edits`). The
  // host applies all `edits` as a single grouped undo step on the resolved buffer
  // (empty `path` => the active editable buffer; a named path must be open), then
  // optionally moves the caret / sets the selection. Unlike `save_participants`
  // (a whole-document transform at save time, producing no undo entry), this is a
  // ranged, undoable edit through the real viewport edit primitives.
  struct WorkspaceEditRequest {
    std::filesystem::path path;     // empty => active editable buffer
    std::vector<EditRequest> edits;  // 1-based; applied atomically (one undo step)
    bool has_cursor = false;
    std::size_t cursor_line = 0;     // 1-based
    std::size_t cursor_column = 0;   // 1-based
    bool has_selection = false;
    std::size_t selection_start_line = 0;
    std::size_t selection_start_column = 0;
    std::size_t selection_end_line = 0;
    std::size_t selection_end_column = 0;
  };

  // A ghost-text (inline AI suggestion) proposal from a plugin
  // (`ctx.editor.set_ghost_text`). The plugin proposes a dimmed, multi-line
  // completion anchored at a 1-based caret position; the host validates it
  // against the live caret (active editable buffer, cursor still there, not
  // stale), owns rendering, and inserts it on Tab. Single-owner: a publish
  // replaces any existing ghost text. Dormant (no per-frame cost) until a
  // plugin publishes.
  struct GhostTextRequest {
    std::filesystem::path path;     // empty => active editable buffer
    std::size_t anchor_line = 0;    // 1-based; 0 => use the current caret line
    std::size_t anchor_column = 0;  // 1-based; 0 => use the current caret column
    std::string text;               // full suggestion, '\n'-separated
  };

  // A navigation target produced by a plugin go-to-definition / find-references
  // provider. `line`/`column` are 1-based; `path` is resolved against the
  // current project root by the host.
  struct LocationResult {
    std::filesystem::path path;
    std::size_t line = 0;
    std::size_t column = 0;
  };

  struct SignatureParameter {
    std::string label;
    std::string documentation;
  };

  struct SignatureInfo {
    std::string label;
    std::string documentation;
    std::vector<SignatureParameter> parameters;
    int active_parameter = -1;
  };

  struct SignatureHelpResult {
    std::vector<SignatureInfo> signatures;
    int active_signature = 0;
  };

  // A node in a plugin-provided document outline. `kind` is a free-form label
  // (e.g. "function", "class"); `line`/`column` are 1-based and point at the
  // symbol's selection range. Children nest arbitrarily (host-bounded depth).
  struct DocumentSymbolNode {
    std::string name;
    std::string detail;
    std::string kind;
    std::size_t line = 0;
    std::size_t column = 0;
    std::vector<DocumentSymbolNode> children;
  };

  struct TestCase {
    std::string id;
    std::string label;
    std::filesystem::path file;
    int line = 0;
    std::string parent_id;
  };

  struct TestRunResult {
    std::string test_id;
    std::string state;
    std::string message;
    int duration_ms = 0;
  };

  struct ScmEntry {
    std::filesystem::path path;
    std::filesystem::path relative_path;
    std::string status;
    bool conflicted = false;
    bool staged = false;
    bool supports_stage = false;
    bool supports_discard = false;
  };

  struct ScmSnapshot {
    std::string base_ref;
    std::string base_label;
    std::vector<ScmEntry> entries;
    bool supports_mutations = false;
  };

  struct ContributedScmProvider {
    std::string id;
    std::string label;
    std::string plugin_id;
  };

  struct AnnotationLine {
    std::size_t line = 0;
    std::string text;
    std::string author;
    std::string summary;
    std::string date;
  };

  struct ContributedAnnotationProvider {
    std::string id;
    std::string label;
    std::string type;  // "blame", "decoration", "margin"
    std::string language_id;
    std::string plugin_id;
  };

  struct ContributedAuthProvider {
    std::string id;
    std::string label;
    std::string plugin_id;
  };

  struct AuthSessionData {
    std::string id;
    std::string account;
    std::string access_token;
    std::vector<std::string> scopes;
  };

  struct ContributedAiProvider {
    std::string id;
    std::string label;
    std::string type;  // "cloud", "local", "external"
    std::vector<std::string> models;
    std::string runtime;            // "sidecar", "openai_compat", "anthropic_messages"
    std::string base_url;
    std::string default_model;
    std::string plugin_id;
  };

  struct ContributedExternalAgent {
    std::string id;
    std::string label;
    std::string protocol;  // "acp", "stdio", "http"
    std::vector<std::string> command;
    std::vector<std::string> capabilities;
    std::string plugin_id;
  };

  struct ContributedMcpTool {
    std::string id;
    std::string name;
    std::string description;
    std::string input_schema;
    std::string plugin_id;
  };

  // Plugin-contributed bracket pair set for a single language. The host merges
  // these on top of built-in defaults during `WorkspaceLanguageContract::Refresh`.
  struct ContributedBracketSet {
    std::string language_id;
    std::vector<std::pair<std::string, std::string>> bracket_pairs;
    std::vector<std::pair<std::string, std::string>> auto_close_pairs;
    std::vector<std::pair<std::string, std::string>> surround_pairs;
    std::string plugin_id;
  };

  // Plugin-contributed comment markers for a single language.
  struct ContributedCommentMarkers {
    std::string language_id;
    std::string line_comment;
    std::string block_comment_open;
    std::string block_comment_close;
    std::string plugin_id;
  };

  // Plugin-contributed indent hints for a single language.
  struct ContributedIndentRules {
    std::string language_id;
    std::vector<std::string> indent_after_open_patterns;
    std::vector<std::string> dedent_on_close_chars;
    std::string plugin_id;
  };

  // Plugin-contributed snippet entry. `id` is host-namespaced (`"<plugin>.<id>"`).
  struct ContributedSnippet {
    std::string id;
    std::string language_id;
    std::string prefix;
    std::string label;
    std::string body;
    std::string plugin_id;
  };

  // One highlight-group override inside a plugin-contributed theme. Mirrors a
  // `.microide` `color-link` row; absent colours stay nullopt so the host's
  // theme derivation falls back to its computed defaults.
  struct ContributedThemeStyle {
    std::string group;  // lower-cased highlight-group name (e.g. "comment")
    std::optional<SDL_Color> foreground;
    std::optional<SDL_Color> background;
    bool reverse = false;
  };

  // Plugin-contributed colour theme. `id` is host-namespaced (`"<plugin>.<id>"`)
  // and selectable as a colorscheme; the host derives a full render::Theme from
  // `styles` via the same path that loads `.microide` files.
  struct ContributedTheme {
    std::string id;
    std::string label;
    std::vector<ContributedThemeStyle> styles;
    std::string plugin_id;
  };

  // One file-icon rule: match by extension (without the dot) or by exact
  // filename, mapping to a built-in gutter-icon shape name + colour.
  struct ContributedFileIconRule {
    std::string matcher;          // extension (no dot) or full filename, lower-cased
    bool match_filename = false;  // false = extension match, true = whole filename
    std::string icon;             // gutter-icon shape name (e.g. "diamond")
    SDL_Color color{};
  };

  // Plugin-contributed file-icon theme. Rules override the host's built-in
  // extension defaults; later-registered themes win on conflicts.
  struct ContributedFileIconTheme {
    std::string id;
    std::string label;
    std::vector<ContributedFileIconRule> rules;
    std::string plugin_id;
  };

  struct Callbacks {
    std::function<bool(std::string_view)> is_command_name_available;
    std::function<bool(const OpenFileRequest&)> open_file;
    std::function<std::optional<ActiveBuffer>()> active_buffer;
    std::function<bool(std::string_view)> show_sidebar;
    std::function<void(std::string_view,
                       const std::filesystem::path&,
                       std::vector<editor::Diagnostic>)>
        publish_diagnostics;
    std::function<void(std::string_view, const std::filesystem::path&)> clear_file_diagnostics;
    std::function<void(std::string_view)> clear_owner_diagnostics;
    // Plugin-published editor decorations (inline text styles, gutter marks,
    // inline/virtual text, code lenses). A publish replaces the owner's full
    // decoration set for the file; clears mirror the diagnostics callbacks.
    std::function<void(std::string_view,
                       const std::filesystem::path&,
                       editor::PluginDecorationData)>
        publish_decorations;
    std::function<void(std::string_view, const std::filesystem::path&)> clear_file_decorations;
    std::function<void(std::string_view)> clear_owner_decorations;
    // Plugin-published content surfaces (Phase E). `publish_surface` replaces one
    // (owner, surface_id) atomically; clears mirror the decoration callbacks.
    // `decode_raster` hands encoded/raw image bytes to the host texture cache for
    // off-thread decode + shell-thread upload, keyed by content hash. `format` is
    // 0 = PNG/JPEG (decode), 1 = raw RGBA8 (validate only).
    std::function<void(std::string_view, std::string_view, editor::SurfaceContent)>
        publish_surface;
    std::function<void(std::string_view, std::string_view)> clear_surface;
    std::function<void(std::string_view)> clear_owner_surfaces;
    std::function<void(std::uint64_t, int, std::vector<std::byte>, int, int)> decode_raster;
    // Apply a plugin-requested ranged text edit (`ctx.editor.apply_edits`) on the
    // host thread. Returns true when the edit was applied to a resolved buffer.
    // The host owns validation, undo grouping, caret placement, and redraw; the
    // plugin never touches the buffer directly. Dormant (no per-frame cost) until
    // a plugin issues an edit.
    std::function<bool(std::string_view owner, const WorkspaceEditRequest&)> apply_workspace_edit;
    // Publish / clear ghost text (`ctx.editor.set_ghost_text` /
    // `clear_ghost_text`) on the host thread. The host validates the anchor
    // against the live caret, stores at most one suggestion (last writer wins),
    // owns rendering, and inserts it on Tab. `clear_ghost_text` only clears a
    // suggestion owned by the caller. Dormant until a plugin publishes.
    std::function<void(std::string_view owner, const GhostTextRequest&)> publish_ghost_text;
    std::function<void(std::string_view owner)> clear_ghost_text;
    std::function<void(const std::string&)> error_sink;
    std::function<void(const std::string&)> log_sink;
    std::function<std::optional<std::string>(std::string_view)> get_setting;
    std::function<void()> request_status_redraw;
    // Post a transient, auto-dismissing notification ("toast"). level is one of
    // "info"/"warning"/"error" (host maps unknown values to info).
    std::function<void(const std::string& level, const std::string& message)> show_notification;
  };

  // One discovered plugin, for the host-owned plugin-management UI. Disabled plugins
  // are still listed (so they can be re-enabled) but their setup never runs.
  struct LoadedPlugin {
    std::string id;
    std::filesystem::path root;
    bool enabled = true;
  };

  PluginHost();
  ~PluginHost();
  PluginHost(const PluginHost&) = delete;
  PluginHost& operator=(const PluginHost&) = delete;
  PluginHost(PluginHost&&) noexcept;
  PluginHost& operator=(PluginHost&&) noexcept;

  void SetCallbacks(Callbacks callbacks);
  // Wire the dedicated worker thread that runs all plugin Lua off the UI thread.
  // Null leaves the host in inline (UI-thread) mode.
  void SetWorker(PluginThread* worker);
  bool enabled() const;
  void SetStartupPluginsEnabled(bool enabled);
  // Plugin ids whose setup should be skipped on the next Reload. They still appear in
  // LoadedPlugins() as disabled so the UI can re-enable them.
  void SetDisabledPlugins(std::vector<std::string> disabled_ids);
  bool Reload(const std::filesystem::path& project_root);
  void Shutdown();
  void OnBufferOpen(const std::filesystem::path& path);
  void OnBufferSave(const std::filesystem::path& path);
  // Which reactive editor events at least one loaded plugin subscribes to. The
  // host queries this once per plugin reload and gates its per-keystroke sampling
  // on it, so a project with no subscribing plugin pays nothing.
  struct EditorEventInterest {
    bool buffer_change = false;
    bool cursor_move = false;
    bool selection_change = false;
    bool buffer_close = false;
    bool any() const {
      return buffer_change || cursor_move || selection_change || buffer_close;
    }
  };
  EditorEventInterest EditorEventInterests() const;
  // Reactive editor events (SEAM 1). `OnBufferChange` reports a 1-based inclusive
  // changed-line range; `OnCursorMove` a 1-based caret; `OnSelectionChange` a
  // 1-based range (all zero => selection cleared); `OnBufferClose` mirrors save.
  void OnBufferChange(const std::filesystem::path& path,
                      std::size_t start_line,
                      std::size_t end_line);
  void OnCursorMove(const std::filesystem::path& path, std::size_t line, std::size_t column);
  void OnSelectionChange(const std::filesystem::path& path,
                         bool has_selection,
                         std::size_t start_line,
                         std::size_t start_column,
                         std::size_t end_line,
                         std::size_t end_column);
  void OnBufferClose(const std::filesystem::path& path);
  bool ExecuteCommand(std::string_view name,
                      const std::vector<std::string>& args,
                      std::string* error_message = nullptr,
                      std::string* feedback = nullptr);
  const std::vector<std::string>& CommandNames() const;
  const std::vector<SidebarProviderInfo>& SidebarProviders() const;
  const SidebarProviderInfo* FindSidebarProvider(std::string_view id) const;
  bool SnapshotSidebar(std::string_view id,
                       std::vector<SidebarItem>* items,
                       std::string* error_message = nullptr);
  bool ConfirmSidebarItem(std::string_view id,
                          const SidebarItem& item,
                          std::string* error_message = nullptr);
  // Toggle a collapsible tree row. Returns false (without setting an error) when
  // the provider declares no `on_toggle` callback, so callers can treat toggling
  // as a no-op for flat sidebars. Re-snapshot after a successful toggle.
  bool ToggleSidebarItem(std::string_view id,
                         const SidebarItem& item,
                         std::string* error_message = nullptr);
  bool QueryHover(const std::filesystem::path& path,
                  std::size_t line,
                  std::size_t column,
                  HoverResult* result,
                  std::string* error_message = nullptr) const;
  std::vector<std::filesystem::path> DataDirectories(std::string_view subdirectory) const;
  const std::vector<ContributedMenuEntry>& ContributedMenuEntries() const;
  const std::vector<ContributedKeybinding>& ContributedKeybindings() const;
  const std::vector<ContributedSettingSpec>& ContributedSettings() const;
  const std::vector<ContributedStatusItem>& ContributedStatusItems() const;
  // Stamp that changes whenever the contributed status items change. Callers that
  // resolve the items into a sorted render view cache it against this value.
  std::uint64_t StatusItemsRevision() const;
  bool UpdateStatusItem(std::string_view id, std::string text, std::string tooltip = {});
  bool RunSaveParticipants(const std::filesystem::path& path,
                           std::string* text,
                           std::string* error_message = nullptr) const;
  std::vector<CompletionCandidate> QueryCompletions(std::string_view language_id,
                                                    const std::filesystem::path& path,
                                                    std::size_t line,
                                                    std::size_t column,
                                                    std::string_view trigger_character = {},
                                                    std::string* error_message = nullptr) const;
  // Async variant: dispatches the completion query to the plugin worker and never
  // blocks the UI thread. `on_result(items, error)` runs on the UI thread during
  // the next mailbox drain. Superseded requests (a newer query for the same kind)
  // are dropped before they run. When no worker is wired the query runs inline and
  // `on_result` is invoked synchronously before returning.
  void QueryCompletionsAsync(
      std::string language_id,
      std::filesystem::path path,
      std::size_t line,
      std::size_t column,
      std::string trigger_character,
      std::function<void(std::vector<CompletionCandidate>, std::string)> on_result);
  std::vector<CodeActionCandidate> QueryCodeActions(std::string_view language_id,
                                                    const std::filesystem::path& path,
                                                    std::size_t start_line,
                                                    std::size_t start_column,
                                                    std::size_t end_line,
                                                    std::size_t end_column,
                                                    std::string* error_message = nullptr) const;
  // Plugin-native language providers. Each merges results across all matching
  // providers for `language_id`; an empty result means "no plugin handled it"
  // so callers can fall back to LSP.
  std::vector<LocationResult> QueryDefinition(std::string_view language_id,
                                              const std::filesystem::path& path,
                                              std::size_t line,
                                              std::size_t column,
                                              std::string* error_message = nullptr) const;
  std::vector<LocationResult> QueryReferences(std::string_view language_id,
                                              const std::filesystem::path& path,
                                              std::size_t line,
                                              std::size_t column,
                                              bool include_declaration,
                                              std::string* error_message = nullptr) const;
  bool QuerySignatureHelp(std::string_view language_id,
                          const std::filesystem::path& path,
                          std::size_t line,
                          std::size_t column,
                          SignatureHelpResult* result,
                          std::string* error_message = nullptr) const;
  std::vector<DocumentSymbolNode> QueryDocumentSymbols(
      std::string_view language_id,
      const std::filesystem::path& path,
      std::string* error_message = nullptr) const;
  bool DiscoverTests(std::string_view provider_id,
                     const std::filesystem::path& path,
                     std::vector<TestCase>* tests,
                     std::string* error_message = nullptr) const;
  bool RunTests(std::string_view provider_id,
                const std::vector<std::string>& test_ids,
                std::vector<TestRunResult>* results,
                std::string* error_message = nullptr) const;
  bool SnapshotScm(std::string_view provider_id,
                   ScmSnapshot* snapshot,
                   std::string* error_message = nullptr) const;
  std::vector<AnnotationLine> QueryAnnotations(std::string_view provider_id,
                                               const std::filesystem::path& path,
                                               std::string_view language_id,
                                               std::size_t visible_start_line,
                                               std::size_t visible_end_line,
                                               std::string* error_message = nullptr) const;
  bool LoginAuthProvider(std::string_view provider_id,
                         const std::vector<std::string>& scopes,
                         AuthSessionData* session,
                         std::string* error_message = nullptr) const;
  bool RefreshAuthSession(std::string_view provider_id,
                          std::string_view session_id,
                          AuthSessionData* session,
                          std::string* error_message = nullptr) const;
  bool LogoutAuthSession(std::string_view provider_id,
                         std::string_view session_id,
                         std::string* error_message = nullptr) const;
  const std::vector<ContributedFormatter>& ContributedFormatters() const;
  const std::vector<ContributedSaveParticipant>& ContributedSaveParticipants() const;
  const std::vector<ContributedCompletion>& ContributedCompletions() const;
  const std::vector<ContributedCodeAction>& ContributedCodeActions() const;
  const std::vector<ContributedLanguageServer>& ContributedLanguageServers() const;
  const std::vector<ContributedDebugAdapter>& ContributedDebugAdapters() const;
  const std::vector<ContributedLaunchConfig>& ContributedLaunchConfigs() const;
  const std::vector<ContributedTask>& ContributedTasks() const;
  const std::vector<ContributedTool>& ContributedTools() const;
  const std::vector<ContributedTestProvider>& ContributedTestProviders() const;
  const std::vector<ContributedScmProvider>& ContributedScmProviders() const;
  const std::vector<ContributedAnnotationProvider>& ContributedAnnotationProviders() const;
  const std::vector<ContributedAuthProvider>& ContributedAuthProviders() const;
  const std::vector<ContributedBracketSet>& ContributedBrackets() const;
  const std::vector<ContributedCommentMarkers>& ContributedComments() const;
  const std::vector<ContributedIndentRules>& ContributedIndents() const;
  const std::vector<ContributedSnippet>& ContributedSnippets() const;
  const std::vector<ContributedTheme>& ContributedThemes() const;
  const std::vector<ContributedFileIconTheme>& ContributedFileIconThemes() const;
  const std::vector<std::string>& Messages() const;
  const std::vector<std::string>& Errors() const;
  void ClearMessages();
  std::string ReloadSummary() const;
  std::size_t LoadedPluginCount() const;
  // All discovered plugins (enabled and disabled), sorted by id, for the plugin UI.
  std::vector<LoadedPlugin> LoadedPlugins() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace microide::plugin
