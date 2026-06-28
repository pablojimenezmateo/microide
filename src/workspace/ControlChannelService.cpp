#include "workspace/ControlChannelService.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <csignal>
#include <cerrno>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "platform/AppDirectories.h"
#include "workspace/ControlProtocol.h"
#include "workspace/WorkspaceProjectPresentation.h"
#include "workspace/DebugViewModel.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceProjectState.h"

namespace microide::workspace {

namespace {

int CurrentProcessId() {
#if defined(__unix__) || defined(__APPLE__)
  return static_cast<int>(::getpid());
#else
  return 0;
#endif
}

// Base runtime directory for control sockets/descriptors: $XDG_RUNTIME_DIR when
// set (the correct home for ephemeral per-user sockets), else the app state
// directory, else /tmp. Always the `microide` subdirectory of that base.
std::filesystem::path RuntimeBaseDir() {
  if (const char* runtime = std::getenv("XDG_RUNTIME_DIR");
      runtime != nullptr && runtime[0] != '\0') {
    return std::filesystem::path(runtime) / "microide";
  }
  const std::filesystem::path state =
      platform::ResolveAppDirectory(platform::UserDirectoryKind::State, "microide");
  if (!state.empty()) {
    return state / "run";
  }
  return std::filesystem::path("/tmp/microide");
}

std::filesystem::path SocketPathForPid(int pid) {
  return RuntimeBaseDir() / (std::to_string(pid) + ".sock");
}

// Whether a command line's verb is part of the debugger surface (and so should
// auto-enable the debugger). Prefix match on the first token covers every
// breakpoint-*/debug-* command, present and future, without an explicit list.
bool CommandTouchesDebugger(const std::string& command) {
  std::size_t start = command.find_first_not_of(" \t");
  if (start == std::string::npos) {
    return false;
  }
  const std::string_view verb(command.data() + start, command.size() - start);
  return verb.starts_with("breakpoint-") || verb.starts_with("debug-");
}

std::filesystem::path DescriptorPathForPid(int pid) {
  return RuntimeBaseDir() / "instances" / (std::to_string(pid) + ".json");
}

// Whether a process is still running. Used to drop descriptors left behind by a
// hard kill / crash (the graceful shutdown path removes its own).
bool ProcessIsAlive(int pid) {
#if defined(__unix__) || defined(__APPLE__)
  if (pid <= 0) {
    return false;
  }
  if (::kill(static_cast<pid_t>(pid), 0) == 0) {
    return true;
  }
  return errno == EPERM;  // exists but not signalable by us
#else
  (void)pid;
  return true;
#endif
}

}  // namespace

std::vector<ControlInstanceDescriptor> EnumerateControlInstances() {
  std::vector<ControlInstanceDescriptor> instances;
  const std::filesystem::path dir = RuntimeBaseDir() / "instances";
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    return instances;
  }
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(dir, ec)) {
    if (entry.path().extension() != ".json") {
      continue;
    }
    std::ifstream in(entry.path());
    if (!in) {
      continue;
    }
    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    while (!contents.empty() && (contents.back() == '\n' || contents.back() == '\r')) {
      contents.pop_back();
    }
    if (contents.empty()) {
      continue;
    }
    const std::optional<util::JsonValue> parsed = util::ParseJson(contents);
    if (!parsed.has_value() || !parsed->IsObject()) {
      continue;
    }
    const int pid = static_cast<int>((*parsed)["pid"].AsInt());
    // Drop (and prune) descriptors whose process is gone, e.g. after a crash or
    // SIGKILL that skipped the graceful teardown.
    if (!ProcessIsAlive(pid)) {
      std::error_code remove_ec;
      std::filesystem::remove(entry.path(), remove_ec);
      continue;
    }
    ControlInstanceDescriptor descriptor;
    descriptor.pid = pid;
    descriptor.socket = std::filesystem::path((*parsed)["socket"].AsString());
    descriptor.project_root = (*parsed)["project_root"].AsString();
    descriptor.project_hash = (*parsed)["project_hash"].AsString();
    descriptor.raw_json = std::move(contents);
    instances.push_back(std::move(descriptor));
  }
  return instances;
}

std::string ControlListInstancesText() {
  std::string text;
  for (const ControlInstanceDescriptor& instance : EnumerateControlInstances()) {
    text += instance.raw_json;
    text += '\n';
  }
  return text;
}

ControlChannelService::~ControlChannelService() { Stop(); }

void ControlChannelService::Configure(WorkspaceContext& context, Operations operations) {
  context_ = &context;
  operations_ = std::move(operations);
}

