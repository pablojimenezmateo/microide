#include "workspace/shell/WorkspaceShell.h"
#include "workspace/services/EditorTabService.h"

#include "workspace/services/RemoteHostService.h"
#include "workspace/shell/ShellGlueCache.h"

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

namespace microide::workspace {

namespace {
std::string DebugConsoleChannelId(int session_id) {
  return "debug.console." + std::to_string(session_id);
}
std::string DebugConsoleChannelLabel(const std::string& session_label) {
  return session_label.empty() ? std::string("Debug Console") : session_label;
}
}

LspManager& WorkspaceShell::CurrentLspManager() { return lsp_service_.CurrentLspManager(); }


void WorkspaceShell::ConsumeLspCallbacks() { lsp_service_.ConsumeLspCallbacks(); }

const LspClient::ReadinessSnapshot& WorkspaceShell::MenuLspReadiness() const {
  const std::uint64_t frame_id = prepared_frame_id_;
  if (frame_id == 0 || frame_id != menu_lsp_readiness_frame_id_) {
    menu_lsp_readiness_ = const_cast<WorkspaceShell*>(this)->lsp_service_.ActiveLspReadinessSnapshot(
        false);
    menu_lsp_readiness_frame_id_ = frame_id;
  }
  return menu_lsp_readiness_;
}

void WorkspaceShell::ActiveLspStatusStrings(bool ensure_started, std::string& text,
                                            std::string& tooltip, StatusTone& tone) {
  LspService::LspStatusSeverity severity = LspService::LspStatusSeverity::Idle;
  lsp_service_.ActiveLspStatusStrings(ensure_started, text, tooltip, &severity);
  switch (severity) {
    case LspService::LspStatusSeverity::Idle:
      tone = StatusTone::Default;
      break;
    case LspService::LspStatusSeverity::Busy:
      tone = StatusTone::Info;
      break;
    case LspService::LspStatusSeverity::Error:
      tone = StatusTone::Error;
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

constexpr std::size_t kMaxOutlineSymbolNodes = 5000;

void SortOutlineSiblingsByPosition(std::vector<plugin::PluginHost::DocumentSymbolNode>& nodes) {
  std::stable_sort(nodes.begin(), nodes.end(),
                   [](const plugin::PluginHost::DocumentSymbolNode& lhs,
                      const plugin::PluginHost::DocumentSymbolNode& rhs) {
                     return lhs.line != rhs.line ? lhs.line < rhs.line : lhs.column < rhs.column;
                   });
}

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

}

void WorkspaceShell::QueryDocumentSymbols(const editor::TextViewport& viewport,
                                          DocumentSymbolsReady deliver) {
  std::filesystem::path path = viewport.path();
  plugin_runtime_.Host().QueryDocumentSymbolsAsync(
      viewport.language_id(), path,
      [this, path, deliver = std::move(deliver)](
          std::vector<plugin::PluginHost::DocumentSymbolNode> symbols, std::string plugin_error) mutable {
        const editor::TextViewport* current = ActiveEditorViewport();
        if (!symbols.empty() || current == nullptr || current->path() != path) {
          deliver(path, plugin_error, std::move(symbols));
          return;
        }
        QueryLspDocumentSymbolsInto(*current, std::move(path), std::move(plugin_error), std::move(deliver));
      });
}

void WorkspaceShell::QueryLspDocumentSymbolsInto(const editor::TextViewport& viewport,
                                                 std::filesystem::path request_path,
                                                 std::string plugin_error,
                                                 DocumentSymbolsReady apply) {
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
  std::string uri = FileUriForPath(request_path);
  client->RequestDocumentSymbolAsync(
      std::move(uri),
      [this, request_path = std::move(request_path), plugin_error = std::move(plugin_error),
       encoding, apply](LspResult<std::vector<LspClient::DocumentSymbol>> symbols) {
        FinishTrackedLspRequest();
        editor::TextViewport* current = ActiveEditorViewport();
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

void WorkspaceShell::SyncLspForActiveEditableLastChange() {
  lsp_service_.SyncLspForActiveEditableLastChange();
}

DapManager& WorkspaceShell::CurrentDapManager() { return debug_service_.CurrentDapManager(); }


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
  std::string& feedback = context_.current_project_state.panel.feedback.text;
  feedback.clear();
  (void)MakeEditorTabService().TakeLandedSaveFailures();
  outcome.ok = MakeCommandLineCoordinator().ExecuteCommandLine(command_line);
  file_io_.writes.Flush();
  if (const std::vector<std::string> failed = MakeEditorTabService().TakeLandedSaveFailures(); !failed.empty()) {
    outcome.ok = false;
    feedback = failed.front();
  }
  if (ModalOverlayLostFocus(context_.current_project_state)) {
    const FocusTarget kept = context_.current_project_state.surface.focus;
    DismissOverlay();
    context_.current_project_state.surface.focus = kept;
  }
  if (glue_->remote_host_service != nullptr) {
    glue_->remote_host_service->SetHostViewVisible(HostViewShown(context_.current_project_state.sidebar),
                                                   context_.current_project_state.root);
  }
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
    outcome.ok = SetSettingValue(id, value, false);
    if (!outcome.ok) {
      outcome.error = "unknown setting or invalid value";
    }
    emit_applied("set-setting " + id + " " + value, outcome);
  };

  for (const auto& [id, value] : spec.settings) {
    apply_setting(id, value);
  }

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
  return SetSettingValue("debug.enabled", "true", false);
}

void WorkspaceShell::ForceStartControlChannel() {
  if (!startup_options_.control_stdout) {
    return;
  }
  control_channel_service_.SetStdoutMirror(true);
  (void)control_channel_service_.Start(context_.current_project_state.root);
}

void WorkspaceShell::MaybeStartControlChannel() {
  if (startup_options_.control_stdout) {
    return;
  }
  if (SettingFlagEnabled(GetSettingValue("control.enabled"))) {
    (void)control_channel_service_.Start(context_.current_project_state.root);
  } else {
    control_channel_service_.Stop();
  }
}

bool WorkspaceShell::StartDebugging(const LaunchConfig& config, const std::string& cwd) {
  return debug_service_.StartDebugging(config, cwd);
}

std::string WorkspaceShell::StartDebuggingWithDefaultConfig() {
  const std::vector<std::string> types = CurrentDapManager().AdapterTypes();
  if (types.empty()) {
    return "no debug adapter is registered (a plugin must contribute one via ctx.debug.add)";
  }

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

void WorkspaceShell::StopDebugging() {
  const bool last_session = debug_service_.SessionCount() <= 1;
  debug_service_.StopDebugging();
  if (last_session) {
    MakeDebugPaneService().Close();
  }
  RequestPreparedFrame();
}

bool WorkspaceShell::IsDebugSessionStopped() const {
  return debug_service_.SessionState() == DebugSession::State::Stopped;
}

bool WorkspaceShell::DebugEnabled() const {
  return SettingFlagEnabled(GetSettingValue("debug.enabled"));
}

void WorkspaceShell::AppendDebugConsoleOutput(int session_id, const std::string& label,
                                              const dap_protocol::DapOutputEvent& output) {
  const std::string channel_id = DebugConsoleChannelId(session_id);
  const std::string channel_label = DebugConsoleChannelLabel(label);
  const std::string& text = output.output;
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
  RequestPreparedFrame();
}

void WorkspaceShell::ShowDebugOutput() {
  const int session_id = debug_service_.ActiveSessionId();
  if (session_id == 0) {
    return;
  }
  ShowDebugConsole(session_id, debug_service_.ActiveSessionLabel());
  context_.current_project_state.panel.output.follow_tail = true;
}

void WorkspaceShell::RemoveDebugConsole(int session_id) {
  const std::string channel_id = DebugConsoleChannelId(session_id);
  CloseOutputChannelTab(channel_id);
  output_channels_.RemoveChannel(channel_id);
  RequestPreparedFrame();
}

}
