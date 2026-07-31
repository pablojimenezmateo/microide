#include "workspace/ControlChannelService.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>

#if defined(__unix__) || defined(__APPLE__)
#include <csignal>
#include <cerrno>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "platform/AppDirectories.h"
#include "platform/RuntimePaths.h"
#include "util/Parse.h"
#include "util/StringUtil.h"
#include "workspace/ControlProtocol.h"
#include "workspace/WorkspaceProjectPresentation.h"
#include "workspace/DebugViewModel.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/WorkspaceProjectState.h"

namespace microide::workspace {

namespace {

// The instances directory is world-droppable on the /tmp fallback (when
// $XDG_RUNTIME_DIR is unset), so every descriptor there is untrusted input to
// `control-list`/`control-send` — the CLI path an external driver runs. Bound
// both the per-descriptor slurp and the directory sweep so a hostile local
// process can't OOM or hang the driver by dropping one giant file or a million
// tiny ones.
constexpr std::uintmax_t kMaxDescriptorFileBytes = 1u << 20;  // 1 MiB
constexpr std::size_t kMaxControlInstances = 4096;
// Cap on directory entries *examined*, independent of how many are accepted. The
// accepted-instance cap above does NOT bound the sweep: every reject path (bad stem,
// oversized, unparseable, pid-mismatch) skips the instance push, and the pid-mismatch
// case is never pruned, so a hostile directory of `<int>.json` files with mismatched
// bodies would otherwise scan without limit. Legitimate instances number in the dozens;
// this ceiling is far above any real fleet while still bounding an adversarial sweep.
constexpr std::size_t kMaxControlInstanceScan = 65536;

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
  // ...except the master switch itself. `debug-toggle-enabled` exists to flip
  // `debug.enabled`, so auto-enabling first made it impossible to turn the
  // debugger OFF over the channel: the toggle read "enabled" every single time,
  // wrote "false", reported "Debugger disabled" — and the next debug- command
  // turned it straight back on. Three toggles in a row logged
  // raw=true/wrote=1/after=false three times, i.e. a no-op that always claims to
  // have disabled. Everything else in the surface still auto-enables.
  if (verb.starts_with("debug-toggle-enabled")) {
    return false;
  }
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
  std::size_t scanned = 0;
  // Advance with the non-throwing increment(ec): the range-for form uses the throwing
  // operator++, and this directory lives under a fallback runtime dir treated as
  // attacker-droppable/stale — an entry removed mid-sweep, a permission flip, or a
  // transient directory error would otherwise throw straight out of `control-list`.
  // A mid-sweep error stops enumeration with whatever was collected so far.
  std::filesystem::directory_iterator it(dir, ec);
  const std::filesystem::directory_iterator end;
  for (; !ec && it != end; it.increment(ec)) {
    const std::filesystem::directory_entry& entry = *it;
    // Bound the sweep by ENTRIES EXAMINED, not instances accepted: a reject path
    // (bad stem, oversized, unparseable, pid-mismatch) never grows `instances`, so a
    // million dropped files must not turn `control-list` into an unbounded loop /
    // kill-storm. Also stop once we have collected the accepted-instance cap.
    if (++scanned > kMaxControlInstanceScan || instances.size() >= kMaxControlInstances) {
      break;
    }
    if (entry.path().extension() != ".json") {
      continue;
    }
    // The descriptor is named <pid>.json by DescriptorPathForPid. Trust only that
    // filename for the pid — the file body is attacker-controllable. A file whose
    // stem is not a positive integer is not one of ours.
    const std::optional<int> filename_pid = util::ParseInt(entry.path().stem().string());
    if (!filename_pid.has_value() || *filename_pid <= 0) {
      continue;
    }
    // Refuse to slurp an oversized descriptor (OOM guard) before opening it.
    std::error_code size_ec;
    const std::uintmax_t file_bytes = entry.file_size(size_ec);
    if (size_ec || file_bytes > kMaxDescriptorFileBytes) {
      continue;
    }
    std::ifstream in(entry.path(), std::ios::binary);
    if (!in) {
      continue;
    }
    // Bound the ACTUAL read, not just the file_size() pre-check above: a hostile
    // local writer can grow the descriptor between the stat and the open (TOCTOU),
    // and an unbounded istreambuf slurp would then defeat kMaxDescriptorFileBytes and
    // OOM/stall the driver. Size the buffer to the stat'd length (already proven
    // <= kMaxDescriptorFileBytes above) rather than pre-allocating and zero-filling the
    // full 1 MiB cap for every ~1 KB descriptor — a hostile directory of small files
    // must not amplify into gigabytes of zero-fill. A file grown past its stat reads
    // truncated, fails JSON parsing, and is rejected below.
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
    // The pid must match the filename — a descriptor claiming a different (living)
    // pid is either stale or forged; don't let its body override the filename.
    const int pid = static_cast<int>((*parsed)["pid"].AsInt());
    if (pid != *filename_pid) {
      continue;
    }
    // Drop (and prune) descriptors whose process is gone, e.g. after a crash or
    // SIGKILL that skipped the graceful teardown.
    if (!ProcessIsAlive(pid)) {
      std::error_code remove_ec;
      std::filesystem::remove(entry.path(), remove_ec);
      continue;
    }
    ControlInstanceDescriptor descriptor;
    descriptor.pid = pid;
    // Ignore the advertised `socket` field entirely: reconstruct the canonical
    // path from the validated pid. The server only ever binds/rebinds
    // SocketPathForPid(pid), so a legitimate descriptor always matches — and a
    // forged `socket` field can never redirect a driver's connection to an
    // attacker-controlled Unix socket (which would inject spoofed responses/events
    // into the driver's stdout stream).
    descriptor.socket = SocketPathForPid(pid);
    descriptor.project_root = (*parsed)["project_root"].AsString();
    descriptor.project_hash = (*parsed)["project_hash"].AsString();
    // Re-serialize from the VALIDATED fields rather than echoing the file body.
    // `descriptor.socket` above is deliberately reconstructed from the pid so a
    // forged `socket` field cannot redirect a driver onto an attacker-controlled
    // Unix socket — but `control-list` prints this string, and printing the raw
    // body handed the forged field straight back to the driver, defeating that
    // defense at the only surface that matters. Re-serializing also guarantees
    // one line per instance: JSON escaping turns an embedded newline in a
    // descriptor written by a hostile local process into `\n`, so it can no
    // longer inject an extra forged line into the JSONL listing.
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

void ControlChannelService::SetWakeEventType(std::uint32_t event_type) {
  wake_event_type_ = event_type;
  server_.SetWakeEventType(event_type);
}

bool ControlChannelService::Start(const std::filesystem::path& project_root) {
  if (server_.IsRunning()) {
    return true;
  }
  const int pid = CurrentProcessId();
  // Harden the runtime base directory before binding. In the `/tmp/microide`
  // fallback (no $XDG_RUNTIME_DIR, no app state dir) the parent is world-writable,
  // so a local attacker could pre-create it as a symlink or a foreign-owned dir to
  // force rebinds or plant forged descriptors. Refuse to start on an untrusted
  // directory rather than expose sockets/descriptors there. (TD-2026-07-17-040.)
  if (!platform::EnsureSecurePrivateDirectory(RuntimeBaseDir())) {
    return false;
  }
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
  const std::filesystem::path parent = descriptor_path_.parent_path();
  std::filesystem::create_directories(parent, ec);
#if defined(__unix__) || defined(__APPLE__)
  // The descriptor exposes project_root/project_hash. Keep the runtime + instances
  // directories owner-only (0700) so other local users cannot enumerate active
  // project roots even when $XDG_RUNTIME_DIR is absent and we fell back to /tmp.
  ::chmod(parent.c_str(), S_IRWXU);
  if (parent.has_parent_path()) {
    ::chmod(parent.parent_path().c_str(), S_IRWXU);
  }
#endif
  util::JsonObject descriptor;
  descriptor["pid"] = util::JsonValue(static_cast<std::int64_t>(pid));
  descriptor["socket"] = util::JsonValue(SocketPathForPid(pid).generic_string());
  descriptor["project_root"] = util::JsonValue(project_root_.generic_string());
  descriptor["project_hash"] =
      util::JsonValue(project_root_.empty() ? std::string()
                                            : ProjectStateDirectoryName(project_root_));

  // Atomic publish: write a temp file then rename into place, so a concurrent
  // reader (a driver racing startup) never observes an empty or half-written
  // descriptor and skips a live instance. A crash mid-write leaves only the temp.
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
#if defined(__unix__) || defined(__APPLE__)
  ::chmod(temp_path.c_str(), S_IRUSR | S_IWUSR);  // 0600 before it becomes visible
#endif
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
  // Cap by aggregate breakpoint count (not just file count) so one huge file can't
  // dominate the response.
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
  // Enumerate every editor group (a split workspace has two) so a headless driver
  // can see and target tabs in either split. Each entry carries its `group` index;
  // `active` is true only for the active tab of the focused group.
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
    if (configs.size() >= kMaxControlQueryEntries) {
      break;
    }
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
  std::size_t total_tabs = 0;
  for (const EditorGroup& group : state.editor_groups) {
    total_tabs += group.open_tabs.size();
  }
  object["tabCount"] = util::JsonValue(static_cast<std::int64_t>(total_tabs));
  object["debugStopped"] = util::JsonValue(state.debug_execution.stopped);
  object["connections"] = util::JsonValue(static_cast<std::int64_t>(server_.ConnectionCount()));
  object["renderer"] = util::JsonValue(context_->render_driver_name);
  object["rendererIsGpu"] = util::JsonValue(context_->render_is_gpu);

  // Whether the per-plugin kernel confinement layers are actually live on this host. The confinement
  // is fail-open, so this lets an operator confirm it was installed rather than silently skipped.
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
  // TD-2026-07-17A-096: the IDE console side caps line fan-out at 100k lines, but the
  // raw output string was copied whole into this JSON event, so a DAP output event that
  // stays within the protocol body cap could still force a large JSON allocation/escape
  // pass and per-client write-buffer attempt. Byte-cap the emitted text on a UTF-8
  // boundary with a truncation marker/flag.
  constexpr std::size_t kMaxDebugOutputEventBytes = 64u * 1024;  // 64 KiB
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

}  // namespace microide::workspace
