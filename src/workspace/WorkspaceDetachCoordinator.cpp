#include "workspace/WorkspaceDetachCoordinator.h"

#include <atomic>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <chrono>
#include <optional>

#include "platform/ControlSocketClient.h"
#include "platform/DetachedProcess.h"
#include "util/JsonValue.h"
#include "workspace/ControlChannelService.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/WorkspacePersistenceCoordinator.h"
#include "workspace/WorkspaceShell.h"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace microide::workspace {

namespace {

int CurrentPid() {
#if defined(_WIN32)
  return _getpid();
#else
  return static_cast<int>(::getpid());
#endif
}

// A transient scratch path for one handoff payload under the system temp dir. The
// child deletes it after hydrating (consume-once); the parent deletes it if the
// spawn never happened. Unique per (pid, monotonic counter) so concurrent detaches
// never collide.
std::filesystem::path MakeHandoffPath() {
  std::error_code ec;
  std::filesystem::path base = std::filesystem::temp_directory_path(ec);
  if (ec || base.empty()) {
    return {};
  }
  base /= "microide-handoff";
  std::filesystem::create_directories(base, ec);
  if (ec) {
    return {};
  }
  static std::atomic<unsigned> counter{0};
  const unsigned serial = counter.fetch_add(1, std::memory_order_relaxed);
  return base / ("handoff-" + std::to_string(CurrentPid()) + "-" + std::to_string(serial) +
                 ".session");
}

// Send `accept-tab-handoff <file>` to the control socket at `socket` and wait for
// its reply. Returns true only when the target acknowledged with ok:true.
bool SendHandoffToSocket(const std::filesystem::path& socket,
                         const std::filesystem::path& handoff_file) {
  platform::ControlSocketClient client;
  if (!client.Connect(socket)) {
    return false;
  }
  util::JsonObject request;
  request["id"] = util::JsonValue(static_cast<std::int64_t>(1));
  request["command"] =
      util::JsonValue("accept-tab-handoff " + QuoteCommandArg(handoff_file.string()));
  if (!client.SendLine(util::SerializeJson(util::JsonValue(std::move(request))))) {
    return false;
  }
  client.ShutdownWrite();
  const std::optional<std::string> reply = client.ReadLine(std::chrono::seconds(3));
  if (!reply.has_value()) {
    return false;
  }
  const std::optional<util::JsonValue> parsed = util::ParseJson(*reply);
  return parsed.has_value() && parsed->IsObject() && (*parsed)["ok"].AsBool(false);
}

}  // namespace

std::optional<TabDropTarget> ResolveDropTarget(
    int global_x, int global_y, int self_pid,
    const std::vector<ControlInstanceDescriptor>& instances) {
  for (const ControlInstanceDescriptor& instance : instances) {
    if (instance.pid == self_pid) {
      continue;  // Dropping on your own window is an in-window reorder, not reattach.
    }
    if (instance.win_w <= 0 || instance.win_h <= 0) {
      continue;  // Geometry not published yet: cannot hit-test this window.
    }
    if (global_x >= instance.win_x && global_x < instance.win_x + instance.win_w &&
        global_y >= instance.win_y && global_y < instance.win_y + instance.win_h) {
      return TabDropTarget{.pid = instance.pid, .socket = instance.socket};
    }
  }
  return std::nullopt;
}

WorkspaceDetachCoordinator::WorkspaceDetachCoordinator(Operations operations)
    : operations_(std::move(operations)) {}

bool WorkspaceDetachCoordinator::DetachActiveTab() {
  if (!operations_.build_active_tab_handoff || !operations_.project_root ||
      !operations_.close_active_tab_discarding_dirty) {
    return false;
  }
  const std::filesystem::path root = operations_.project_root();
  if (root.empty()) {
    return false;
  }
  const PersistedProjectSessionState handoff = operations_.build_active_tab_handoff();
  if (handoff.groups.empty() || handoff.groups.front().tabs.empty()) {
    return false;  // Transient / empty tab: nothing detachable to hand off.
  }
  if (!SpawnHandoffWindow(handoff, root, /*child_owns_session=*/false)) {
    return false;
  }
  operations_.close_active_tab_discarding_dirty();
  return true;
}

