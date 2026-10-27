#include "workspace/services/RemoteHostActivityRows.h"

#include <algorithm>
#include <filesystem>

#include "project/remote/ServerStatusText.h"
#include "util/Parse.h"

namespace microide::workspace {
namespace {

namespace remote = project::remote;
using Item = plugin::PluginHost::SidebarItem;

constexpr std::string_view kDot = " \xc2\xb7 ";

std::string Count(std::size_t n, std::string_view one, std::string_view many) {
  return std::to_string(n) + " " + std::string(n == 1 ? one : many);
}

// The rows' totals: the sum of what was reported, or "…" when nothing was.
struct Totals {
  std::optional<std::int64_t> rss;
  std::optional<std::int64_t> cpu;
  void Add(const remote::ActivityFigures& figures) {
    if (figures.rss_bytes.has_value()) {
      rss = rss.value_or(0) + *figures.rss_bytes;
    }
    if (figures.cpu_permille.has_value()) {
      cpu = cpu.value_or(0) + *figures.cpu_permille;
    }
  }
  std::string Text() const {
    return remote::FormatFigures(remote::ActivityFigures{
        .rss_bytes = rss,
        .cpu_permille = cpu.has_value() ? std::optional<std::int32_t>(static_cast<std::int32_t>(
                                              std::min<std::int64_t>(*cpu, 1'000'000'000)))
                                        : std::nullopt});
  }
};

}  // namespace

std::vector<Item> BuildHostActivityRows(const HostActivityInput& input) {
  std::vector<Item> rows;
  const remote::ServerStatus* status = input.status;
  const auto collapsed = [&](std::string_view id) {
    return input.collapsed != nullptr && input.collapsed->count(id) > 0;
  };
  const auto group = [&](std::string id, std::string label, std::string detail) {
    const bool closed = collapsed(id);
    rows.push_back(Item{.label = std::move(label),
                        .detail = std::move(detail),
                        .id = std::move(id),
                        .depth = 0,
                        .collapsible = true,
                        .collapsed = closed});
    return !closed;
  };
  const auto row = [&](std::string id, std::string label, std::string detail) {
    rows.push_back(Item{.label = std::move(label), .detail = std::move(detail), .id = std::move(id), .depth = 1});
  };
  const auto own = [&](const std::string& root) {
    return std::find(input.own_roots.begin(), input.own_roots.end(), root) != input.own_roots.end();
  };

  // Server: the connection as this window sees it, and the server's own figures.
  if (group("group:server", "Server", input.connection_state)) {
    const std::string release = status != nullptr && !status->release.empty() ? status->release : input.server_release;
    std::string detail;
    if (status != nullptr && status->pid.has_value()) {
      detail = "pid " + std::to_string(*status->pid);
    }
    if (status != nullptr && status->uptime.has_value()) {
      detail += (detail.empty() ? "" : std::string(kDot)) + "up " + remote::FormatAge(*status->uptime);
    }
    if (status != nullptr) {
      detail += (detail.empty() ? "" : std::string(kDot)) + remote::FormatFigures(status->server);
    } else {
      detail = input.connection_state;
    }
    row("server", "microide-server" + (release.empty() ? std::string() : " " + release), std::move(detail));
  }
  if (status == nullptr) {
    return rows;
  }

  if (group("group:workspaces", "Workspaces", std::to_string(status->workspaces.size()))) {
    for (const remote::ServerWorkspace& workspace : status->workspaces) {
      std::string detail = Count(workspace.clients, "client", "clients");
      if (workspace.files.has_value()) {
        detail += std::string(kDot) + Count(*workspace.files, "file", "files");
      }
      if (!workspace.watch.empty() && workspace.watch != "none") {
        detail += std::string(kDot) + "watch " + workspace.watch;
      }
      if (own(workspace.root)) {
        detail += std::string(kDot) + "this window";
      }
      row("workspace:" + workspace.root, workspace.root, std::move(detail));
    }
  }

  if (!status->terminals.has_value()) {
    (void)group("group:terminals", "Terminals", "not reported");
  } else {
    Totals totals;
    for (const remote::ServerTerminal& terminal : *status->terminals) {
      totals.Add(terminal.figures);
    }
    if (group("group:terminals", "Terminals",
              std::to_string(status->terminals->size()) + std::string(kDot) + totals.Text())) {
      for (const remote::ServerTerminal& terminal : *status->terminals) {
        std::string place = remote::TerminalPlace(terminal.cwd, terminal.root);
        if (place.empty() || place == ".") {
          place = terminal.root.empty() ? std::string("~")
                                        : std::filesystem::path(terminal.root).filename().string();
        }
        std::string detail;
        if (terminal.exited) {
          detail = "exited";
        } else {
          detail = remote::FormatFigures(terminal.figures) + std::string(kDot) +
                   (terminal.clients == 0 ? std::string("no tab") : Count(terminal.clients, "client", "clients"));
        }
        row("terminal:" + std::to_string(terminal.handle), "#" + std::to_string(terminal.handle) + std::string(kDot) + place,
            std::move(detail));
      }
    }
  }

  if (!status->processes.has_value()) {
    (void)group("group:processes", "Processes", "not reported");
  } else {
    Totals totals;
    for (const remote::ServerProcess& process : *status->processes) {
      totals.Add(process.figures);
    }
    if (group("group:processes", "Processes",
              std::to_string(status->processes->size()) + std::string(kDot) + totals.Text())) {
      for (const remote::ServerProcess& process : *status->processes) {
        std::string state;
        if (process.exited) {
          state = "exited";
        } else if (process.detached_for.has_value()) {
          state = std::string(process.kept ? "kept" : "detached") + std::string(kDot) +
                  remote::FormatAge(*process.detached_for);
        } else {
          state = "running";
        }
        std::string detail = process.exited ? std::move(state)
                                            : state + std::string(kDot) + remote::FormatFigures(process.figures);
        row("process:" + std::to_string(process.handle), remote::ProcessDisplayName(process.argv), std::move(detail));
      }
    }
  }
  return rows;
}

std::optional<HostRowId> ParseHostRowId(std::string_view id) {
  const std::size_t colon = id.find(':');
  const std::string_view kind = id.substr(0, colon);
  const std::string_view rest = colon == std::string_view::npos ? std::string_view() : id.substr(colon + 1);
  if (kind == "server" && colon == std::string_view::npos) {
    return HostRowId{.kind = HostRowId::Kind::Server};
  }
  if (kind == "group" && !rest.empty()) {
    return HostRowId{.kind = HostRowId::Kind::Group, .name = rest};
  }
  if (kind == "workspace" && !rest.empty()) {
    return HostRowId{.kind = HostRowId::Kind::Workspace, .name = rest};
  }
  if (kind == "terminal" || kind == "process") {
    const std::optional<std::int64_t> handle = util::ParseInt64(rest);
    if (!handle.has_value() || *handle <= 0) {
      return std::nullopt;
    }
    return HostRowId{.kind = kind == "terminal" ? HostRowId::Kind::Terminal : HostRowId::Kind::Process,
                     .handle = static_cast<std::uint64_t>(*handle)};
  }
  return std::nullopt;
}

}  // namespace microide::workspace
