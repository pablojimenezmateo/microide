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
  // Beside other hosts, its group is opened; alone, it is all there is.
  if (host_view_.collapsed.erase(HostGroupRowId(host)) > 0) {
    BuildHostRows();
  }
  UpdateActivitySubscription();
  return true;
}

void RemoteHostService::SetHostViewVisible(bool visible, const std::filesystem::path& active_root) {
  (void)active_root;
  if (visible == host_view_.visible) {
    return;
  }
  host_view_.visible = visible;
  if (visible) {
    BuildHostRows();  // each host's last status, if this window has one
  }
  UpdateActivitySubscription();
}

std::vector<std::string> RemoteHostService::CollapsedHostGroups() const {
  return {host_view_.collapsed.begin(), host_view_.collapsed.end()};
}

void RemoteHostService::RestoreCollapsedHostGroups(const std::vector<std::string>& ids) {
  host_view_.collapsed.clear();
  for (const std::string& id : ids) {
    if (const std::optional<HostRowId> row = ParseHostRowId(id);
        row.has_value() && (row->kind == HostRowId::Kind::Group || row->kind == HostRowId::Kind::Host)) {
      host_view_.collapsed.insert(id);
    }
  }
  BuildHostRows();
}

void RemoteHostService::UpdateActivitySubscription() {
  // Every host with an open project, while the view is on screen: subscribed
  // through the connection that should carry it, unsubscribed from one that
  // should not.
  std::map<std::string, std::shared_ptr<remote::RemoteServerClient>, std::less<>> wanted;
  if (host_view_.visible) {
    for (std::string& host : OpenHosts()) {
      if (std::shared_ptr<remote::RemoteServerClient> client = ConnectedClient(host); client != nullptr) {
        wanted.emplace(std::move(host), std::move(client));
      }
    }
  }
  // Both on the host-job queue, in order: an unsubscribe sent straight away could
  // overtake a subscribe still queued behind a slow job, leaving a hidden view
  // subscribed for good.
  for (auto it = host_view_.subscriptions.begin(); it != host_view_.subscriptions.end();) {
    std::shared_ptr<remote::RemoteServerClient> current = it->second.client.lock();
    const auto keep = wanted.find(it->first);
    if (keep != wanted.end() && keep->second == current) {
      wanted.erase(keep);  // as it should be
      ++it;
      continue;
    }
    if (current != nullptr) {
      work_.Post([current]() { current->UnsubscribeActivity(); });
    }
    it = host_view_.subscriptions.erase(it);
  }
  for (auto& [host, client] : wanted) {
    const std::uint64_t generation = ++host_view_.next_generation;
    host_view_.subscriptions[host] = HostView::Subscription{.client = client, .generation = generation};
    work_.Post([this, client = std::move(client), host, generation]() {
      std::string error;
      std::optional<remote::ServerStatus> status = client->SubscribeActivity(
          [this, host, generation](const remote::ServerStatus& pushed) {
            // Only the latest matters by the time the UI thread runs.
            mailbox_.PostLatest("remote-activity:" + host,
                                [this, host, generation, pushed]() { OnHostActivity(host, generation, pushed); });
          },
          &error);
      if (!status.has_value() && client->connected()) {
        // A server older than activity/subscribe: one plain status, and the view
        // stays as that says, with its figures not reported. Not an error.
        status = client->Status(&error);
      }
      mailbox_.Post([this, host, generation, status = std::move(status)]() mutable {
        OnHostActivity(host, generation, std::move(status));
      });
    });
  }
}

void RemoteHostService::OnHostActivity(const std::string& host, std::uint64_t generation,
                                       std::optional<remote::ServerStatus> status) {
  const auto it = host_view_.subscriptions.find(host);
  if (it == host_view_.subscriptions.end() || it->second.generation != generation || !status.has_value()) {
    return;
  }
  last_status_[host] = std::move(*status);
  BuildHostRows();
}

