#include "workspace/WorkspaceShell.h"

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "util/JsonValue.h"
#include "workspace/ControlSpec.h"
#include "workspace/WorkspaceCommandPromptCoordinator.h"

// Thin forwarders to the host-owned protocol-client services so existing
// render/menu/plugin call sites stay unchanged. The LSP glue lives in
// LspService (src/workspace/LspService.*); the DAP glue lives in DebugService
// (src/workspace/DebugService.*). Kept in one companion TU to respect the
// WorkspaceShell*.cpp companion-count invariant — the behavior is on the
// services, only these thin shells live here.
namespace microide::workspace {

namespace {
bool ControlSettingIsOn(const std::optional<std::string>& value) {
  return value.has_value() && *value != "false" && *value != "0" && *value != "off";
}

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
                                            std::string& tooltip) {
  lsp_service_.ActiveLspStatusStrings(ensure_started, text, tooltip);
}

std::string WorkspaceShell::ActiveLspStatusText(bool ensure_started) {
  return lsp_service_.ActiveLspStatusText(ensure_started);
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

void WorkspaceShell::PublishLspDiagnostics(ProjectWorkspaceState& state, std::string uri,
                                           std::vector<LspClient::Diagnostic> diagnostics) {
  lsp_service_.PublishLspDiagnostics(state, std::move(uri), std::move(diagnostics));
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
  outcome.ok = MakeCommandPromptCoordinator().ExecuteCommandLine(command_line);
  const std::string& feedback = context_.current_project_state.panel.command.feedback_text;
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
  // the ordering trap where structured breakpoints/launch silently no-op.
  if ((!spec.breakpoints.empty() || spec.launch.has_value()) && !DebugEnabled()) {
    apply_setting("debug.enabled", "true");
  }

  const std::vector<std::string> commands =
      ControlSpecToCommands(spec, context_.current_project_state.root);
  for (const std::string& command : commands) {
    emit_applied(command, ExecuteControlCommand(command));
  }
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
  if (ControlSettingIsOn(GetSettingValue("control.enabled"))) {
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

namespace {
std::string ToLowerAscii(std::string_view text) {
  std::string lowered(text);
  for (char& c : lowered) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return lowered;
}
}  // namespace

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
    picker.items.push_back(LaunchConfigPickerItem{
        .config_index = i,
        .primary_label = config.name.empty() ? config.type : config.name,
        .secondary_label = std::move(secondary),
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
  const std::string query = ToLowerAscii(picker.query.text());
  for (const LaunchConfigPickerItem& item : picker.items) {
    if (!query.empty()) {
      const std::string haystack = ToLowerAscii(item.primary_label + " " + item.secondary_label);
      if (haystack.find(query) == std::string::npos) {
        continue;
      }
    }
    picker.matches.push_back(item);
  }
  picker.summary_line =
      std::to_string(picker.matches.size()) + " of " + std::to_string(picker.items.size());
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
  const std::optional<std::string> value = GetSettingValue("debug.enabled");
  return value.has_value() && *value != "false" && *value != "0" && *value != "off";
}

void WorkspaceShell::DebugContinue() { debug_service_.Continue(); }
void WorkspaceShell::DebugStepOver() { debug_service_.StepOver(); }
void WorkspaceShell::DebugStepIn() { debug_service_.StepIn(); }
void WorkspaceShell::DebugStepOut() { debug_service_.StepOut(); }
void WorkspaceShell::DebugPause() { debug_service_.Pause(); }
void WorkspaceShell::DebugRestart() { debug_service_.Restart(); }
void WorkspaceShell::DebugFocusThread(int thread_id) { debug_service_.FocusThread(thread_id); }

void WorkspaceShell::AppendDebugConsoleOutput(int session_id, const std::string& label,
                                              const dap_protocol::DapOutputEvent& output) {
  const std::string channel_id = DebugConsoleChannelId(session_id);
  const std::string channel_label = DebugConsoleChannelLabel(label);
  // Split on newlines so each console line is its own channel entry. A trailing
  // newline (common in adapter output) does not produce a spurious blank entry.
  const std::string& text = output.output;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t newline = text.find('\n', start);
    if (newline == std::string::npos) {
      output_channels_.AppendLine(channel_id, channel_label, text.substr(start));
      break;
    }
    output_channels_.AppendLine(channel_id, channel_label, text.substr(start, newline - start));
    start = newline + 1;
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

void WorkspaceShell::RemoveDebugConsole(int session_id) {
  const std::string channel_id = DebugConsoleChannelId(session_id);
  // Close the tab first (advances the active output channel if this was it), then
  // drop the channel's backing data.
  CloseOutputChannelTab(channel_id);
  output_channels_.RemoveChannel(channel_id);
  RequestBottomPanelRedraw();
}

}  // namespace microide::workspace