bool WorkspaceDetachCoordinator::DetachActiveProject() {
  if (!operations_.build_project_handoff || !operations_.project_root ||
      !operations_.active_project_index || !operations_.request_close_project) {
    return false;
  }
  const std::filesystem::path root = operations_.project_root();
  if (root.empty()) {
    return false;
  }
  // Even a project with no persistable tabs opens fine in the child (welcome
  // surface), so an empty-groups handoff is still a valid project detach.
  const PersistedProjectSessionState handoff = operations_.build_project_handoff();
  if (!SpawnHandoffWindow(handoff, root, /*child_owns_session=*/true)) {
    return false;
  }
  operations_.request_close_project(operations_.active_project_index());
  return true;
}

bool WorkspaceDetachCoordinator::DropActiveTabAtGlobal(int global_x, int global_y) {
  if (!operations_.build_active_tab_handoff || !operations_.project_root ||
      !operations_.write_handoff || !operations_.close_active_tab_discarding_dirty) {
    return false;
  }
  const std::filesystem::path root = operations_.project_root();
  if (root.empty()) {
    return false;
  }
  const PersistedProjectSessionState handoff = operations_.build_active_tab_handoff();
  if (handoff.groups.empty() || handoff.groups.front().tabs.empty()) {
    return false;  // Transient / empty tab: nothing to move.
  }

  // Prefer handing the tab to an existing window under the drop point (reattach).
  const std::optional<TabDropTarget> target =
      ResolveDropTarget(global_x, global_y, CurrentPid(), EnumerateControlInstances());
  if (target.has_value()) {
    const std::filesystem::path handoff_path = MakeHandoffPath();
    if (!handoff_path.empty() && operations_.write_handoff(handoff_path, handoff) &&
        SendHandoffToSocket(target->socket, handoff_path)) {
      operations_.close_active_tab_discarding_dirty();
      return true;
    }
    // The target vanished or refused; clean up and fall back to a new window so
    // the tab is never lost.
    std::error_code ec;
    std::filesystem::remove(handoff_path, ec);
  }

  // No window under the point (or the handoff send failed): detach into a fresh
  // window, exactly like the menu detach path.
  if (!SpawnHandoffWindow(handoff, root, /*child_owns_session=*/false)) {
    return false;
  }
  operations_.close_active_tab_discarding_dirty();
  return true;
}

bool WorkspaceDetachCoordinator::SpawnHandoffWindow(const PersistedProjectSessionState& handoff,
                                                    const std::filesystem::path& project_root,
                                                    bool child_owns_session) {
  if (!operations_.write_handoff) {
    return false;
  }
  const std::filesystem::path exe = platform::CurrentExecutablePath();
  if (exe.empty()) {
    return false;
  }
  const std::filesystem::path handoff_path = MakeHandoffPath();
  if (handoff_path.empty() || !operations_.write_handoff(handoff_path, handoff)) {
    return false;
  }
  std::vector<std::string> argv = {exe.string(), project_root.string(), "--detach-handoff",
                                   handoff_path.string()};
  if (child_owns_session) {
    argv.push_back("--detach-owns-session");
  }
  // No --set control.enabled needed: every window binds a handoff-capable socket
  // by default (see WorkspaceShell::MaybeStartControlChannel), so the child is a
  // reattach drop target the moment it opens.
  if (!platform::SpawnDetached(argv, project_root)) {
    std::error_code ec;
    std::filesystem::remove(handoff_path, ec);  // No child consumed it; clean up.
    return false;
  }
  return true;
}

WorkspaceDetachCoordinator WorkspaceShell::MakeDetachCoordinator() {
  return WorkspaceDetachCoordinator(WorkspaceDetachCoordinator::Operations{
      .build_active_tab_handoff =
          [this]() {
            SyncActiveEditorTab();
            auto& state = context_.current_project_state;
            return MakePersistenceCoordinator().BuildSingleTabHandoff(
                state.clamped_focused_group_index(), state.focused_group().active_tab_index);
          },
      .build_project_handoff =
          [this]() {
            SyncActiveEditorTab();
            return MakePersistenceCoordinator().BuildPersistedProjectSession();
          },
      .write_handoff =
          [this](const std::filesystem::path& out_path,
                 const PersistedProjectSessionState& session) {
            return MakePersistenceCoordinator().WriteHandoffFile(out_path, session);
          },
      .project_root = [this]() { return context_.current_project_state.root; },
      .active_project_index = [this]() { return context_.project_catalog.active_index; },
      .request_close_project = [this](std::size_t index) { RequestCloseProject(index); },
      .close_active_tab_discarding_dirty =
          [this]() {
            CloseTab(context_.current_project_state.focused_group().active_tab_index);
          },
  });
}

}  // namespace microide::workspace