void RemoteHostService::BuildHostRows() {
  host_view_.rows.clear();
  const std::vector<std::string> hosts = OpenHosts();
  const bool several = hosts.size() > 1;
  for (const std::string& host : hosts) {
    HostActivityInput input{.host = host, .collapsed = &host_view_.collapsed};
    for (const auto& [tree, entry] : projects_) {
      (void)tree;
      if (entry.closed || entry.host != host) {
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
    if (const auto it = last_status_.find(host); it != last_status_.end()) {
      input.status = &it->second;
    }
    if (several) {
      // The host's own group: its connection, and what it runs once it said.
      std::string id = HostGroupRowId(host);
      const bool collapsed = host_view_.collapsed.count(id) > 0;
      std::string detail = input.connection_state;
      if (input.status != nullptr) {
        detail += " \xc2\xb7 " + remote::FormatFigures(input.status->server);
      }
      host_view_.rows.push_back(plugin::PluginHost::SidebarItem{.label = host,
                                                                .detail = std::move(detail),
                                                                .id = std::move(id),
                                                                .depth = 0,
                                                                .collapsible = true,
                                                                .collapsed = collapsed});
      if (collapsed) {
        continue;
      }
      input.id_prefix = HostRowPrefix(host);
      input.depth = 1;
    }
    std::vector<plugin::PluginHost::SidebarItem> rows = BuildHostActivityRows(input);
    host_view_.rows.insert(host_view_.rows.end(), std::make_move_iterator(rows.begin()),
                           std::make_move_iterator(rows.end()));
  }
  if (operations_.host_activity_updated) {
    operations_.host_activity_updated();
  }
}

std::vector<plugin::PluginHost::SidebarItem> RemoteHostService::HostRows() const {
  return host_view_.rows;
}

bool RemoteHostService::ToggleHostGroup(std::string_view id) {
  const std::optional<HostRowId> row = ParseHostRowId(id);
  if (!row.has_value() || (row->kind != HostRowId::Kind::Group && row->kind != HostRowId::Kind::Host)) {
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

std::pair<std::string, std::string_view> RemoteHostService::HostOfRow(std::string_view id) const {
  const std::optional<HostRowId> row = ParseHostRowId(id);
  if (!row.has_value()) {
    return {};
  }
  std::string host(row->host);
  if (!id.starts_with('@')) {
    host = SoleHost();  // an unprefixed id: the view shows one host
  } else {
    const std::size_t space = id.find(' ');
    id = space == std::string_view::npos ? std::string_view() : id.substr(space + 1);
  }
  return {std::move(host), id};
}

std::optional<RemoteHostService::RowCommand> RemoteHostService::HostRowOpenCommand(std::string_view id) const {
  const auto [view_host, local] = HostOfRow(id);
  const std::optional<HostRowId> row = ParseHostRowId(local);
  if (!row.has_value() || view_host.empty()) {
    return std::nullopt;
  }
  const std::string host = QuoteCommandArg(view_host);
  switch (row->kind) {
    case HostRowId::Kind::Terminal:
      // Focuses the tab that holds it, else attaches it in a new one.
      return RowCommand{.command_line = "remote-attach-terminals " + host + " " + std::to_string(row->handle)};
    case HostRowId::Kind::Server:
      return RowCommand{.command_line = "remote-stop-server " + host,
                        .confirm = "Stop microide-server on " + view_host +
                                   "? Its terminals and processes end with it; the next connect starts it again."};
    case HostRowId::Kind::Group:
    case HostRowId::Kind::Host:
    case HostRowId::Kind::Workspace:
    case HostRowId::Kind::Process:
      return std::nullopt;  // the row is information
  }
  return std::nullopt;
}

std::optional<RemoteHostService::RowCommand> RemoteHostService::HostRowEndCommand(std::string_view id) const {
  const auto [view_host, local] = HostOfRow(id);
  const std::optional<HostRowId> row = ParseHostRowId(local);
  const auto status = last_status_.find(view_host);
  if (!row.has_value() || view_host.empty() || status == last_status_.end()) {
    return std::nullopt;
  }
  const std::string host = QuoteCommandArg(view_host);
  if (row->kind == HostRowId::Kind::Terminal) {
    const remote::ServerTerminal* terminal = FindTerminal(status->second, row->handle);
    if (terminal == nullptr) {
      return std::nullopt;
    }
    // term/close from a connection that is not attached leaves a terminal someone
    // else is using alone, by design (tmux's detach): say so rather than appear to
    // have ended it.
    const std::shared_ptr<remote::RemoteServerClient> client = ConnectedClient(view_host);
    const bool here = client != nullptr && operations_.host_terminal_tab &&
                      operations_.host_terminal_tab(
                          terminal::HostIdentity{.epoch = client->hello().daemon_epoch, .handle = terminal->handle},
                          /*focus=*/false);
    if (!terminal->exited && terminal->clients > (here ? 1u : 0u)) {
      return RowCommand{.refusal = "Terminal #" + std::to_string(terminal->handle) + " on " + view_host +
                                   " is open in another window; close it there"};
    }
    RowCommand command{.command_line = "remote-close-terminals " + host + " " + std::to_string(terminal->handle)};
    if (!terminal->exited) {
      const std::string place = remote::TerminalPlace(terminal->cwd, terminal->root);
      command.confirm = "End terminal #" + std::to_string(terminal->handle) +
                        (place.empty() ? std::string() : " (" + place + ")") + " on " + view_host +
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
                        " on " + view_host + "?";
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
