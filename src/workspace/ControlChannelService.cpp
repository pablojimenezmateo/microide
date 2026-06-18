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

std::string ControlListInstancesText() {
  const std::filesystem::path dir = RuntimeBaseDir() / "instances";
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    return {};
  }
  std::string text;
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
    // Drop (and prune) descriptors whose process is gone, e.g. after a crash or
    // SIGKILL that skipped the graceful teardown.
    const std::optional<util::JsonValue> parsed = util::ParseJson(contents);
    const int pid =
        parsed.has_value() && parsed->IsObject() ? static_cast<int>((*parsed)["pid"].AsInt()) : 0;
    if (!ProcessIsAlive(pid)) {
      std::error_code remove_ec;
      std::filesystem::remove(entry.path(), remove_ec);
      continue;
    }
    text += contents;
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
  std::error_code ec;
  std::filesystem::create_directories(descriptor_path_.parent_path(), ec);
  util::JsonObject descriptor;
  descriptor["pid"] = util::JsonValue(static_cast<std::int64_t>(pid));
  descriptor["socket"] = util::JsonValue(socket_path.generic_string());
  descriptor["project_root"] = util::JsonValue(project_root.generic_string());
  descriptor["project_hash"] =
      util::JsonValue(project_root.empty() ? std::string()
                                           : ProjectStateDirectoryName(project_root));
  std::ofstream out(descriptor_path_, std::ios::trunc);
  if (out) {
    out << util::SerializeJson(util::JsonValue(std::move(descriptor)));
  }

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
  const std::vector<platform::ControlInboundMessage> messages = server_.TakeInbound();
  for (const platform::ControlInboundMessage& message : messages) {
    const ControlRequest request = ParseControlRequest(message.line);
    ControlResponse response;
    response.id = request.id;
    if (!request.valid) {
      response.ok = false;
      response.error = request.parse_error;
    } else if (request.is_command()) {
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

util::JsonValue ControlChannelService::BuildTabs() const {
  util::JsonArray tabs;
  if (context_ == nullptr) {
    return util::JsonValue(std::move(tabs));
  }
  const ProjectWorkspaceState& state = context_->current_project_state;
  for (std::size_t i = 0; i < state.open_tabs.size(); ++i) {
    const TabEntry& tab = state.open_tabs[i];
    util::JsonObject tab_object;
    tab_object["index"] = util::JsonValue(static_cast<std::int64_t>(i));
    const char* kind = tab.kind == TabEntry::Kind::Editor    ? "editor"
                       : tab.kind == TabEntry::Kind::Compare ? "compare"
                                                             : "merge";
    tab_object["kind"] = util::JsonValue(std::string(kind));
    tab_object["path"] = util::JsonValue(tab.path.generic_string());
    tab_object["title"] = util::JsonValue(tab.title);
    tab_object["active"] = util::JsonValue(i == state.active_tab_index);
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
    configs.push_back(util::JsonValue(std::move(object)));
  }
  return util::JsonValue(std::move(configs));
}

util::JsonValue ControlChannelService::BuildAdapters() const {
  util::JsonArray adapters;
  if (operations_.adapter_types) {
    for (const std::string& type : operations_.adapter_types()) {
      adapters.push_back(util::JsonValue(type));
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
  object["tabCount"] = util::JsonValue(static_cast<std::int64_t>(state.open_tabs.size()));
  object["debugStopped"] = util::JsonValue(state.debug_execution.stopped);
  object["connections"] = util::JsonValue(static_cast<std::int64_t>(server_.ConnectionCount()));
  return util::JsonValue(std::move(object));
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
  EmitEvent(util::JsonValue(std::move(event)));
}

void ControlChannelService::OnDebugTerminated(int session_id) {
  if (!server_.IsRunning() && !stdout_mirror_) {
    return;
  }
  util::JsonObject event;
  event["event"] = util::JsonValue(std::string("terminated"));
  event["sessionId"] = util::JsonValue(static_cast<std::int64_t>(session_id));
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
