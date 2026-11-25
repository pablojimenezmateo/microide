#include "workspace/control/ControlChannelService.h"

#include "workspace/services/TabStripService.h"

#include "workspace/WorkspaceOutputChannels.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <system_error>

#include <csignal>
#include <cerrno>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "platform/AppDirectories.h"
#include "platform/RuntimePaths.h"
#include "util/CoverageProfile.h"
#include "util/CrashPointControl.h"
#include "util/HeapUsage.h"
#include "util/Parse.h"
#include "util/Sha256.h"
#include "util/StringUtil.h"
#include "workspace/control/ControlCompareQuery.h"
#include "workspace/control/ControlInvariantsQuery.h"
#include "workspace/control/ControlProtocol.h"
#include "workspace/WorkspaceTerminalSelection.h"
#include "workspace/registries/WorkspaceCommandRegistry.h"
#include "workspace/WorkspaceProjectPresentation.h"
#include "workspace/debug/WorkspaceDapManager.h"
#include "workspace/debug/DebugViewModel.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::workspace {

namespace {

constexpr std::uintmax_t kMaxDescriptorFileBytes = 1u << 20;
constexpr std::size_t kMaxControlInstances = 4096;
constexpr std::size_t kMaxControlInstanceScan = 65536;

int CurrentProcessId() {
  return static_cast<int>(::getpid());
}

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

bool CommandTouchesDebugger(const std::string& command) {
  std::size_t start = command.find_first_not_of(" \t");
  if (start == std::string::npos) {
    return false;
  }
  const std::string_view verb(command.data() + start, command.size() - start);
  if (verb.starts_with("debug-toggle-enabled")) {
    return false;
  }
  return verb.starts_with("breakpoint-") || verb.starts_with("debug-");
}

std::filesystem::path DescriptorPathForPid(int pid) {
  return RuntimeBaseDir() / "instances" / (std::to_string(pid) + ".json");
}

bool ProcessIsAlive(int pid) {
  if (pid <= 0) {
    return false;
  }
  if (::kill(static_cast<pid_t>(pid), 0) == 0) {
    return true;
  }
  return errno == EPERM;
}

}