void ControlChannelService::SetWakeEventType(std::uint32_t event_type) {
  wake_event_type_ = event_type;
  server_.SetWakeEventType(event_type);
}

bool ControlChannelService::Start(const std::filesystem::path& project_root) {
  if (server_.IsRunning()) {
    return true;
  }
  const int pid = CurrentProcessId();
  const std::filesystem::path socket_path = SocketPathForPid(pid);
  if (!server_.Start(socket_path)) {
    return false;
  }
  server_.SetWakeEventType(wake_event_type_);

  // Write the discovery descriptor so an external tool can locate this socket by
  // project. One file per process, so no cross-process locking is needed.
  descriptor_path_ = DescriptorPathForPid(pid);
  project_root_ = project_root;
  WriteDescriptor();

  // Handshake line for the headless agent: announce pid + socket + project so the
  // stream's first line tells a driver what it is attached to. Mirrored to stdout
  // only (no clients are connected at bind time).
  util::JsonObject ready;
  ready["event"] = util::JsonValue(std::string("ready"));
  ready["pid"] = util::JsonValue(static_cast<std::int64_t>(pid));
  ready["socket"] = util::JsonValue(socket_path.generic_string());
  ready["project_root"] = util::JsonValue(project_root.generic_string());
  EmitJsonLine(SerializeControlEvent(util::JsonValue(std::move(ready))));
  return true;
}

void ControlChannelService::WriteDescriptor() {
  if (descriptor_path_.empty()) {
    return;
  }
  const int pid = CurrentProcessId();
  std::error_code ec;
  std::filesystem::create_directories(descriptor_path_.parent_path(), ec);
  util::JsonObject descriptor;
  descriptor["pid"] = util::JsonValue(static_cast<std::int64_t>(pid));
  descriptor["socket"] = util::JsonValue(SocketPathForPid(pid).generic_string());
  descriptor["project_root"] = util::JsonValue(project_root_.generic_string());
  descriptor["project_hash"] =
      util::JsonValue(project_root_.empty() ? std::string()
                                            : ProjectStateDirectoryName(project_root_));
  std::ofstream out(descriptor_path_, std::ios::trunc);
  if (out) {
    out << util::SerializeJson(util::JsonValue(std::move(descriptor)));
  }
}

void ControlChannelService::EmitJsonLine(const std::string& line) const {
  if (stdout_mirror_ && operations_.emit_jsonl) {
    operations_.emit_jsonl(line);
  }
}

void ControlChannelService::EmitEvent(util::JsonValue event) {
  const std::string line = SerializeControlEvent(event);
  if (server_.IsRunning()) {
    server_.Broadcast(line);
  }
  EmitJsonLine(line);
}

void ControlChannelService::Stop() {
  server_.Stop();
  if (!descriptor_path_.empty()) {
    std::error_code ec;
    std::filesystem::remove(descriptor_path_, ec);
    descriptor_path_.clear();
  }
}

bool ControlChannelService::IsRunning() const { return server_.IsRunning(); }

std::size_t ControlChannelService::ConnectionCount() const { return server_.ConnectionCount(); }

void ControlChannelService::ConsumeControlCallbacks() {
  // The I/O thread re-binds the listener when the advertised socket vanishes
  // mid-run; re-publish the discovery descriptor (which vanished with it) so
  // external tooling can rediscover the socket.
  if (server_.ConsumeRebound()) {
    WriteDescriptor();
  }
  const std::vector<platform::ControlInboundMessage> messages = server_.TakeInbound();
  for (const platform::ControlInboundMessage& message : messages) {
    const ControlRequest request = ParseControlRequest(message.line);
    ControlResponse response;
    response.id = request.id;
    if (!request.valid) {
      response.ok = false;
      response.error = request.parse_error;
    } else if (request.is_command()) {
      // A breakpoint-/debug- command auto-enables the debugger transiently, so a
      // headless driver never has to send `set-setting debug.enabled true` first.
      if (operations_.ensure_debugger_enabled && CommandTouchesDebugger(request.command)) {
        operations_.ensure_debugger_enabled();
      }
      CommandOutcome outcome;
      if (operations_.execute_command_line) {
        outcome = operations_.execute_command_line(request.command);
      } else {
        outcome.error = "command execution unavailable";
      }
      response.ok = outcome.ok;
      response.feedback = outcome.feedback;
      response.error = outcome.error;
    } else {
      bool ok = false;
      std::string error;
      util::JsonValue result = HandleQuery(request.query, request.args, &ok, &error);
      response.ok = ok;
      if (ok) {
        response.result = std::move(result);
      } else {
        response.error = error;
      }
    }
    const std::string serialized = SerializeControlResponse(response);
    server_.SendLine(message.connection_id, serialized);
    EmitJsonLine(serialized);
  }
}

