#include "workspace/shell/WorkspaceShell.h"

#include <algorithm>

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "util/JsonValue.h"
#include "util/StringUtil.h"
#include "workspace/control/ControlSpec.h"
#include "workspace/FileUri.h"
#include "workspace/WorkspaceUiText.h"
#include "workspace/lsp/LspFeatureFlags.h"
#include "workspace/lsp/LspViewportPositions.h"
#include "workspace/SettingFlags.h"
#include "workspace/coordinators/WorkspaceCommandLineCoordinator.h"
#include "workspace/coordinators/WorkspaceSidebarCoordinator.h"

// Thin forwarders to the host-owned protocol-client services so existing
// render/menu/plugin call sites stay unchanged. The LSP glue lives in
// LspService (src/workspace/LspService.*); the DAP glue lives in DebugService
// (src/workspace/DebugService.*). Kept in one companion TU to respect the
// WorkspaceShell*.cpp companion-count invariant — the behavior is on the
// services, only these thin shells live here.
namespace microide::workspace {

namespace {
// One console channel per debug session, keyed by session id. The label defaults
// to the session name; a generic fallback covers an unnamed session.
std::string DebugConsoleChannelId(int session_id) {
  return "debug.console." + std::to_string(session_id);
}
std::string DebugConsoleChannelLabel(const std::string& session_label) {
  return session_label.empty() ? std::string("Debug Console") : session_label;
}
}  // namespace

bool WorkspaceShell::HasActiveCompletionProvider() const {
  return lsp_service_.HasActiveCompletionProvider();
}

bool WorkspaceShell::HasActiveCodeActionProvider() const {
  return lsp_service_.HasActiveCodeActionProvider();
}

bool WorkspaceShell::HasActiveDefinitionProvider() const {
  return lsp_service_.HasActiveDefinitionProvider();
}

bool WorkspaceShell::HasActiveReferencesProvider() const {
  return lsp_service_.HasActiveReferencesProvider();
}

LspManager& WorkspaceShell::CurrentLspManager() { return lsp_service_.CurrentLspManager(); }

const LspManager& WorkspaceShell::CurrentLspManager() const {
  return lsp_service_.CurrentLspManager();
}

LspManager& WorkspaceShell::EnsureProjectLspManager(ProjectWorkspaceState& state) {
  return lsp_service_.EnsureProjectLspManager(state);
}

void WorkspaceShell::ConsumeLspCallbacks() { lsp_service_.ConsumeLspCallbacks(); }

LspClient::ReadinessSnapshot WorkspaceShell::ActiveLspReadinessSnapshot(bool ensure_started) {
  return lsp_service_.ActiveLspReadinessSnapshot(ensure_started);
}

void WorkspaceShell::ActiveLspStatusStrings(bool ensure_started, std::string& text,
                                            std::string& tooltip, StatusBarSegmentTone& tone) {
  LspService::LspStatusSeverity severity = LspService::LspStatusSeverity::Idle;
  lsp_service_.ActiveLspStatusStrings(ensure_started, text, tooltip, &severity);
  switch (severity) {
    case LspService::LspStatusSeverity::Idle:
      tone = StatusBarSegmentTone::Default;
      break;
    case LspService::LspStatusSeverity::Busy:
      tone = StatusBarSegmentTone::Info;
      break;
    case LspService::LspStatusSeverity::Error:
      tone = StatusBarSegmentTone::Error;
      break;
  }
}

void WorkspaceShell::BeginTrackedLspRequest() { lsp_service_.BeginTrackedLspRequest(); }

void WorkspaceShell::FinishTrackedLspRequest() { lsp_service_.FinishTrackedLspRequest(); }

LspClient* WorkspaceShell::LspClientForViewport(const editor::TextViewport& viewport,
                                                std::string* language_id) {
  return lsp_service_.LspClientForViewport(viewport, language_id);
}

void WorkspaceShell::EnsureLspDocumentOpen(const editor::TextViewport& viewport, LspClient& client,
                                           std::string_view language_id) {
  lsp_service_.EnsureLspDocumentOpen(viewport, client, language_id);
}

namespace {

// LSP SymbolKind index -> the lowercase kind label the outline sidebar shows (it
// mirrors the strings plugin document-symbol providers return).
std::string_view LspSymbolKindName(int kind) {
  switch (kind) {
    case 1: return "file";
    case 2: return "module";
    case 3: return "namespace";
    case 4: return "package";
    case 5: return "class";
    case 6: return "method";
    case 7: return "property";
    case 8: return "field";
    case 9: return "constructor";
    case 10: return "enum";
    case 11: return "interface";
    case 12: return "function";
    case 13: return "variable";
    case 14: return "constant";
    case 15: return "string";
    case 16: return "number";
    case 17: return "boolean";
    case 18: return "array";
    case 19: return "object";
    case 20: return "key";
    case 21: return "null";
    case 22: return "enum-member";
    case 23: return "struct";
    case 24: return "event";
    case 25: return "operator";
    case 26: return "type-parameter";
    default: return "symbol";
  }
}

// Presentation cap on adapted outline nodes. The protocol parser tolerates up to
// kMaxLspSymbolNodes (100k) as a transport backstop, but the outline sidebar only
// needs a human-navigable count; adapting AND flattening 100k nodes on the
// main-thread request callback would spend a long frame allocating strings, mapping
// columns, and building rows. Stop adapting past this budget and surface a marker.
constexpr std::size_t kMaxOutlineSymbolNodes = 5000;

// Siblings in document order, as VS Code's outline shows them by default. Servers
// usually return them that way, but the protocol does not promise it and the
// flat SymbolInformation shape in particular often arrives grouped by kind; a
// stable sort keeps the server's order for symbols that start at one position.
void SortOutlineSiblingsByPosition(std::vector<plugin::PluginHost::DocumentSymbolNode>& nodes) {
  std::stable_sort(nodes.begin(), nodes.end(),
                   [](const plugin::PluginHost::DocumentSymbolNode& lhs,
                      const plugin::PluginHost::DocumentSymbolNode& rhs) {
                     return lhs.line != rhs.line ? lhs.line < rhs.line : lhs.column < rhs.column;
                   });
}

// Adapt an LSP DocumentSymbol tree onto the plugin DocumentSymbolNode tree the
// outline flatten path consumes. `line`/`column` become 1-based, and the column is
// mapped from the server's position encoding to an editor byte column so the
// outline navigates to the right spot on non-ASCII lines. `budget` bounds the total
// adapted node count across the whole (recursive) tree; the caller consumes one for
// this node before calling, and recursion stops adding children once it hits 0.
plugin::PluginHost::DocumentSymbolNode AdaptLspDocumentSymbol(
    const LspClient::DocumentSymbol& symbol, const editor::TextViewport& viewport,
    lsp_encoding::PositionEncoding encoding, std::size_t& budget) {
  const std::size_t line = static_cast<std::size_t>(std::max(0, symbol.selection_range.start.line));
  plugin::PluginHost::DocumentSymbolNode node;
  node.name = symbol.name;
  node.detail = symbol.detail;
  node.kind = std::string(LspSymbolKindName(symbol.kind));
  node.line = line + 1;
  node.column =
      LspPositionToByteColumn(viewport, line, symbol.selection_range.start.character, encoding) + 1;
  node.children.reserve(std::min<std::size_t>(symbol.children.size(), budget));
  for (const auto& child : symbol.children) {
    if (budget == 0) {
      break;
    }
    --budget;
    node.children.push_back(AdaptLspDocumentSymbol(child, viewport, encoding, budget));
  }
  SortOutlineSiblingsByPosition(node.children);
  return node;
}

}  // namespace

void WorkspaceShell::QueryLspDocumentSymbolsForOutline(const editor::TextViewport& viewport,
                                                       std::filesystem::path request_path,
                                                       std::string plugin_error) {
  const auto apply = [this](const std::filesystem::path& path, const std::string& error,
                            std::vector<plugin::PluginHost::DocumentSymbolNode> nodes) {
    // Re-create the coordinator from the (long-lived) shell so the apply never runs
    // on the transient coordinator that issued the request.
    MakeSidebarCoordinator().ApplyLspOutlineResult(path, error, nodes);
  };
  const auto get_setting = [this](std::string_view id) { return GetSettingValue(id); };
  if (!LspFeatureEnabled(get_setting, "lsp.document_symbols.enabled")) {
    apply(request_path, plugin_error, {});
    return;
  }
  std::string language_id;
  LspClient* client = LspClientForViewport(viewport, &language_id);
  if (client == nullptr) {
    apply(request_path, plugin_error, {});
    return;
  }
  EnsureLspDocumentOpen(viewport, *client, language_id);
  const lsp_encoding::PositionEncoding encoding = LspEncodingForClient(*client);
  BeginTrackedLspRequest();
  client->RequestDocumentSymbolAsync(
      FileUriForPath(request_path),
      [this, request_path = std::move(request_path), plugin_error = std::move(plugin_error),
       encoding, apply](LspResult<std::vector<LspClient::DocumentSymbol>> symbols) {
        FinishTrackedLspRequest();
        editor::TextViewport* current = ActiveEditorViewport();
        // Superseded (buffer/file switched): apply empty; ApplyLspOutlineResult
        // re-checks the path and leaves the newer view untouched.
        if (current == nullptr || current->path() != request_path) {
          apply(request_path, plugin_error, {});
          return;
        }
        std::vector<plugin::PluginHost::DocumentSymbolNode> nodes;
        if (symbols.has_value()) {
          std::size_t budget = kMaxOutlineSymbolNodes;
          nodes.reserve(std::min<std::size_t>(symbols->size(), budget));
          bool truncated = false;
          for (const auto& symbol : *symbols) {
            if (budget == 0) {
              truncated = true;
              break;
            }
            --budget;
            nodes.push_back(AdaptLspDocumentSymbol(symbol, *current, encoding, budget));
          }
          SortOutlineSiblingsByPosition(nodes);
          if (truncated) {
            // Surface a non-navigable marker row through the normal flatten path so
            // the user knows the outline was capped rather than silently missing tail
            // symbols. line/column stay 0 (unmapped) so it does not jump anywhere.
            plugin::PluginHost::DocumentSymbolNode marker;
            marker.name = "… (outline truncated)";
            nodes.push_back(std::move(marker));
          }
        }
        apply(request_path, plugin_error, std::move(nodes));
      });
}

void WorkspaceShell::PublishLspDiagnostics(ProjectWorkspaceState& state, std::string uri,
                                           lsp_encoding::PositionEncoding encoding,
                                           std::vector<LspClient::Diagnostic> diagnostics) {
  lsp_service_.PublishLspDiagnostics(state, std::move(uri), encoding, std::move(diagnostics));
}

void WorkspaceShell::SyncLspForActiveEditableChange(const std::vector<std::string>& before_lines,
                                                    const std::vector<std::string>& after_lines) {
  lsp_service_.SyncLspForActiveEditableChange(before_lines, after_lines);
}

void WorkspaceShell::SyncLspForActiveEditableLastChange() {
  lsp_service_.SyncLspForActiveEditableLastChange();
}

// ---- DAP forwarders (DebugService) ----------------------------------------

DapManager& WorkspaceShell::CurrentDapManager() { return debug_service_.CurrentDapManager(); }

const DapManager& WorkspaceShell::CurrentDapManager() const {
  return debug_service_.CurrentDapManager();
}

DapManager& WorkspaceShell::EnsureProjectDapManager(ProjectWorkspaceState& state) {
  return debug_service_.EnsureProjectDapManager(state);
}

void WorkspaceShell::ConsumeDapCallbacks() { debug_service_.ConsumeDapCallbacks(); }

void WorkspaceShell::ConsumeControlCallbacks() {
  control_channel_service_.ConsumeControlCallbacks();
}

ControlChannelService::CommandOutcome WorkspaceShell::ExecuteControlCommand(
    const std::string& command_line) {
  ControlChannelService::CommandOutcome outcome;
  // Snapshot the shared panel feedback before dispatch: it may still hold a message
  // from a prior unrelated UI action. Only attribute feedback to THIS command if the
  // command actually changed it, so an ok:true reply does not carry a misleading
  // stale line.
  const std::string feedback_before = context_.current_project_state.panel.feedback.text;
  outcome.ok = MakeCommandLineCoordinator().ExecuteCommandLine(command_line);
  const std::string& feedback_after = context_.current_project_state.panel.feedback.text;
  const std::string feedback =
      feedback_after != feedback_before ? feedback_after : std::string{};
  if (outcome.ok) {
    outcome.feedback = feedback;
  } else {
    outcome.error = feedback.empty() ? "command failed" : feedback;
  }
  return outcome;
}

void WorkspaceShell::ApplyControlSpec(const ControlSpec& spec) {
  if (!spec.valid) {
    return;
  }
  const auto emit_applied = [this](const std::string& command,
                                   const ControlChannelService::CommandOutcome& outcome) {
    // Surface one line per applied spec entry on the JSONL mirror so a headless
    // driver can see exactly what succeeded / failed (no more silent swallow).
    util::JsonObject object;
    object["applied"] = util::JsonValue(command);
    object["ok"] = util::JsonValue(outcome.ok);
    if (!outcome.ok && !outcome.error.empty()) {
      object["error"] = util::JsonValue(outcome.error);
    }
    control_channel_service_.EmitJsonLine(
        util::SerializeJson(util::JsonValue(std::move(object))));
  };
  const auto apply_setting = [&](const std::string& id, const std::string& value) {
    ControlChannelService::CommandOutcome outcome;
    outcome.ok = SetSettingValue(id, value, /*persist=*/false);
    if (!outcome.ok) {
      outcome.error = "unknown setting or invalid value";
    }
    emit_applied("set-setting " + id + " " + value, outcome);
  };

  // Spec `settings` apply transiently (never persisted) and first, so the spec can
  // turn on control.enabled / debug.enabled through the chokepoint without
  // clobbering the user's saved config.
  for (const auto& [id, value] : spec.settings) {
    apply_setting(id, value);
  }

  // Auto-enable the debugger when the spec needs it but it is still off — removes
  // the ordering trap where structured breakpoints/launch silently no-op. Emit an
  // applied line for the headless mirror when it actually flips.
  if ((!spec.breakpoints.empty() || spec.launch.has_value() ||
       !spec.function_breakpoints.empty()) &&
      EnsureDebuggerEnabledTransiently()) {
    ControlChannelService::CommandOutcome outcome;
    outcome.ok = true;
    emit_applied("set-setting debug.enabled true", outcome);
  }

  const std::vector<std::string> commands =
      ControlSpecToCommands(spec, context_.current_project_state.root);
  for (const std::string& command : commands) {
    emit_applied(command, ExecuteControlCommand(command));
  }
}

bool WorkspaceShell::EnsureDebuggerEnabledTransiently() {
  if (DebugEnabled()) {
    return false;
  }
  return SetSettingValue("debug.enabled", "true", /*persist=*/false);
}

void WorkspaceShell::ForceStartControlChannel() {
  if (!startup_options_.control_stdout) {
    return;
  }
  // Bypass the `control.enabled` gate: turn on the stdout JSONL mirror and bind
  // the socket directly. Start() emits the `ready` handshake line.
  control_channel_service_.SetStdoutMirror(true);
  control_channel_service_.Start(context_.current_project_state.root);
}

void WorkspaceShell::MaybeStartControlChannel() {
  // Headless `--control` force-starts the channel independently of the
  // `control.enabled` setting (see ForceStartControlChannel). A live settings
  // change — e.g. the cold-start spec's `set-setting debug.enabled true` — must
  // never tear that socket down: doing so strands the headless driver, which
  // owns the channel lifecycle for the whole run. Leave it running.
  if (startup_options_.control_stdout) {
    return;
  }
  if (SettingFlagEnabled(GetSettingValue("control.enabled"))) {
    control_channel_service_.Start(context_.current_project_state.root);
  } else {
    control_channel_service_.Stop();
  }
}

bool WorkspaceShell::StartDebugging(const LaunchConfig& config, const std::string& cwd) {
  // DebugService surfaces the new session's console channel via the show_debug_console
  // operation, so no separate ShowDebugConsole() call is needed here.
  return debug_service_.StartDebugging(config, cwd);
}

std::string WorkspaceShell::StartDebuggingWithDefaultConfig() {
  const std::vector<std::string> types = CurrentDapManager().AdapterTypes();
  if (types.empty()) {
    return "no debug adapter is registered (a plugin must contribute one via ctx.debug.add)";
  }

  // Prefer the project's selected launch config (persisted or plugin-contributed)
  // when its adapter type is registered; otherwise fall back to launching the
  // first registered adapter with empty arguments.
  LaunchConfig config;
  const auto& project_state = context_.current_project_state;
  const auto& configs = project_state.launch_configs;
  const auto has_registered_adapter = [&](const std::string& type) {
    return !type.empty() && CurrentDapManager().HasAdapter(type);
  };
  if (!configs.empty() &&
      project_state.selected_launch_config_index < configs.size() &&
      has_registered_adapter(configs[project_state.selected_launch_config_index].type)) {
    config = configs[project_state.selected_launch_config_index];
  } else {
    config.type = types.front();
    config.name = config.type;
    config.request = "launch";
  }

  if (!StartDebugging(config, context_.current_project_state.root.generic_string())) {
    const std::string error = debug_service_.LastError();
    return error.empty() ? "failed to start debug session" : error;
  }
  return {};
}

std::string WorkspaceShell::StartNamedDebugConfig(const std::string& name) {
  if (name.empty()) {
    return StartDebuggingWithDefaultConfig();
  }
  const std::vector<LaunchConfig>& configs = context_.current_project_state.launch_configs;
  for (std::size_t i = 0; i < configs.size(); ++i) {
    if (configs[i].name == name) {
      context_.current_project_state.selected_launch_config_index = i;
      if (!StartDebugging(configs[i], context_.current_project_state.root.generic_string())) {
        const std::string error = debug_service_.LastError();
        return error.empty() ? "failed to start debug session" : error;
      }
      return {};
    }
  }
  return "no launch config named \"" + name + "\"";
}

std::string WorkspaceShell::StartAdHocDebug(const std::string& program,
                                            const std::vector<std::string>& args,
                                            const std::string& type) {
  if (program.empty()) {
    return "debug-run: a program path is required";
  }
  const std::vector<std::string> types = CurrentDapManager().AdapterTypes();
  if (types.empty()) {
    return "no debug adapter is registered (a plugin must contribute one via ctx.debug.add)";
  }
  std::string adapter_type = type;
  if (adapter_type.empty()) {
    if (types.size() != 1) {
      std::string joined;
      for (const std::string& candidate : types) {
        if (!joined.empty()) joined += ", ";
        joined += candidate;
      }
      return "multiple debug adapters are registered (" + joined +
             "); choose one with --type <adapter>";
    }
    adapter_type = types.front();
  } else if (!CurrentDapManager().HasAdapter(adapter_type)) {
    return "no debug adapter of type \"" + adapter_type + "\" is registered";
  }

  // Resolve the program against the project root unless it is already absolute,
  // so `debug-run ./build/app` works from a headless driver with no cwd context.
  const std::filesystem::path root = context_.current_project_state.root;
  std::filesystem::path program_path(program);
  if (!program_path.is_absolute()) {
    program_path = (root / program_path).lexically_normal();
  }

  LaunchConfig config;
  config.type = adapter_type;
  config.request = "launch";
  config.name = program_path.filename().string();
  util::JsonObject arguments;
  arguments["program"] = util::JsonValue(program_path.generic_string());
  if (!args.empty()) {
    util::JsonArray arg_array;
    for (const std::string& arg : args) {
      arg_array.push_back(util::JsonValue(arg));
    }
    arguments["args"] = util::JsonValue(std::move(arg_array));
  }
  arguments["cwd"] = util::JsonValue(root.generic_string());
  config.arguments = util::JsonValue(std::move(arguments));

  if (!StartDebugging(config, root.generic_string())) {
    const std::string error = debug_service_.LastError();
    return error.empty() ? "failed to start debug session" : error;
  }
  return {};
}

void WorkspaceShell::OpenLaunchConfigPicker() {
  LaunchConfigPickerState& picker =
      context_.current_project_state.overlay.workflow.launch_config_picker;
  picker.query.SetText("");
  picker.items.clear();
  const std::vector<LaunchConfig>& configs = context_.current_project_state.launch_configs;
  picker.items.reserve(configs.size());
  for (std::size_t i = 0; i < configs.size(); ++i) {
    const LaunchConfig& config = configs[i];
    std::string secondary = config.type;
    if (!config.request.empty()) {
      secondary += " · " + config.request;
    }
    std::string primary = config.name.empty() ? config.type : config.name;
    std::string search_text = util::ToLowerAscii(primary + " " + secondary);
    picker.items.push_back(LaunchConfigPickerItem{
        .config_index = i,
        .primary_label = std::move(primary),
        .secondary_label = std::move(secondary),
        .search_text = std::move(search_text),
    });
  }
  RefreshLaunchConfigPicker();
  ShowOverlay(OverlayMode::LaunchConfigPicker);
}

void WorkspaceShell::RefreshLaunchConfigPicker() {
  LaunchConfigPickerState& picker =
      context_.current_project_state.overlay.workflow.launch_config_picker;
  picker.matches.clear();
  picker.selected_index = 0;
  const std::string query = util::ToLowerAscii(picker.query.text());
  for (const LaunchConfigPickerItem& item : picker.items) {
    if (!query.empty() && item.search_text.find(query) == std::string::npos) {
      continue;
    }
    picker.matches.push_back(item);
  }
  picker.summary_line = BuildFilteredCountSummary(picker.matches.size(), picker.items.size(),
                                                  "configurations");
  ResetOverlayScroll();
  RequestOverlayRedraw();
}

void WorkspaceShell::ConfirmLaunchConfigSelection() {
  LaunchConfigPickerState& picker =
      context_.current_project_state.overlay.workflow.launch_config_picker;
  if (picker.matches.empty() || picker.selected_index >= picker.matches.size()) {
    return;
  }
  const std::size_t config_index = picker.matches[picker.selected_index].config_index;
  const std::vector<LaunchConfig>& configs = context_.current_project_state.launch_configs;
  if (config_index >= configs.size()) {
    return;
  }
  // Persist the selection (so Start Debugging / restart reuse it) and launch.
  context_.current_project_state.selected_launch_config_index = config_index;
  StartDebugging(configs[config_index], context_.current_project_state.root.generic_string());
}

void WorkspaceShell::StopDebugging() {
  // Stops the active session. Only close the debug pane when this was the last
  // live session; with others remaining, the pane stays (re-projected by the
  // next-frame prune as the active session advances).
  const bool last_session = CurrentDapManager().SessionCount() <= 1;
  debug_service_.StopDebugging();
  if (last_session) {
    CloseDebugPane();
  }
  RequestWindowRedraw();
}

void WorkspaceShell::StopAllDebugSessions() {
  debug_service_.StopAllDebugging();
  CloseDebugPane();
  RequestWindowRedraw();
}

void WorkspaceShell::DebugFocusSession(int session_id) {
  debug_service_.FocusSession(session_id);
}

void WorkspaceShell::DebugSwitchSession(int index) {
  // index < 0 cycles to the next session; index >= 1 selects a 1-based session.
  if (index < 0) {
    debug_service_.FocusNextSession();
    return;
  }
  const std::vector<DapSessionInfo> sessions = debug_service_.Sessions();
  if (index >= 1 && static_cast<std::size_t>(index) <= sessions.size()) {
    debug_service_.FocusSession(sessions[static_cast<std::size_t>(index - 1)].id);
  }
}

void WorkspaceShell::ResendBreakpointsForFile(const std::filesystem::path& path) {
  debug_service_.ResendBreakpointsForFile(path);
  // The Breakpoints panel mirrors the line-breakpoint set; rebuild it whenever a
  // breakpoint is toggled or a modifier edited (both route through here).
  debug_service_.SyncBreakpointsPanel();
}

bool WorkspaceShell::IsDebugSessionActive() const { return debug_service_.IsSessionActive(); }

bool WorkspaceShell::IsDebugSessionStopped() const {
  return debug_service_.SessionState() == DebugSession::State::Stopped;
}

bool WorkspaceShell::DebugEnabled() const {
  return SettingFlagEnabled(GetSettingValue("debug.enabled"));
}

void WorkspaceShell::DebugContinue() { debug_service_.Continue(); }
void WorkspaceShell::DebugStepOver() { debug_service_.StepOver(); }
void WorkspaceShell::DebugStepIn() { debug_service_.StepIn(); }
void WorkspaceShell::DebugStepOut() { debug_service_.StepOut(); }
void WorkspaceShell::DebugPause() { debug_service_.Pause(); }
void WorkspaceShell::DebugReverseContinue() { debug_service_.ReverseContinue(); }
void WorkspaceShell::DebugStepBack() { debug_service_.StepBack(); }
bool WorkspaceShell::DebugSupportsReverse() const { return debug_service_.SupportsStepBack(); }
void WorkspaceShell::DebugRestart() { debug_service_.Restart(); }
void WorkspaceShell::DebugFocusThread(int thread_id) { debug_service_.FocusThread(thread_id); }

void WorkspaceShell::AppendDebugConsoleOutput(int session_id, const std::string& label,
                                              const dap_protocol::DapOutputEvent& output) {
  const std::string channel_id = DebugConsoleChannelId(session_id);
  const std::string channel_label = DebugConsoleChannelLabel(label);
  // Split on newlines so each console line is its own channel entry. A trailing
  // newline (common in adapter output) does not produce a spurious blank entry.
  const std::string& text = output.output;
  // Cap lines materialized from a single output event. A DAP message body can be
  // up to 64 MiB (WorkspaceDapClientInternal kMaxDapMessageBytes); one all-newline
  // event would otherwise fan out into ~64 M AppendLine calls on the UI thread —
  // a multi-second freeze (and heap churn) from one hostile message. The channel
  // itself is also entry-capped, so lines beyond the cap would be trimmed anyway.
  constexpr std::size_t kMaxLinesPerOutputEvent = 100000;
  std::size_t start = 0;
  std::size_t emitted = 0;
  while (start < text.size() && emitted < kMaxLinesPerOutputEvent) {
    const std::size_t newline = text.find('\n', start);
    if (newline == std::string::npos) {
      output_channels_.AppendLine(channel_id, channel_label, text.substr(start));
      break;
    }
    output_channels_.AppendLine(channel_id, channel_label, text.substr(start, newline - start));
    start = newline + 1;
    ++emitted;
  }
}

void WorkspaceShell::ShowDebugConsole(int session_id, const std::string& label) {
  const std::string channel_id = DebugConsoleChannelId(session_id);
  const std::string channel_label = DebugConsoleChannelLabel(label);
  EnsureOutputChannelTabOpen(channel_id);
  output_channels_.EnsureChannel(channel_id, channel_label);
  context_.current_project_state.panel.content = PanelContentKind::Output;
  context_.current_project_state.panel.output.channel_id = channel_id;
  RequestBottomPanelRedraw();
}

void WorkspaceShell::ShowDebugOutput() {
  const int session_id = debug_service_.ActiveSessionId();
  if (session_id == 0) {
    return;  // no active debug session — nothing to surface
  }
  // Reuse the auto-show recipe (open the channel tab + select it) and stick to the
  // tail so the latest output is visible immediately.
  ShowDebugConsole(session_id, debug_service_.ActiveSessionLabel());
  context_.current_project_state.panel.output.follow_tail = true;
}

void WorkspaceShell::RemoveDebugConsole(int session_id) {
  const std::string channel_id = DebugConsoleChannelId(session_id);
  // Close the tab first (advances the active output channel if this was it), then
  // drop the channel's backing data.
  CloseOutputChannelTab(channel_id);
  output_channels_.RemoveChannel(channel_id);
  RequestBottomPanelRedraw();
}

}  // namespace microide::workspace
