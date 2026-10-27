// RemoteHostService's Host view (remote-host-activity D4-D6): what a connected
// host runs for microide, pushed by the host while the view is on screen.
#include "workspace/services/RemoteHostService.h"

#include <algorithm>

#include "project/remote/RemoteProject.h"
#include "project/remote/RemoteServerClient.h"
#include "project/remote/ServerStatusText.h"
#include "workspace/WorkspaceCommandParsing.h"
#include "workspace/services/RemoteHostActivityRows.h"

namespace microide::workspace {
namespace {

namespace remote = project::remote;

const remote::ServerTerminal* FindTerminal(const remote::ServerStatus& status, std::uint64_t handle) {
  if (!status.terminals.has_value()) {
    return nullptr;
  }
  const auto it = std::find_if(status.terminals->begin(), status.terminals->end(),
                               [&](const remote::ServerTerminal& t) { return t.handle == handle; });
  return it != status.terminals->end() ? &*it : nullptr;
}

const remote::ServerProcess* FindProcess(const remote::ServerStatus& status, std::uint64_t handle) {
  if (!status.processes.has_value()) {
    return nullptr;
  }
  const auto it = std::find_if(status.processes->begin(), status.processes->end(),
                               [&](const remote::ServerProcess& p) { return p.handle == handle; });
  return it != status.processes->end() ? &*it : nullptr;
}

}  // namespace

std::shared_ptr<remote::RemoteServerClient> RemoteHostService::ConnectedClient(std::string_view host) const {
  const remote::RemoteHostSession* session = SessionFor(host);
  if (session == nullptr || session->status().state != remote::RemoteHostSession::State::Ready) {
    return nullptr;
  }
  std::shared_ptr<remote::RemoteServerClient> client = session->connection()->client();
  return client != nullptr && client->connected() ? client : nullptr;
}

bool RemoteHostService::ShowHost(std::string_view host) {
  if (SessionFor(host) == nullptr) {
    return false;
  }
  if (host_view_.host != host) {
    host_view_.host = std::string(host);
    host_view_.collapsed.clear();
    BuildHostRows();  // the new host's last status, if this window has one
  }
  UpdateActivitySubscription();
  return true;
}

void RemoteHostService::SetHostViewVisible(bool visible, const std::filesystem::path& active_root) {
  if (visible && host_view_.host.empty()) {
    // A view restored with the session, or opened as `sidebar host`: the active
    // project's host, else the only one.
    host_view_.host = HostOfProject(active_root);
    if (host_view_.host.empty()) {
      host_view_.host = SoleHost();
    }
    if (!host_view_.host.empty()) {
      BuildHostRows();
    }
  }
  if (visible == host_view_.visible) {
    return;
  }
  host_view_.visible = visible;
  UpdateActivitySubscription();
}

void RemoteHostService::UpdateActivitySubscription() {
  std::shared_ptr<remote::RemoteServerClient> wanted =
      host_view_.visible && !host_view_.host.empty() ? ConnectedClient(host_view_.host) : nullptr;
  std::shared_ptr<remote::RemoteServerClient> current = host_view_.subscribed.lock();
  if (wanted == current) {
    return;
  }
  const std::uint64_t generation = ++host_view_.generation;
  host_view_.subscribed = wanted;
  // Both on the host-job queue, in order: an unsubscribe sent straight away could
  // overtake a subscribe still queued behind a slow job, leaving a hidden view
  // subscribed for good.
  if (current != nullptr) {
    work_.Post([current]() { current->UnsubscribeActivity(); });
  }
  if (wanted == nullptr) {
    return;
  }
  const std::string host = host_view_.host;
  work_.Post([this, client = std::move(wanted), host, generation]() {
    std::string error;
    std::optional<remote::ServerStatus> status = client->SubscribeActivity(
        [this, host, generation](const remote::ServerStatus& pushed) {
          // Only the latest matters by the time the UI thread runs.
          mailbox_.PostLatest("remote-activity:" + host,
                              [this, generation, pushed]() { OnHostActivity(generation, pushed); });
        },
        &error);
    if (!status.has_value() && client->connected()) {
      // A server older than activity/subscribe: one plain status, and the view
      // stays as that says, with its figures not reported. Not an error.
      status = client->Status(&error);
    }
    mailbox_.Post([this, generation, status = std::move(status)]() mutable {
      OnHostActivity(generation, std::move(status));
    });
  });
}

void RemoteHostService::OnHostActivity(std::uint64_t generation, std::optional<remote::ServerStatus> status) {
  if (generation != host_view_.generation || !status.has_value()) {
    return;
  }
  last_status_[host_view_.host] = std::move(*status);
  BuildHostRows();
}

void RemoteHostService::BuildHostRows() {
  HostActivityInput input{.host = host_view_.host, .collapsed = &host_view_.collapsed};
  for (const auto& [tree, entry] : projects_) {
    (void)tree;
    if (entry.closed || entry.host != host_view_.host) {
      continue;
    }
    input.own_roots.push_back(entry.host_root);
    if (input.connection_state.empty()) {
      input.connection_state = std::string(remote::RemoteHostSession::StateName(entry.status.state));
      if (const auto client = entry.project->session().connection()->client(); client != nullptr) {
        input.server_release = client->hello().release;
      }
    }
  }
  if (input.connection_state.empty()) {
    input.connection_state = "no open project";
  }
  if (const auto it = last_status_.find(host_view_.host); it != last_status_.end()) {
    input.status = &it->second;
  }
  host_view_.rows = host_view_.host.empty() ? std::vector<plugin::PluginHost::SidebarItem>{}
                                            : BuildHostActivityRows(input);
  if (operations_.host_activity_updated) {
    operations_.host_activity_updated();
  }
}

std::vector<plugin::PluginHost::SidebarItem> RemoteHostService::HostRows() const {
  return host_view_.rows;
}

bool RemoteHostService::ToggleHostGroup(std::string_view id) {
  const std::optional<HostRowId> row = ParseHostRowId(id);
  if (!row.has_value() || row->kind != HostRowId::Kind::Group) {
    return false;
  }
  if (const auto it = host_view_.collapsed.find(id); it != host_view_.collapsed.end()) {
    host_view_.collapsed.erase(it);
  } else {
    host_view_.collapsed.emplace(id);
  }
  BuildHostRows();
  return true;
}

std::optional<RemoteHostService::RowCommand> RemoteHostService::HostRowOpenCommand(std::string_view id) const {
  const std::optional<HostRowId> row = ParseHostRowId(id);
  if (!row.has_value() || host_view_.host.empty()) {
    return std::nullopt;
  }
  const std::string host = QuoteCommandArg(host_view_.host);
  switch (row->kind) {
    case HostRowId::Kind::Terminal:
      // Focuses the tab that holds it, else attaches it in a new one.
      return RowCommand{.command_line = "remote-attach-terminals " + host + " " + std::to_string(row->handle)};
    case HostRowId::Kind::Server:
      return RowCommand{.command_line = "remote-stop-server " + host,
                        .confirm = "Stop microide-server on " + host_view_.host +
                                   "? Its terminals and processes end with it; the next connect starts it again."};
    case HostRowId::Kind::Group:
    case HostRowId::Kind::Workspace:
    case HostRowId::Kind::Process:
      return std::nullopt;  // the row is information
  }
  return std::nullopt;
}

std::optional<RemoteHostService::RowCommand> RemoteHostService::HostRowEndCommand(std::string_view id) const {
  const std::optional<HostRowId> row = ParseHostRowId(id);
  const auto status = last_status_.find(host_view_.host);
  if (!row.has_value() || status == last_status_.end()) {
    return std::nullopt;
  }
  const std::string host = QuoteCommandArg(host_view_.host);
  if (row->kind == HostRowId::Kind::Terminal) {
    const remote::ServerTerminal* terminal = FindTerminal(status->second, row->handle);
    if (terminal == nullptr) {
      return std::nullopt;
    }
    // term/close from a connection that is not attached leaves a terminal someone
    // else is using alone, by design (tmux's detach): say so rather than appear to
    // have ended it.
    const std::shared_ptr<remote::RemoteServerClient> client = ConnectedClient(host_view_.host);
    const bool here = client != nullptr && operations_.host_terminal_tab &&
                      operations_.host_terminal_tab(
                          terminal::HostIdentity{.epoch = client->hello().daemon_epoch, .handle = terminal->handle},
                          /*focus=*/false);
    if (!terminal->exited && terminal->clients > (here ? 1u : 0u)) {
      return RowCommand{.refusal = "Terminal #" + std::to_string(terminal->handle) + " on " + host_view_.host +
                                   " is open in another window; close it there"};
    }
    RowCommand command{.command_line = "remote-close-terminals " + host + " " + std::to_string(terminal->handle)};
    if (!terminal->exited) {
      const std::string place = remote::TerminalPlace(terminal->cwd, terminal->root);
      command.confirm = "End terminal #" + std::to_string(terminal->handle) +
                        (place.empty() ? std::string() : " (" + place + ")") + " on " + host_view_.host +
                        "? Its shell and everything it runs end.";
    }
    return command;
  }
  if (row->kind == HostRowId::Kind::Process) {
    const remote::ServerProcess* process = FindProcess(status->second, row->handle);
    if (process == nullptr) {
      return std::nullopt;
    }
    RowCommand command{.command_line = "remote-end-process " + host + " " + std::to_string(process->handle)};
    if (!process->exited) {
      command.confirm = "End " + remote::ProcessDisplayName(process->argv) +
                        (process->pid.has_value() ? " (pid " + std::to_string(*process->pid) + ")" : std::string()) +
                        " on " + host_view_.host + "?";
    }
    return command;
  }
  return std::nullopt;
}

bool RemoteHostService::EndHostProcess(std::string_view host, std::uint64_t handle) {
  const remote::RemoteHostSession* session = SessionFor(host);
  const std::shared_ptr<remote::RemoteServerClient> client =
      session != nullptr ? session->connection()->client() : nullptr;
  if (client == nullptr) {
    return false;
  }
  util::JsonObject params;
  params["handle"] = util::JsonValue(static_cast<std::int64_t>(handle));
  client->peer().Notify(remote::method::kProcEnd, util::JsonValue(std::move(params)));
  return true;
}

}  // namespace microide::workspace
