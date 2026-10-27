#include "project/remote/ServerStatusText.h"

#include <algorithm>
#include <cstdio>

namespace microide::project::remote {
namespace {

constexpr std::string_view kDot = " \xc2\xb7 ";

std::string Count(std::size_t n, std::string_view one, std::string_view many) {
  return std::to_string(n) + " " + std::string(n == 1 ? one : many);
}

void Pad(std::string& line, std::size_t column) {
  // Padding counts bytes; the table's text is ASCII but for "…" and "·", which
  // only ever follow the last padded column.
  if (line.size() < column) {
    line.append(column - line.size(), ' ');
  } else {
    line.push_back(' ');
  }
}

}  // namespace

std::string FormatCpuShare(std::optional<std::int32_t> permille) {
  if (!permille.has_value()) {
    return std::string(kNotReported);
  }
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f%%", static_cast<double>(*permille) / 10.0);
  return text;
}

std::string FormatBytes(std::optional<std::int64_t> bytes) {
  if (!bytes.has_value()) {
    return std::string(kNotReported);
  }
  constexpr double kKilo = 1024.0;
  const double value = static_cast<double>(*bytes);
  char text[32];
  if (value >= kKilo * kKilo * kKilo) {
    std::snprintf(text, sizeof(text), "%.1f GB", value / (kKilo * kKilo * kKilo));
  } else if (value >= kKilo * kKilo) {
    std::snprintf(text, sizeof(text), "%.0f MB", value / (kKilo * kKilo));
  } else {
    std::snprintf(text, sizeof(text), "%.0f KB", value / kKilo);
  }
  return text;
}

std::string FormatAge(std::chrono::milliseconds age) {
  const std::int64_t seconds = std::max<std::int64_t>(age.count() / 1000, 0);
  if (seconds < 60) {
    return std::to_string(seconds) + " s";
  }
  const std::int64_t minutes = seconds / 60;
  if (minutes < 60) {
    return std::to_string(minutes) + " min";
  }
  const std::int64_t hours = minutes / 60;
  if (hours < 24) {
    return std::to_string(hours) + " h" + (minutes % 60 != 0 ? " " + std::to_string(minutes % 60) + " min" : "");
  }
  return std::to_string(hours / 24) + " d" + (hours % 24 != 0 ? " " + std::to_string(hours % 24) + " h" : "");
}

std::string ProcessDisplayName(const std::vector<std::string>& argv, std::size_t max_bytes) {
  std::string name;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    std::string_view arg = argv[i];
    if (i == 0) {
      if (const std::size_t slash = arg.rfind('/'); slash != std::string_view::npos && slash + 1 < arg.size()) {
        arg.remove_prefix(slash + 1);
      }
    } else {
      name.push_back(' ');
    }
    name.append(arg);
    if (name.size() > max_bytes) {
      break;
    }
  }
  if (name.size() > max_bytes) {
    std::size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) {
      --cut;  // not inside a UTF-8 sequence
    }
    name.resize(cut);
    name.append(kNotReported);
  }
  return name.empty() ? std::string("(no argv)") : name;
}

std::string TerminalPlace(std::string_view cwd, std::string_view root) {
  if (cwd.empty()) {
    return {};
  }
  if (!root.empty() && cwd == root) {
    return ".";
  }
  if (!root.empty() && cwd.size() > root.size() && cwd.starts_with(root) &&
      (root.back() == '/' || cwd[root.size()] == '/')) {
    return std::string(cwd.substr(root.size() + (root.back() == '/' ? 0 : 1)));
  }
  return std::string(cwd);
}

std::string FormatFigures(const ActivityFigures& figures) {
  return FormatCpuShare(figures.cpu_permille) + std::string(kDot) + FormatBytes(figures.rss_bytes);
}

std::vector<std::string> StatusRoots(const ServerStatus& status) {
  std::vector<std::string> roots;
  const auto add = [&](const std::string& root) {
    if (std::find(roots.begin(), roots.end(), root) == roots.end()) {
      roots.push_back(root);
    }
  };
  for (const ServerWorkspace& workspace : status.workspaces) {
    add(workspace.root);
  }
  if (status.terminals.has_value()) {
    for (const ServerTerminal& terminal : *status.terminals) {
      add(terminal.root);
    }
  }
  if (status.processes.has_value()) {
    for (const ServerProcess& process : *status.processes) {
      add(process.root);
    }
  }
  return roots;
}

std::string FormatServerStatusTable(const ServerStatus& status) {
  std::string out = "microide-server";
  if (!status.release.empty()) {
    out += " " + status.release;
  }
  if (status.pid.has_value()) {
    out += std::string(kDot) + "pid " + std::to_string(*status.pid);
  }
  if (status.uptime.has_value()) {
    out += std::string(kDot) + "up " + FormatAge(*status.uptime);
  }
  out += std::string(kDot) + FormatFigures(status.server);
  out += "\n";
  const std::vector<std::string> roots = StatusRoots(status);
  if (roots.empty()) {
    out += "  nothing open\n";
    return out;
  }
  constexpr std::size_t kNameColumn = 4;
  constexpr std::size_t kPidColumn = 44;
  constexpr std::size_t kStateColumn = 56;
  constexpr std::size_t kFiguresColumn = 76;
  for (const std::string& root : roots) {
    out += "\n" + (root.empty() ? std::string("(no workspace)") : root);
    const auto workspace = std::find_if(status.workspaces.begin(), status.workspaces.end(),
                                        [&](const ServerWorkspace& w) { return w.root == root; });
    if (workspace != status.workspaces.end()) {
      out += std::string(kDot) + Count(workspace->clients, "client", "clients");
      if (workspace->files.has_value()) {
        out += std::string(kDot) + Count(*workspace->files, "file", "files");
      }
      if (!workspace->watch.empty() && workspace->watch != "none") {
        out += std::string(kDot) + "watch " + workspace->watch;
      }
    } else {
      out += std::string(kDot) + "not open";
    }
    out += "\n";
    const auto row = [&](std::string name, std::optional<int> pid, std::string state, const ActivityFigures& figures) {
      std::string line(kNameColumn, ' ');
      line += name;
      Pad(line, kPidColumn);
      line += pid.has_value() ? "pid " + std::to_string(*pid) : std::string("-");
      Pad(line, kStateColumn);
      line += state;
      Pad(line, kFiguresColumn);
      line += FormatFigures(figures);
      out += line + "\n";
    };
    if (status.terminals.has_value()) {
      for (const ServerTerminal& terminal : *status.terminals) {
        if (terminal.root != root) {
          continue;
        }
        std::string place = TerminalPlace(terminal.cwd, terminal.root);
        if (!terminal.command.empty()) {
          place += (place.empty() ? "" : " ") + std::string("(") + terminal.command + ")";
        }
        row("terminal #" + std::to_string(terminal.handle) + (place.empty() ? "" : " " + place), terminal.pid,
            terminal.exited ? std::string("exited") : Count(terminal.clients, "client", "clients"),
            terminal.figures);
      }
    }
    if (status.processes.has_value()) {
      for (const ServerProcess& process : *status.processes) {
        if (process.root != root) {
          continue;
        }
        std::string state = process.exited                  ? std::string("exited")
                            : process.detached_for.has_value() ? (process.kept ? "kept " : "detached ") +
                                                                     FormatAge(*process.detached_for)
                                                               : std::string("running");
        row(ProcessDisplayName(process.argv, kPidColumn - kNameColumn - 2), process.pid, std::move(state),
            process.figures);
      }
    }
  }
  return out;
}

}  // namespace microide::project::remote