util::JsonValue ControlChannelService::HandleQuery(const std::string& verb,
                                                   const util::JsonValue& args, bool* ok,
                                                   std::string* error) const {
  (void)args;
  *ok = true;
  if (verb == "debug-state") {
    return BuildDebugState();
  }
  if (verb == "breakpoints") {
    return BuildBreakpoints();
  }
  if (verb == "function-breakpoints") {
    return BuildFunctionBreakpoints();
  }
  if (verb == "exception-filters") {
    return BuildExceptionFilters();
  }
  if (verb == "tabs") {
    return BuildTabs();
  }
  if (verb == "projects") {
    return BuildProjects();
  }
  if (verb == "status") {
    return BuildStatus();
  }
  if (verb == "launch-configs") {
    return BuildLaunchConfigs();
  }
  if (verb == "adapters") {
    return BuildAdapters();
  }
  *ok = false;
  *error = "unknown query \"" + verb + "\"";
  return util::JsonValue(nullptr);
}

util::JsonValue ControlChannelService::BuildDebugState() const {
  util::JsonObject object;
  if (context_ == nullptr) {
    return util::JsonValue(std::move(object));
  }
  const DebugExecutionView& exec = context_->current_project_state.debug_execution;
  object["stopped"] = util::JsonValue(exec.stopped);
  object["reason"] = util::JsonValue(exec.stop_reason);
  object["threadId"] = util::JsonValue(static_cast<std::int64_t>(exec.thread_id));
  util::JsonArray frames;
  for (const DebugStackFrameView& frame : exec.frames) {
    util::JsonObject frame_object;
    frame_object["file"] = util::JsonValue(frame.source_path.generic_string());
    frame_object["line"] = util::JsonValue(static_cast<std::int64_t>(frame.line + 1));
    frame_object["function"] = util::JsonValue(frame.display_primary);
    frames.push_back(util::JsonValue(std::move(frame_object)));
  }
  if (exec.stopped && exec.focused_frame_index < exec.frames.size()) {
    const DebugStackFrameView& top = exec.frames[exec.focused_frame_index];
    object["file"] = util::JsonValue(top.source_path.generic_string());
    object["line"] = util::JsonValue(static_cast<std::int64_t>(top.line + 1));
  }
  object["frames"] = util::JsonValue(std::move(frames));
  return util::JsonValue(std::move(object));
}

util::JsonValue ControlChannelService::BuildBreakpoints() const {
  util::JsonArray files;
  if (context_ == nullptr) {
    return util::JsonValue(std::move(files));
  }
  const std::vector<editor::BreakpointStore::FileBreakpoints> snapshot =
      context_->current_project_state.breakpoint_store.SnapshotAll();
  for (const editor::BreakpointStore::FileBreakpoints& file : snapshot) {
    util::JsonObject file_object;
    file_object["file"] = util::JsonValue(file.path.generic_string());
    util::JsonArray breakpoints;
    for (const editor::Breakpoint& breakpoint : file.breakpoints) {
      util::JsonObject breakpoint_object;
      breakpoint_object["line"] = util::JsonValue(static_cast<std::int64_t>(breakpoint.line + 1));
      breakpoint_object["enabled"] = util::JsonValue(breakpoint.enabled);
      breakpoint_object["verified"] = util::JsonValue(breakpoint.verified);
      if (breakpoint.condition) {
        breakpoint_object["condition"] = util::JsonValue(*breakpoint.condition);
      }
      if (breakpoint.hit_condition) {
        breakpoint_object["hitCondition"] = util::JsonValue(*breakpoint.hit_condition);
      }
      if (breakpoint.log_message) {
        breakpoint_object["logMessage"] = util::JsonValue(*breakpoint.log_message);
      }
      breakpoints.push_back(util::JsonValue(std::move(breakpoint_object)));
    }
    file_object["breakpoints"] = util::JsonValue(std::move(breakpoints));
    files.push_back(util::JsonValue(std::move(file_object)));
  }
  return util::JsonValue(std::move(files));
}

