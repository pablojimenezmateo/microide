#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "project/remote/RemoteProtocol.h"

namespace microide::project::remote {

// How a host's activity reads to a person (remote-host-activity): one set of words
// for `microide-server status`'s table and the client's Host view, so the two
// never describe the same figure differently. A figure that was not reported
// reads as "…", never as zero.
inline constexpr std::string_view kNotReported = "\xe2\x80\xa6";

// "3.2%" (of one core), "…" when not reported.
std::string FormatCpuShare(std::optional<std::int32_t> permille);
// "41 MB", "1.2 GB", "820 KB", "…" when not reported.
std::string FormatBytes(std::optional<std::int64_t> bytes);
// "12 s", "12 min", "3 h 5 min", "2 d 4 h".
std::string FormatAge(std::chrono::milliseconds age);
// A process row's name: its argv joined, the program's directory dropped
// ("clangd --background-index"), cut to `max_bytes` on a UTF-8 boundary.
std::string ProcessDisplayName(const std::vector<std::string>& argv, std::size_t max_bytes = 64);
// Where a terminal is, against its workspace root: "." for the root itself, the
// relative path under it, the absolute path elsewhere, and "" when not reported.
std::string TerminalPlace(std::string_view cwd, std::string_view root);
// "3.2% · 41 MB", the two figures a row carries.
std::string FormatFigures(const ActivityFigures& figures);

// The roots a status lists anything under, in a stable order: the workspaces
// first, then the roots that only a terminal or a process still names (its
// workspace closed while it ran on).
std::vector<std::string> StatusRoots(const ServerStatus& status);

// `microide-server status`'s table: the server's line, then per root its
// workspace line and one line per terminal and process under it.
std::string FormatServerStatusTable(const ServerStatus& status);

}  // namespace microide::project::remote
