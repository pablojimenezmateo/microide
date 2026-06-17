#include "workspace/WorkspaceShell.h"

#include <string>
#include <string_view>
#include <vector>

// Thin forwarders to the host-owned protocol-client services so existing
// render/menu/plugin call sites stay unchanged. The LSP glue lives in
// LspService (src/workspace/LspService.*); the DAP glue lives in DebugService
// (src/workspace/DebugService.*). Kept in one companion TU to respect the
// WorkspaceShell*.cpp companion-count invariant — the behavior is on the
// services, only these thin shells live here.
namespace microide::workspace {

namespace {
constexpr std::string_view kDebugConsoleChannelId = "debug.console";
constexpr std::string_view kDebugConsoleChannelLabel = "Debug Console";
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

bool WorkspaceShell::StartDebugging(const LaunchConfig& config, const std::string& cwd) {
  const bool started = debug_service_.StartDebugging(config, cwd);
  if (started) {
    ShowDebugConsole();
  }
  return started;
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

void WorkspaceShell::StopDebugging() { debug_service_.StopDebugging(); }

void WorkspaceShell::ResendBreakpointsForFile(const std::filesystem::path& path) {
  debug_service_.ResendBreakpointsForFile(path);
}

bool WorkspaceShell::IsDebugSessionActive() const { return debug_service_.IsSessionActive(); }

void WorkspaceShell::AppendDebugConsoleOutput(const dap_protocol::DapOutputEvent& output) {
  // Split on newlines so each console line is its own channel entry. A trailing
  // newline (common in adapter output) does not produce a spurious blank entry.
  const std::string& text = output.output;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t newline = text.find('\n', start);
    if (newline == std::string::npos) {
      output_channels_.AppendLine(kDebugConsoleChannelId, kDebugConsoleChannelLabel,
                                  text.substr(start));
      break;
    }
    output_channels_.AppendLine(kDebugConsoleChannelId, kDebugConsoleChannelLabel,
                                text.substr(start, newline - start));
    start = newline + 1;
  }
}

void WorkspaceShell::ShowDebugConsole() {
  EnsureOutputChannelTabOpen(kDebugConsoleChannelId);
  output_channels_.EnsureChannel(kDebugConsoleChannelId, kDebugConsoleChannelLabel);
  context_.current_project_state.panel.content = PanelContentKind::Output;
  context_.current_project_state.panel.output.channel_id = std::string(kDebugConsoleChannelId);
  RequestBottomPanelRedraw();
}

}  // namespace microide::workspace