util::JsonValue ControlChannelService::BuildFunctionBreakpoints() const {
  util::JsonArray breakpoints;
  if (context_ == nullptr) {
    return util::JsonValue(std::move(breakpoints));
  }
  for (const editor::FunctionBreakpoint& fn :
       context_->current_project_state.function_breakpoint_store.All()) {
    util::JsonObject object;
    object["name"] = util::JsonValue(fn.name);
    object["enabled"] = util::JsonValue(fn.enabled);
    object["verified"] = util::JsonValue(fn.verified);
    if (fn.condition) {
      object["condition"] = util::JsonValue(*fn.condition);
    }
    if (fn.hit_condition) {
      object["hitCondition"] = util::JsonValue(*fn.hit_condition);
    }
    if (!fn.verify_message.empty()) {
      object["message"] = util::JsonValue(fn.verify_message);
    }
    breakpoints.push_back(util::JsonValue(std::move(object)));
  }
  return util::JsonValue(std::move(breakpoints));
}

util::JsonValue ControlChannelService::BuildExceptionFilters() const {
  util::JsonArray filters;
  if (context_ == nullptr) {
    return util::JsonValue(std::move(filters));
  }
  const DebugBreakpointsModel& panel = context_->current_project_state.debug_breakpoints_panel;
  const std::map<std::string, std::string>& conditions = panel.FilterConditions();
  for (const dap_protocol::DapExceptionFilter& filter : panel.AdvertisedFilters()) {
    util::JsonObject object;
    object["id"] = util::JsonValue(filter.filter);
    object["label"] = util::JsonValue(filter.label);
    object["enabled"] = util::JsonValue(panel.IsEnabled(filter.filter));
    object["supportsCondition"] = util::JsonValue(filter.supports_condition);
    if (const auto it = conditions.find(filter.filter);
        it != conditions.end() && !it->second.empty()) {
      object["condition"] = util::JsonValue(it->second);
    }
    filters.push_back(util::JsonValue(std::move(object)));
  }
  return util::JsonValue(std::move(filters));
}

util::JsonValue ControlChannelService::BuildTabs() const {
  util::JsonArray tabs;
  if (context_ == nullptr) {
    return util::JsonValue(std::move(tabs));
  }
  const ProjectWorkspaceState& state = context_->current_project_state;
  for (std::size_t i = 0; i < state.focused_group().open_tabs.size(); ++i) {
    const TabEntry& tab = state.focused_group().open_tabs[i];
    util::JsonObject tab_object;
    tab_object["index"] = util::JsonValue(static_cast<std::int64_t>(i));
    const char* kind = tab.kind == TabEntry::Kind::Editor    ? "editor"
                       : tab.kind == TabEntry::Kind::Compare ? "compare"
                                                             : "merge";
    tab_object["kind"] = util::JsonValue(std::string(kind));
    tab_object["path"] = util::JsonValue(tab.path.generic_string());
    tab_object["title"] = util::JsonValue(tab.title);
    tab_object["active"] = util::JsonValue(i == state.focused_group().active_tab_index);
    tabs.push_back(util::JsonValue(std::move(tab_object)));
  }
  return util::JsonValue(std::move(tabs));
}

util::JsonValue ControlChannelService::BuildProjects() const {
  util::JsonArray projects;
  if (context_ == nullptr) {
    return util::JsonValue(std::move(projects));
  }
  const ProjectCatalogState& catalog = context_->project_catalog;
  for (std::size_t i = 0; i < catalog.entries.size(); ++i) {
    const ProjectWorkspaceState* entry = catalog.entries[i].get();
    util::JsonObject project_object;
    project_object["index"] = util::JsonValue(static_cast<std::int64_t>(i));
    project_object["root"] =
        util::JsonValue(entry != nullptr ? entry->root.generic_string() : std::string());
    project_object["active"] = util::JsonValue(i == catalog.active_index);
    projects.push_back(util::JsonValue(std::move(project_object)));
  }
  return util::JsonValue(std::move(projects));
}

util::JsonValue ControlChannelService::BuildLaunchConfigs() const {
  util::JsonArray configs;
  if (context_ == nullptr) {
    return util::JsonValue(std::move(configs));
  }
  const ProjectWorkspaceState& state = context_->current_project_state;
  for (std::size_t i = 0; i < state.launch_configs.size(); ++i) {
    const LaunchConfig& config = state.launch_configs[i];
    util::JsonObject object;
    object["name"] = util::JsonValue(config.name);
    object["type"] = util::JsonValue(config.type);
    object["request"] = util::JsonValue(config.request);
    object["selected"] = util::JsonValue(i == state.selected_launch_config_index);
    // Surface the launch/attach body (program, args, cwd, stopOnEntry, ...) so a
    // headless driver can see exactly what each config will run.
    object["arguments"] = config.arguments;
    configs.push_back(util::JsonValue(std::move(object)));
  }
  return util::JsonValue(std::move(configs));
}