std::vector<ControlInstanceDescriptor> EnumerateControlInstances() {
  std::vector<ControlInstanceDescriptor> instances;
  const std::filesystem::path dir = RuntimeBaseDir() / "instances";
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    return instances;
  }
  std::size_t scanned = 0;
  std::filesystem::directory_iterator it(dir, ec);
  const std::filesystem::directory_iterator end;
  for (; !ec && it != end; it.increment(ec)) {
    const std::filesystem::directory_entry& entry = *it;
    if (++scanned > kMaxControlInstanceScan || instances.size() >= kMaxControlInstances) {
      break;
    }
    if (entry.path().extension() != ".json") {
      continue;
    }
    const std::optional<int> filename_pid = util::ParseInt(entry.path().stem().string());
    if (!filename_pid.has_value() || *filename_pid <= 0) {
      continue;
    }
    std::error_code size_ec;
    const std::uintmax_t file_bytes = entry.file_size(size_ec);
    if (size_ec || file_bytes > kMaxDescriptorFileBytes) {
      continue;
    }
    std::ifstream in(entry.path(), std::ios::binary);
    if (!in) {
      continue;
    }
    const std::size_t read_size = static_cast<std::size_t>(file_bytes);
    std::string contents(read_size, '\0');
    in.read(contents.data(), static_cast<std::streamsize>(read_size));
    contents.resize(static_cast<std::size_t>(in.gcount()));
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
    if (pid != *filename_pid) {
      continue;
    }
    if (!ProcessIsAlive(pid)) {
      std::error_code remove_ec;
      std::filesystem::remove(entry.path(), remove_ec);
      continue;
    }
    ControlInstanceDescriptor descriptor;
    descriptor.pid = pid;
    descriptor.socket = SocketPathForPid(pid);
    descriptor.project_root = (*parsed)["project_root"].AsString();
    descriptor.project_hash = (*parsed)["project_hash"].AsString();
    util::JsonObject canonical;
    canonical["pid"] = util::JsonValue(static_cast<std::int64_t>(descriptor.pid));
    canonical["socket"] = util::JsonValue(descriptor.socket.generic_string());
    canonical["project_root"] = util::JsonValue(descriptor.project_root);
    canonical["project_hash"] = util::JsonValue(descriptor.project_hash);
    descriptor.raw_json = util::SerializeJson(util::JsonValue(std::move(canonical)));
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

void ControlChannelService::SetWakeChannel(std::uint32_t event_type) {
  wake_event_type_ = event_type;
  server_.SetWakeChannel(event_type);
}

bool ControlChannelService::Start(const std::filesystem::path& project_root) {
  if (server_.IsRunning()) {
    return true;
  }
  const int pid = CurrentProcessId();
  if (!platform::EnsureSecurePrivateDirectory(RuntimeBaseDir())) {
    return false;
  }
  const std::filesystem::path socket_path = SocketPathForPid(pid);
  if (!server_.Start(socket_path)) {
    return false;
  }
  server_.SetWakeChannel(wake_event_type_);

  descriptor_path_ = DescriptorPathForPid(pid);
  project_root_ = project_root;
  WriteDescriptor();

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
  const std::filesystem::path parent = descriptor_path_.parent_path();
  std::filesystem::create_directories(parent, ec);
  ::chmod(parent.c_str(), S_IRWXU);
  if (parent.has_parent_path()) {
    ::chmod(parent.parent_path().c_str(), S_IRWXU);
  }
  util::JsonObject descriptor;
  descriptor["pid"] = util::JsonValue(static_cast<std::int64_t>(pid));
  descriptor["socket"] = util::JsonValue(SocketPathForPid(pid).generic_string());
  descriptor["project_root"] = util::JsonValue(project_root_.generic_string());
  descriptor["project_hash"] =
      util::JsonValue(project_root_.empty() ? std::string()
                                            : ProjectStateDirectoryName(project_root_));

  std::filesystem::path temp_path = descriptor_path_;
  temp_path += ".tmp";
  {
    std::ofstream out(temp_path, std::ios::trunc | std::ios::binary);
    if (!out) {
      return;
    }
    out << util::SerializeJson(util::JsonValue(std::move(descriptor)));
    out.flush();
    if (!out) {
      std::error_code remove_ec;
      std::filesystem::remove(temp_path, remove_ec);
      return;
    }
  }
  ::chmod(temp_path.c_str(), S_IRUSR | S_IWUSR);
  std::error_code rename_ec;
  std::filesystem::rename(temp_path, descriptor_path_, rename_ec);
  if (rename_ec) {
    std::error_code remove_ec;
    std::filesystem::remove(temp_path, remove_ec);
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

void ControlChannelService::ConsumeControlCallbacks() {
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
    } else if (request.query == "idle" && request.args.IsObject() && request.args.HasKey("wait") &&
               request.args["wait"].IsBool() && request.args["wait"].AsBool()) {
      idle_waiters_.emplace_back(message.connection_id, request.id);
      continue;
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
    const std::string serialized =
        SerializeControlResponseWithin(response, platform::ControlSocketServer::kMaxLineBytes);
    server_.SendLine(message.connection_id, serialized);
    EmitJsonLine(serialized);
  }
}

void ControlChannelService::ReleaseIdleWaiters() {
  if (idle_waiters_.empty()) {
    return;
  }
  util::JsonObject result;
  result["idle"] = util::JsonValue(true);
  const util::JsonValue idle(std::move(result));
  for (const auto& [connection, id] : idle_waiters_) {
    ControlResponse response;
    response.id = id;
    response.ok = true;
    response.result = idle;
    const std::string serialized = SerializeControlResponse(response);
    server_.SendLine(connection, serialized);
    EmitJsonLine(serialized);
  }
  idle_waiters_.clear();
}

util::JsonValue ControlChannelService::HandleQuery(const std::string& verb,
                                                   const util::JsonValue& args, bool* ok,
                                                   std::string* error) const {
  const std::span<const std::string_view> verbs = ControlQueryVerbs();
  if (std::find(verbs.begin(), verbs.end(), verb) == verbs.end()) {
    *ok = false;
    *error = "unknown query \"" + verb + "\"";
    return util::JsonValue(nullptr);
  }
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
  if (verb == "editor") {
    return BuildEditor();
  }
  if (verb == "documents") {
    return BuildDocuments(args);
  }
  if (verb == "surface") {
    return BuildSurface();
  }
  if (verb == "commands") {
    return BuildCommands();
  }
  if (verb == "terminals") {
    return BuildTerminals();
  }
  if (verb == "terminal-output") {
    return BuildTerminalOutput(args, ok, error);
  }
  if (verb == "compare") {
    return BuildControlCompareJson(
        context_ != nullptr ? context_->current_project_state.focused_group().active_compare() : nullptr);
  }
  if (verb == "idle") {
    util::JsonObject idle;
    idle["idle"] = util::JsonValue(false);
    return util::JsonValue(std::move(idle));
  }
  if (verb == "notifications") {
    return BuildNotifications(args.IsObject() && args["recent"].AsBool(false));
  }
  if (verb == "output") {
    return BuildOutput(args);
  }
  if (verb == "keybindings") {
    return BuildKeybindings();
  }
  if (verb == "settings") {
    return BuildSettings();
  }
  if (verb == "buffer") {
    const ControlActiveBuffer active =
        operations_.active_buffer ? operations_.active_buffer() : ControlActiveBuffer{};
    return BuildControlBufferJson(active.viewport, active.kind);
  }
  if (verb == "hit-test") {
    if (!operations_.window_hit_region || !args.IsObject() || !args.HasKey("x") ||
        !args.HasKey("y")) {
      *ok = false;
      *error = "hit-test needs {\"x\": number, \"y\": number}";
      return util::JsonValue(nullptr);
    }
    util::JsonObject result;
    result["region"] = util::JsonValue(std::string(operations_.window_hit_region(
        static_cast<float>(args["x"].AsDouble()), static_cast<float>(args["y"].AsDouble()))));
    return util::JsonValue(std::move(result));
  }
  if (verb == "ui") {
    return context_ == nullptr ? util::JsonValue(util::JsonObject{})
                               : ControlUiQuery::Build(*context_, operations_.ui);
  }
  if (verb == "persistence") {
    const ControlPersistenceState state =
        operations_.persistence ? operations_.persistence() : ControlPersistenceState{};
    util::JsonObject result;
    result["crashFlushArmed"] = util::JsonValue(state.crash_flush_armed);
    result["writesSettled"] = util::JsonValue(state.writes_settled);
    result["staged"] = util::JsonValue(!state.crash_flush_armed && state.writes_settled);
    return util::JsonValue(std::move(result));
  }
  if (verb == "action-counts") {
    return operations_.ui.action_counts ? operations_.ui.action_counts()
                                        : util::JsonValue(util::JsonObject{});
  }
  if (verb == "code-coverage") {
    util::JsonObject result;
    result["instrumented"] = util::JsonValue(util::CoverageProfileInstrumented());
    if (args.IsObject() && args.HasKey("flush") && args["flush"].AsBool(false)) {
      result["flushed"] = util::JsonValue(util::FlushCoverageProfile());
    }
    return util::JsonValue(std::move(result));
  }
  if (verb == "invariants") {
    ControlInvariantsQueryOptions options;
    options.deep = args.IsObject() && args.HasKey("deep") && args["deep"].AsBool(false);
    return BuildControlInvariantsJson(context_ != nullptr ? &context_->current_project_state : nullptr,
                                      options);
  }
  if (verb == "crash-points") {
    if (args.IsObject() && args.HasKey("arm")) {
      std::string why;
      if (!util::CrashPointControl::Arm(args["arm"].AsString(), &why)) {
        *ok = false;
        *error = "crash-points: " + why;
        return util::JsonValue(nullptr);
      }
    } else if (args.IsObject() && args.HasKey("disarm") && args["disarm"].AsBool(false)) {
      util::CrashPointControl::Disarm();
    }
    const util::CrashPointControl::Snapshot snapshot = util::CrashPointControl::Read();
    util::JsonObject result;
    result["installed"] = util::JsonValue(snapshot.installed);
    result["armed"] = util::JsonValue(snapshot.armed);
    result["armedMatches"] = util::JsonValue(static_cast<std::int64_t>(snapshot.armed_matches));
    util::JsonArray sites;
    for (const util::CrashPointControl::SitePasses& site : snapshot.sites) {
      util::JsonObject entry;
      entry["site"] = util::JsonValue(site.site);
      entry["passes"] = util::JsonValue(static_cast<std::int64_t>(site.passes));
      util::JsonObject files;
      for (const auto& [name, count] : site.files) {
        files[name] = util::JsonValue(static_cast<std::int64_t>(count));
      }
      entry["files"] = util::JsonValue(std::move(files));
      sites.push_back(util::JsonValue(std::move(entry)));
    }
    result["sites"] = util::JsonValue(std::move(sites));
    return util::JsonValue(std::move(result));
  }
  *ok = false;
  *error = "unknown query \"" + verb + "\"";
  return util::JsonValue(nullptr);
}

util::JsonValue ControlChannelService::BuildOutput(const util::JsonValue& args) const {
  util::JsonObject object;
  const WorkspaceOutputChannels* channels = operations_.output_channels;
  const std::string shown =
      context_ != nullptr ? context_->current_project_state.panel.output.channel_id : std::string{};
  const std::string wanted =
      args.IsObject() && args.HasKey("channel") ? args["channel"].AsString() : shown;
  util::JsonArray list;
  if (channels != nullptr) {
    for (const WorkspaceOutputChannels::ChannelInfo& info : channels->Channels()) {
      util::JsonObject entry;
      entry["id"] = util::JsonValue(info.id);
      entry["label"] = util::JsonValue(info.label);
      const std::vector<std::string>* lines = channels->Entries(info.id);
      entry["lines"] = util::JsonValue(static_cast<std::int64_t>(lines != nullptr ? lines->size() : 0));
      list.push_back(util::JsonValue(std::move(entry)));
    }
  }
  object["channels"] = util::JsonValue(std::move(list));
  object["shown"] = util::JsonValue(shown);
  object["channel"] = util::JsonValue(wanted);
  util::JsonArray text;
  if (const std::vector<std::string>* lines = channels != nullptr ? channels->Entries(wanted) : nullptr;
      lines != nullptr) {
    const std::size_t first = lines->size() > kMaxControlQueryEntries ? lines->size() - kMaxControlQueryEntries : 0;
    for (std::size_t i = first; i < lines->size(); ++i) {
      text.push_back(util::JsonValue((*lines)[i]));
    }
  }
  object["text"] = util::JsonValue(std::move(text));
  return util::JsonValue(std::move(object));
}

util::JsonValue ControlChannelService::BuildDebugState() const {
  util::JsonObject object;
  if (context_ == nullptr) {
    return util::JsonValue(std::move(object));
  }
  const DebugExecutionView& exec = context_->current_project_state.debug_execution;
  const auto state_name = [](DebugSession::State state) -> const char* {
    switch (state) {
      case DebugSession::State::Inactive: return "inactive";
      case DebugSession::State::Initializing: return "initializing";
      case DebugSession::State::Configuring: return "configuring";
      case DebugSession::State::Running: return "running";
      case DebugSession::State::Stopped: return "stopped";
      case DebugSession::State::Terminated: return "terminated";
      case DebugSession::State::Failed: return "failed";
    }
    return "inactive";
  };
  const char* session_state = "none";
  util::JsonArray sessions;
  if (const DapManager* manager = context_->current_project_state.dap_manager.get(); manager != nullptr) {
    if (const DebugSession* session = manager->ActiveSession(); session != nullptr) {
      session_state = state_name(session->CurrentState());
    }
    for (const DapSessionInfo& info : manager->Sessions()) {
      util::JsonObject entry;
      entry["id"] = util::JsonValue(static_cast<std::int64_t>(info.id));
      entry["label"] = util::JsonValue(info.name);
      entry["state"] = util::JsonValue(std::string(state_name(info.state)));
      entry["active"] = util::JsonValue(info.id == manager->ActiveSessionId());
      sessions.push_back(util::JsonValue(std::move(entry)));
    }
  }
  object["session"] = util::JsonValue(std::string(session_state));
  object["sessions"] = util::JsonValue(std::move(sessions));
  object["stopped"] = util::JsonValue(exec.stopped);
  object["reason"] = util::JsonValue(exec.stop_reason);
  object["threadId"] = util::JsonValue(static_cast<std::int64_t>(exec.thread_id));
  util::JsonArray frames;
  for (const DebugStackFrameView& frame : exec.frames) {
    if (frames.size() >= kMaxControlQueryEntries) {
      object["framesTruncated"] = util::JsonValue(true);
      break;
    }
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
  std::size_t emitted = 0;
  for (const editor::BreakpointStore::FileBreakpoints& file : snapshot) {
    if (emitted >= kMaxControlQueryEntries) {
      break;
    }
    util::JsonObject file_object;
    file_object["file"] = util::JsonValue(file.path.generic_string());
    util::JsonArray breakpoints;
    for (const editor::Breakpoint& breakpoint : file.breakpoints) {
      if (emitted >= kMaxControlQueryEntries) {
        break;
      }
      ++emitted;
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
    if (breakpoints.size() >= kMaxControlQueryEntries) {
      break;
    }
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
  const std::size_t focused_group_index = state.clamped_focused_group_index();
  for (std::size_t g = 0; g < state.editor_groups.size(); ++g) {
    const EditorGroup& group = state.editor_groups[g];
    for (std::size_t i = 0; i < group.open_tabs.size(); ++i) {
      if (tabs.size() >= kMaxControlQueryEntries) {
        return util::JsonValue(std::move(tabs));
      }
      const TabEntry& tab = group.open_tabs[i];
      util::JsonObject tab_object;
      tab_object["group"] = util::JsonValue(static_cast<std::int64_t>(g));
      tab_object["index"] = util::JsonValue(static_cast<std::int64_t>(i));
      const char* kind = tab.kind == TabEntry::Kind::Editor    ? "editor"
                         : tab.kind == TabEntry::Kind::Compare ? "compare"
                                                               : "merge";
      tab_object["kind"] = util::JsonValue(std::string(kind));
      tab_object["path"] = util::JsonValue(tab.path.generic_string());
      tab_object["title"] = util::JsonValue(tab.title);
      tab_object["active"] = util::JsonValue(g == focused_group_index &&
                                             i == group.active_tab_index);
      tabs.push_back(util::JsonValue(std::move(tab_object)));
    }
  }
  return util::JsonValue(std::move(tabs));
}

namespace {

util::JsonValue BuildEditorTabJson(std::size_t index, const TabEntry& tab, bool active,
                                   bool is_active_in_focus) {
  util::JsonObject object;
  object["index"] = util::JsonValue(static_cast<std::int64_t>(index));
  const char* kind = tab.kind == TabEntry::Kind::Editor    ? "editor"
                     : tab.kind == TabEntry::Kind::Compare ? "compare"
                                                           : "merge";
  object["kind"] = util::JsonValue(std::string(kind));
  object["path"] = util::JsonValue(tab.path.generic_string());
  object["title"] = util::JsonValue(tab.title);
  object["active"] = util::JsonValue(active);
  object["dirty"] = util::JsonValue(TabEntryIsDirty(tab));
  if (tab.kind == TabEntry::Kind::Compare && tab.compare.has_value()) {
    const CompareTabState& compare = *tab.compare;
    object["loading"] = util::JsonValue(compare.load_pending);
    object["changes"] = util::JsonValue(static_cast<std::int64_t>(compare.model.hunks.size()));
    object["ignoreWhitespace"] = util::JsonValue(compare.build_options.ignore_whitespace);
    object["showWhitespace"] = util::JsonValue(compare.show_whitespace);
    std::int64_t moved_left = 0;
    std::int64_t moved_right = 0;
    for (const compare::CompareRow& row : compare.model.rows) {
      moved_left += row.left_moved ? 1 : 0;
      moved_right += row.right_moved ? 1 : 0;
    }
    object["movedLeftLines"] = util::JsonValue(moved_left);
    object["movedRightLines"] = util::JsonValue(moved_right);
    object["leftLabel"] = util::JsonValue(compare.left_label);
    object["rightLabel"] = util::JsonValue(compare.right_label);
    if (compare.review_markers_valid) {
      std::int64_t reviewed = 0;
      std::int64_t noted = 0;
      for (const compare::BranchReviewHunkMarker& marker : compare.review_hunk_markers) {
        reviewed += marker.status == compare::BranchReviewMarkerStatus::Reviewed ? 1 : 0;
        noted += marker.has_note ? 1 : 0;
      }
      object["reviewedHunks"] = util::JsonValue(reviewed);
      object["notedHunks"] = util::JsonValue(noted);
    }
    if (!compare.review_files.empty()) {
      object["reviewFiles"] = util::JsonValue(static_cast<std::int64_t>(compare.review_files.size()));
      object["reviewFileIndex"] = util::JsonValue(static_cast<std::int64_t>(compare.review_file_index) + 1);
    }
  }
  if (tab.kind == TabEntry::Kind::Merge && tab.merge.has_value()) {
    const MergeTabState& merge = *tab.merge;
    std::int64_t unresolved = 0;
    for (const MergeTrackedConflict& conflict : merge.conflicts) {
      unresolved += conflict.valid && !conflict.resolved ? 1 : 0;
    }
    object["conflicts"] = util::JsonValue(static_cast<std::int64_t>(merge.conflicts.size()));
    object["unresolved"] = util::JsonValue(unresolved);
    object["selectedConflict"] = util::JsonValue(static_cast<std::int64_t>(merge.selected_hunk) + 1);
    object["basePane"] = util::JsonValue(merge.base_pane_visible);
    object["markedResolved"] = util::JsonValue(merge.marked_resolved);
    object["status"] = util::JsonValue(merge.status_message);
  }
  if (tab.kind != TabEntry::Kind::Editor) {
    return util::JsonValue(std::move(object));
  }
  const auto one_based = [](std::size_t value) {
    return util::JsonValue(static_cast<std::int64_t>(value) + 1);
  };
  const std::optional<DeferredTabHandle> view = EditorTabViewState(tab);
  if (!view.has_value()) {
    return util::JsonValue(std::move(object));
  }
  const bool loaded = tab.editor_state.has_value() && !tab.editor_state->content_pending();
  if (tab.editor_state.has_value() && !loaded) {
    object["loading"] = util::JsonValue(true);
  }
  object["cursorLine"] = one_based(view->cursor_line);
  object["cursorColumn"] = one_based(view->cursor_column);
  object["scrollLine"] = one_based(view->scroll_line);
  util::JsonArray folded;
  for (const std::size_t opener : view->collapsed_fold_openers) {
    folded.push_back(one_based(opener));
  }
  object["foldedLines"] = util::JsonValue(std::move(folded));
  if (loaded && is_active_in_focus) {
    const editor::TextViewport& viewport = tab.editor_state->viewport;
    object["visibleTop"] = one_based(viewport.scroll_line());
    object["visibleCount"] = util::JsonValue(static_cast<std::int64_t>(viewport.visible_lines()));
  }
  return util::JsonValue(std::move(object));
}

const char* SplitOrientationName(EditorSplitOrientation orientation) {
  switch (orientation) {
    case EditorSplitOrientation::Vertical:
      return "vertical";
    case EditorSplitOrientation::Horizontal:
      return "horizontal";
    case EditorSplitOrientation::None:
      break;
  }
  return "leaf";
}

}

util::JsonValue ControlChannelService::BuildEditor() const {
  util::JsonObject root;
  if (context_ == nullptr) {
    return util::JsonValue(std::move(root));
  }
  const ProjectWorkspaceState& state = context_->current_project_state;
  const std::size_t focused = state.clamped_focused_group_index();
  root["focusedGroupIndex"] = util::JsonValue(static_cast<std::int64_t>(focused));
  util::JsonArray split;
  for (const EditorSplitNodeRecord& record : state.editor_split.Flatten()) {
    util::JsonObject node;
    node["orientation"] = util::JsonValue(std::string(SplitOrientationName(record.orientation)));
    if (!record.weights.empty()) {
      util::JsonArray weights;
      for (float weight : record.weights) {
        weights.push_back(util::JsonValue(static_cast<double>(weight)));
      }
      node["weights"] = util::JsonValue(std::move(weights));
    }
    split.push_back(util::JsonValue(std::move(node)));
  }
  root["split"] = util::JsonValue(std::move(split));

  util::JsonArray groups;
  std::size_t emitted = 0;
  for (std::size_t g = 0; g < state.editor_groups.size(); ++g) {
    const EditorGroup& group = state.editor_groups[g];
    util::JsonObject group_object;
    group_object["index"] = util::JsonValue(static_cast<std::int64_t>(g));
    const bool group_focused = g == focused;
    group_object["focused"] = util::JsonValue(group_focused);
    util::JsonArray tabs;
    for (std::size_t i = 0; i < group.open_tabs.size() && emitted < kMaxControlQueryEntries;
         ++i, ++emitted) {
      const bool active = i == group.active_tab_index;
      tabs.push_back(BuildEditorTabJson(i, group.open_tabs[i], active, group_focused && active));
    }
    group_object["tabs"] = util::JsonValue(std::move(tabs));
    groups.push_back(util::JsonValue(std::move(group_object)));
  }
  root["groups"] = util::JsonValue(std::move(groups));
  return util::JsonValue(std::move(root));
}

util::JsonValue ControlChannelService::BuildDocuments(const util::JsonValue& args) const {
  const std::string content_path =
      args.IsObject() && args.HasKey("content") ? args["content"].AsString() : std::string{};
  util::JsonArray documents;
  if (context_ == nullptr) {
    return util::JsonValue(std::move(documents));
  }
  const bool all_projects = args.IsObject() && args.HasKey("all") && args["all"].AsBool();
  std::vector<std::pair<const ProjectWorkspaceState*, bool>> projects{{&context_->current_project_state, true}};
  if (all_projects) {
    const ProjectCatalogState& catalog = context_->project_catalog;
    for (std::size_t i = 0; i < catalog.entries.size(); ++i) {
      if (i != catalog.active_index && catalog.entries[i] != nullptr) {
        projects.emplace_back(catalog.entries[i].get(), false);
      }
    }
  }
  std::vector<const void*> seen;
  for (const auto& [project, active] : projects) {
    for (const TabEntry::Kind surface : {TabEntry::Kind::Editor, TabEntry::Kind::Compare, TabEntry::Kind::Merge}) {
    for (const EditorGroup& group : project->editor_groups) {
      for (const TabEntry& tab : group.open_tabs) {
        const editor::TextViewport* loaded =
            tab.kind != surface                 ? nullptr
            : surface != TabEntry::Kind::Merge ? LoadedBufferViewOf(tab)
            : tab.merge.has_value() && tab.merge->result_viewport.dirty() ? &tab.merge->result_viewport
                                                                           : nullptr;
        if (loaded == nullptr) {
          continue;
        }
        const editor::TextViewport& view = *loaded;
        if (std::find(seen.begin(), seen.end(), view.document_identity()) != seen.end()) {
          continue;
        }
        seen.push_back(view.document_identity());
        const bool opaque = view.encoding() == editor::TextViewport::TextEncoding::Bytes;
        const std::string bytes = view.SerializeDocumentText();
        util::JsonObject object;
        object["path"] = util::JsonValue(view.path().generic_string());
        object["title"] = util::JsonValue(tab.title);
        object["surface"] = util::JsonValue(std::string(surface == TabEntry::Kind::Editor    ? "editor"
                                                        : surface == TabEntry::Kind::Compare ? "compare"
                                                                                             : "merge"));
        object["group"] = util::JsonValue(static_cast<std::int64_t>(&group - project->editor_groups.data()));
        object["tab"] = util::JsonValue(static_cast<std::int64_t>(&tab - group.open_tabs.data()));
        object["untitledNumber"] = util::JsonValue(static_cast<std::int64_t>(tab.untitled_number));
        object["dirty"] = util::JsonValue(view.dirty());
        object["readOnly"] = util::JsonValue(view.read_only());
        object["encoding"] = util::JsonValue(std::string(
            opaque ? "bytes" : view.encoding() == editor::TextViewport::TextEncoding::ASCII ? "ascii" : "utf8"));
        object["mixedLineEndings"] = util::JsonValue(view.has_mixed_line_endings());
        object["bytes"] = util::JsonValue(static_cast<std::int64_t>(bytes.size()));
        object["sha256"] = util::JsonValue(util::Sha256Hex(bytes));
        if (all_projects) {
          object["project"] = util::JsonValue(project->root.generic_string());
          object["active"] = util::JsonValue(active);
        }
        if (!content_path.empty() && view.path().generic_string() == content_path) {
          static constexpr char kHex[] = "0123456789abcdef";
          std::string hex;
          hex.reserve(bytes.size() * 2);
          for (const unsigned char byte : bytes) {
            hex.push_back(kHex[byte >> 4]);
            hex.push_back(kHex[byte & 0x0f]);
          }
          object["contentHex"] = util::JsonValue(std::move(hex));
        }
        documents.push_back(util::JsonValue(std::move(object)));
      }
    }
    }
  }
  return util::JsonValue(std::move(documents));
}

util::JsonValue ControlChannelService::BuildCommands() const {
  util::JsonArray commands;
  for (const ActionSpec& spec : WorkspaceCommandSpecs()) {
    if (spec.command_name.empty()) {
      continue;
    }
    util::JsonObject object;
    object["command"] = util::JsonValue(std::string(spec.command_name));
    object["usage"] = util::JsonValue(std::string(spec.command_usage));
    object["label"] = util::JsonValue(std::string(spec.label));
    commands.push_back(util::JsonValue(std::move(object)));
  }
  if (operations_.plugin_command_names) {
    for (std::string& name : operations_.plugin_command_names()) {
      util::JsonObject object;
      object["command"] = util::JsonValue(name);
      object["usage"] = util::JsonValue(name);
      object["label"] = util::JsonValue(name);
      object["plugin"] = util::JsonValue(true);
      commands.push_back(util::JsonValue(std::move(object)));
    }
  }
  return util::JsonValue(std::move(commands));
}

util::JsonValue ControlChannelService::BuildSettings() const {
  util::JsonArray rows;
  if (!operations_.settings) {
    return util::JsonValue(std::move(rows));
  }
  const auto type_name = [](SettingType type) {
    switch (type) {
      case SettingType::Bool: return "bool";
      case SettingType::Int: return "int";
      case SettingType::Float: return "float";
      case SettingType::Enum: return "enum";
      case SettingType::String: break;
    }
    return "string";
  };
  for (const SettingInfo& info : operations_.settings()) {
    util::JsonObject row;
    row["id"] = util::JsonValue(info.id);
    row["label"] = util::JsonValue(info.label);
    row["description"] = util::JsonValue(info.description);
    row["type"] = util::JsonValue(std::string(type_name(info.type)));
    row["scope"] = util::JsonValue(std::string(info.scope == SettingScope::User ? "user" : "project"));
    if (!info.plugin_id.empty()) {
      row["plugin"] = util::JsonValue(info.plugin_id);
    }
    row["group"] = util::JsonValue(info.group);
    row["default"] = util::JsonValue(SerializeSettingValue(info.default_value));
    const std::optional<std::string> value =
        operations_.setting_value ? operations_.setting_value(info.id) : std::nullopt;
    row["value"] = value.has_value() ? util::JsonValue(*value) : util::JsonValue(nullptr);
    if (info.type == SettingType::Enum) {
      util::JsonArray values;
      for (const std::string& enum_value : info.enum_values) {
        values.push_back(util::JsonValue(enum_value));
      }
      row["enumValues"] = util::JsonValue(std::move(values));
    }
    if (const SettingSpec* spec = FindBuiltinSettingSpec(info.id); spec != nullptr) {
      if (spec->type == SettingType::Int) {
        if (spec->min_int != std::numeric_limits<int>::min()) {
          row["min"] = util::JsonValue(static_cast<std::int64_t>(spec->min_int));
        }
        if (spec->max_int != std::numeric_limits<int>::max()) {
          row["max"] = util::JsonValue(static_cast<std::int64_t>(spec->max_int));
        }
        row["step"] = util::JsonValue(static_cast<std::int64_t>(spec->int_step));
      } else if (spec->type == SettingType::Float) {
        if (spec->min_float != std::numeric_limits<float>::lowest()) {
          row["min"] = util::JsonValue(static_cast<double>(spec->min_float));
        }
        if (spec->max_float != std::numeric_limits<float>::max()) {
          row["max"] = util::JsonValue(static_cast<double>(spec->max_float));
        }
      }
      if (spec->suggests_fonts) {
        row["suggestsFonts"] = util::JsonValue(true);
      }
    }
    rows.push_back(util::JsonValue(std::move(row)));
    if (rows.size() >= kMaxControlQueryEntries) {
      break;
    }
  }
  return util::JsonValue(std::move(rows));
}

util::JsonValue ControlChannelService::BuildKeybindings() const {
  util::JsonArray rows;
  if (!operations_.keybindings) {
    return util::JsonValue(std::move(rows));
  }
  const auto context_name = [](KeybindingContext context) {
    switch (context) {
      case KeybindingContext::Editor: return "editor";
      case KeybindingContext::Sidebar: return "sidebar";
      case KeybindingContext::Terminal: return "terminal";
      case KeybindingContext::Global: break;
    }
    return "global";
  };
  for (const ResolvedKeybinding& binding : operations_.keybindings()) {
    if (rows.size() >= kMaxControlQueryEntries) {
      break;
    }
    util::JsonObject object;
    object["id"] = util::JsonValue(binding.id);
    object["chord"] = util::JsonValue(FormatBindingKeys(binding));
    object["context"] = util::JsonValue(context_name(binding.context));
    object["skipsTerminal"] = util::JsonValue(binding.skips_terminal);
    object["plugin"] = util::JsonValue(binding.from_plugin);
    object["sharesChord"] = util::JsonValue(binding.shares_chord);
    object["enabled"] = util::JsonValue(!binding.command_name.empty() || !operations_.action_enabled ||
                                        operations_.action_enabled(binding.action));
    std::string command = binding.command_name;
    if (command.empty()) {
      for (const ActionSpec& spec : WorkspaceCommandSpecs()) {
        if (spec.id == binding.action) {
          command = std::string(spec.command_name.empty() ? spec.label : spec.command_name);
          break;
        }
      }
    }
    object["command"] = util::JsonValue(std::move(command));
    rows.push_back(util::JsonValue(std::move(object)));
  }
  return util::JsonValue(std::move(rows));
}

util::JsonValue ControlChannelService::BuildNotifications(bool recent) const {
  util::JsonArray rows;
  if (operations_.notifications == nullptr) {
    return util::JsonValue(std::move(rows));
  }
  const auto tone_name = [](NotificationService::Tone tone) {
    switch (tone) {
      case NotificationService::Tone::Error: return "error";
      case NotificationService::Tone::Warning: return "warning";
      case NotificationService::Tone::Info: break;
    }
    return "info";
  };
  std::vector<std::pair<const NotificationService::Notification*, std::uint64_t>> listed;
  if (recent) {
    for (const NotificationService::Posted& posted : operations_.notifications->Recent()) {
      listed.emplace_back(&posted.row, posted.seq);
    }
  } else {
    for (const NotificationService::Notification& row : operations_.notifications->Active()) {
      listed.emplace_back(&row, 0);
    }
  }
  for (std::size_t i = 0; i < listed.size(); ++i) {
    const NotificationService::Notification& row = *listed[i].first;
    util::JsonObject object;
    object["index"] = util::JsonValue(static_cast<std::int64_t>(i));
    if (recent) {
      object["seq"] = util::JsonValue(static_cast<std::int64_t>(listed[i].second));
    }
    object["key"] = util::JsonValue(row.key);
    object["tone"] = util::JsonValue(tone_name(row.tone));
    object["message"] = util::JsonValue(row.message);
    object["sticky"] = util::JsonValue(row.sticky);
    if (row.progress.has_value()) {
      object["progress"] = util::JsonValue(static_cast<double>(*row.progress));
    }
    util::JsonArray actions;
    for (std::size_t a = 0; a < row.actions.size(); ++a) {
      util::JsonObject action;
      action["index"] = util::JsonValue(static_cast<std::int64_t>(a));
      action["label"] = util::JsonValue(row.actions[a].label);
      const ActionSpec* spec = FindWorkspaceActionSpec(row.actions[a].id);
      action["command"] =
          util::JsonValue(spec != nullptr ? std::string(spec->command_name) : std::string());
      util::JsonArray args;
      for (const std::string& arg : row.actions[a].args) {
        args.push_back(util::JsonValue(arg));
      }
      action["args"] = util::JsonValue(std::move(args));
      actions.push_back(util::JsonValue(std::move(action)));
    }
    object["actions"] = util::JsonValue(std::move(actions));
    rows.push_back(util::JsonValue(std::move(object)));
  }
  return util::JsonValue(std::move(rows));
}

util::JsonValue ControlChannelService::BuildTerminals() const {
  util::JsonArray tabs;
  if (context_ == nullptr) {
    return util::JsonValue(std::move(tabs));
  }
  const ProjectWorkspaceState& state = context_->current_project_state;
  for (std::size_t t = 0; t < state.terminal_tabs.size(); ++t) {
    const TerminalTabState* tab = state.terminal_tabs[t].get();
    if (tab == nullptr) {
      continue;
    }
    util::JsonObject tab_object;
    tab_object["tab"] = util::JsonValue(static_cast<std::int64_t>(t));
    tab_object["active"] = util::JsonValue(t == state.active_terminal_tab_index);
    tab_object["unseenOutput"] = util::JsonValue(tab->has_unseen_output);
    tab_object["unseenBell"] = util::JsonValue(tab->has_unseen_bell);
    util::JsonArray panes;
    for (std::size_t p = 0; p < tab->panes.size(); ++p) {
      const TerminalPaneState* pane = tab->panes[p].get();
      if (pane == nullptr) {
        continue;
      }
      util::JsonObject pane_object;
      pane_object["pane"] = util::JsonValue(static_cast<std::int64_t>(p));
      pane_object["active"] = util::JsonValue(p == tab->active_pane);
      pane_object["label"] = util::JsonValue(pane->session.LaunchLabel());
      pane_object["labelPrefix"] = util::JsonValue(TerminalPaneLabelPrefix(state, *pane));
      pane_object["running"] = util::JsonValue(pane->session.running());
      pane_object["pending"] = util::JsonValue(pane->session.host_pending());
      pane_object["hostHandle"] =
          util::JsonValue(static_cast<std::int64_t>(pane->session.host_identity().handle));
      pane_object["lineCount"] =
          util::JsonValue(static_cast<std::int64_t>(pane->session.LineCount()));
      panes.push_back(util::JsonValue(std::move(pane_object)));
    }
    tab_object["panes"] = util::JsonValue(std::move(panes));
    tabs.push_back(util::JsonValue(std::move(tab_object)));
  }
  return util::JsonValue(std::move(tabs));
}

util::JsonValue ControlChannelService::BuildTerminalOutput(const util::JsonValue& args, bool* ok,
                                                           std::string* error) const {
  constexpr std::int64_t kDefaultMaxLines = 1000;
  constexpr std::int64_t kMaxLines = 20000;
  const auto fail = [&](const char* message) {
    *ok = false;
    *error = message;
    return util::JsonValue(nullptr);
  };
  if (context_ == nullptr) {
    return fail("no active project");
  }
  const ProjectWorkspaceState& state = context_->current_project_state;
  if (state.terminal_tabs.empty()) {
    return fail("no terminal tabs");
  }
  const bool has_args = args.IsObject();
  const std::int64_t tab_index =
      has_args && args.HasKey("tab")
          ? args["tab"].AsInt(-1)
          : static_cast<std::int64_t>(state.active_terminal_tab_index);
  if (tab_index < 0 || static_cast<std::size_t>(tab_index) >= state.terminal_tabs.size() ||
      state.terminal_tabs[static_cast<std::size_t>(tab_index)] == nullptr) {
    return fail("terminal tab index out of range");
  }
  const TerminalTabState& tab = *state.terminal_tabs[static_cast<std::size_t>(tab_index)];
  const std::int64_t pane_index = has_args && args.HasKey("pane")
                                      ? args["pane"].AsInt(-1)
                                      : static_cast<std::int64_t>(tab.active_pane);
  if (pane_index < 0 || static_cast<std::size_t>(pane_index) >= tab.panes.size() ||
      tab.panes[static_cast<std::size_t>(pane_index)] == nullptr) {
    return fail("terminal pane index out of range");
  }
  std::int64_t max_lines = has_args && args.HasKey("lines") ? args["lines"].AsInt(kDefaultMaxLines)
                                                             : kDefaultMaxLines;
  if (max_lines <= 0) {
    max_lines = kDefaultMaxLines;
  }
  max_lines = std::min(max_lines, kMaxLines);

  const terminal::TerminalSession& session =
      tab.panes[static_cast<std::size_t>(pane_index)]->session;
  const std::size_t line_count = session.LineCount();
  const std::size_t cap = static_cast<std::size_t>(max_lines);
  const std::size_t start = line_count > cap ? line_count - cap : 0;
  const std::vector<terminal::TerminalLine> lines = session.SnapshotLineRange(start, cap);
  std::string text;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i != 0) {
      text.push_back('\n');
    }
    text += TerminalLineSliceText(lines[i], 0, lines[i].cells.size(), true);
  }
  while (!text.empty() && text.back() == '\n') {
    text.pop_back();
  }
  util::JsonObject object;
  object["tab"] = util::JsonValue(tab_index);
  object["pane"] = util::JsonValue(pane_index);
  object["running"] = util::JsonValue(session.running());
  object["lineCount"] = util::JsonValue(static_cast<std::int64_t>(line_count));
  object["text"] = util::JsonValue(std::move(text));
  const terminal::TerminalCursorSnapshot cursor = session.CursorSnapshot();
  util::JsonObject cursor_object;
  cursor_object["row"] = util::JsonValue(static_cast<std::int64_t>(cursor.row));
  cursor_object["column"] = util::JsonValue(static_cast<std::int64_t>(cursor.column));
  cursor_object["visible"] = util::JsonValue(cursor.visible);
  cursor_object["shape"] = util::JsonValue(std::string(
      cursor.shape == terminal::TerminalCursorShape::Bar         ? "bar"
      : cursor.shape == terminal::TerminalCursorShape::Underline ? "underline"
                                                                 : "block"));
  cursor_object["blinking"] = util::JsonValue(cursor.blinking);
  object["cursor"] = util::JsonValue(std::move(cursor_object));
  return util::JsonValue(std::move(object));
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
    project_object["loaded"] =
        util::JsonValue(i == catalog.active_index || (entry != nullptr && entry->initialized));
    if (const project::ProjectId::Remote* remote = context_->ProjectCatalogId(i).remote()) {
      project_object["host"] = util::JsonValue(remote->host);
      project_object["hostRoot"] = util::JsonValue(remote->host_root);
    }
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
    if (configs.size() >= kMaxControlQueryEntries) {
      break;
    }
    const LaunchConfig& config = state.launch_configs[i];
    util::JsonObject object;
    object["name"] = util::JsonValue(config.name);
    object["type"] = util::JsonValue(config.type);
    object["request"] = util::JsonValue(config.request);
    object["selected"] = util::JsonValue(i == state.selected_launch_config_index);
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
  std::size_t total_tabs = 0;
  for (const EditorGroup& group : state.editor_groups) {
    total_tabs += group.open_tabs.size();
  }
  object["tabCount"] = util::JsonValue(static_cast<std::int64_t>(total_tabs));
  object["debugStopped"] = util::JsonValue(state.debug_execution.stopped);
  object["lspRequestsInFlight"] =
      util::JsonValue(static_cast<std::int64_t>(state.lsp.request_in_flight_count));
  object["connections"] = util::JsonValue(static_cast<std::int64_t>(server_.ConnectionCount()));
  object["renderer"] = util::JsonValue(context_->render_driver_name);
  if (const std::optional<util::HeapUsage> heap = util::CurrentHeapUsage()) {
    util::JsonObject heap_object;
    heap_object["inUse"] = util::JsonValue(static_cast<std::int64_t>(heap->in_use_bytes));
    heap_object["free"] = util::JsonValue(static_cast<std::int64_t>(heap->free_bytes));
    object["heap"] = util::JsonValue(std::move(heap_object));
  }
  object["rendererIsGpu"] = util::JsonValue(context_->render_is_gpu);
  if (operations_.remote_project) {
    if (const auto remote = operations_.remote_project(state.root)) {
      util::JsonObject remote_object;
      remote_object["host"] = util::JsonValue(remote->host);
      remote_object["hostRoot"] = util::JsonValue(remote->host_root);
      remote_object["state"] = util::JsonValue(remote->state);
      remote_object["files"] = util::JsonValue(remote->files);
      remote_object["filesPending"] = util::JsonValue(remote->files_pending);
      remote_object["unsentPushes"] = util::JsonValue(remote->unsent_pushes);
      remote_object["conflicts"] = util::JsonValue(remote->conflicts);
      remote_object["blocked"] = util::JsonValue(remote->blocked);
      remote_object["heldDeletes"] = util::JsonValue(remote->held_deletes);
      object["remote"] = util::JsonValue(std::move(remote_object));
    }
  }

  const platform::SandboxSupport& sandbox = context_->sandbox_support;
  util::JsonObject sandbox_object;
  sandbox_object["compiledLandlock"] = util::JsonValue(sandbox.compiled_with_landlock);
  sandbox_object["landlockAvailable"] = util::JsonValue(sandbox.landlock_runtime_available);
  sandbox_object["landlockAbi"] = util::JsonValue(static_cast<std::int64_t>(sandbox.landlock_abi));
  sandbox_object["compiledSeccomp"] = util::JsonValue(sandbox.compiled_with_seccomp);
  sandbox_object["seccompAvailable"] = util::JsonValue(sandbox.seccomp_runtime_available);
  sandbox_object["active"] = util::JsonValue(sandbox.fully_active());
  object["sandbox"] = util::JsonValue(std::move(sandbox_object));
  return util::JsonValue(std::move(object));
}

void ControlChannelService::OnDebugStopBegan(const std::string& reason, int thread_id) {
  if (context_ == nullptr || (!server_.IsRunning() && !stdout_mirror_)) {
    return;
  }
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
  constexpr std::size_t kMaxDebugOutputEventBytes = 64u * 1024;
  if (text.size() <= kMaxDebugOutputEventBytes) {
    event["text"] = util::JsonValue(text);
  } else {
    std::string truncated(text, 0, util::Utf8ByteBudgetPrefixLength(text, kMaxDebugOutputEventBytes));
    truncated += "…[truncated]";
    event["text"] = util::JsonValue(std::move(truncated));
    event["truncated"] = util::JsonValue(true);
  }
  EmitEvent(util::JsonValue(std::move(event)));
}

}