util::JsonValue ControlChannelService::BuildAdapters() const {
  util::JsonArray adapters;
  if (operations_.adapters) {
    for (const ControlAdapterInfo& adapter : operations_.adapters()) {
      util::JsonObject object;
      object["type"] = util::JsonValue(adapter.type);
      util::JsonArray command;
      for (const std::string& arg : adapter.command) {
        command.push_back(util::JsonValue(arg));
      }
      object["command"] = util::JsonValue(std::move(command));
      adapters.push_back(util::JsonValue(std::move(object)));
    }
  }
  return util::JsonValue(std::move(adapters));
}

util::JsonValue ControlChannelService::BuildStatus() const {
  util::JsonObject object;
  if (context_ == nullptr) {
    return util::JsonValue(std::move(object));
  }
  const ProjectWorkspaceState& state = context_->current_project_state;
  object["projectRoot"] = util::JsonValue(state.root.generic_string());
  object["tabCount"] = util::JsonValue(static_cast<std::int64_t>(state.focused_group().open_tabs.size()));
  object["debugStopped"] = util::JsonValue(state.debug_execution.stopped);
  object["connections"] = util::JsonValue(static_cast<std::int64_t>(server_.ConnectionCount()));
  object["renderer"] = util::JsonValue(context_->render_driver_name);
  object["rendererIsGpu"] = util::JsonValue(context_->render_is_gpu);
  return util::JsonValue(std::move(object));
}

void ControlChannelService::OnDebugStopBegan(const std::string& reason, int thread_id) {
  if (context_ == nullptr || (!server_.IsRunning() && !stdout_mirror_)) {
    return;
  }
  // The immediate phase carries only what the DAP `stopped` event already knows;
  // file/line/frames follow in OnDebugStopped once the stack resolves.
  util::JsonObject event;
  event["event"] = util::JsonValue(std::string("stopped"));
  event["reason"] = util::JsonValue(reason);
  event["threadId"] = util::JsonValue(static_cast<std::int64_t>(thread_id));
  event["framesPending"] = util::JsonValue(true);
  EmitEvent(util::JsonValue(std::move(event)));
}

void ControlChannelService::OnDebugStopped() {
  if (context_ == nullptr || (!server_.IsRunning() && !stdout_mirror_)) {
    return;
  }
  const DebugExecutionView& exec = context_->current_project_state.debug_execution;
  util::JsonObject event;
  event["event"] = util::JsonValue(std::string("stopped"));
  event["reason"] = util::JsonValue(exec.stop_reason);
  event["threadId"] = util::JsonValue(static_cast<std::int64_t>(exec.thread_id));
  if (exec.stopped && exec.focused_frame_index < exec.frames.size()) {
    const DebugStackFrameView& top = exec.frames[exec.focused_frame_index];
    event["file"] = util::JsonValue(top.source_path.generic_string());
    event["line"] = util::JsonValue(static_cast<std::int64_t>(top.line + 1));
  }
  util::JsonArray frames;
  for (const DebugStackFrameView& frame : exec.frames) {
    util::JsonObject frame_object;
    frame_object["file"] = util::JsonValue(frame.source_path.generic_string());
    frame_object["line"] = util::JsonValue(static_cast<std::int64_t>(frame.line + 1));
    frame_object["function"] = util::JsonValue(frame.display_primary);
    frames.push_back(util::JsonValue(std::move(frame_object)));
  }
  event["frames"] = util::JsonValue(std::move(frames));
  event["framesPending"] = util::JsonValue(false);
  EmitEvent(util::JsonValue(std::move(event)));
}

void ControlChannelService::OnDebugTerminated(int session_id, const std::string& reason) {
  if (!server_.IsRunning() && !stdout_mirror_) {
    return;
  }
  util::JsonObject event;
  event["event"] = util::JsonValue(std::string("terminated"));
  event["sessionId"] = util::JsonValue(static_cast<std::int64_t>(session_id));
  if (!reason.empty()) {
    event["reason"] = util::JsonValue(reason);
  }
  EmitEvent(util::JsonValue(std::move(event)));
}

void ControlChannelService::OnDebugOutput(const std::string& category, const std::string& text) {
  if (!server_.IsRunning() && !stdout_mirror_) {
    return;
  }
  util::JsonObject event;
  event["event"] = util::JsonValue(std::string("output"));
  event["category"] = util::JsonValue(category);
  event["text"] = util::JsonValue(text);
  EmitEvent(util::JsonValue(std::move(event)));
}

}  // namespace microide::workspace
