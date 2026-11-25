#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "project/remote/RemoteProtocol.h"

namespace microide::project::remote {

inline constexpr std::string_view kNotReported = "\xe2\x80\xa6";
inline constexpr std::string_view kFieldSeparator = " \xc2\xb7 ";

std::string FormatCount(std::size_t n, std::string_view one, std::string_view many);

std::string FormatCpuShare(std::optional<std::int32_t> permille);
std::string FormatBytes(std::optional<std::int64_t> bytes);
std::string FormatAge(std::chrono::milliseconds age);
std::string FormatLocalTime(std::uint64_t seconds_since_epoch);
std::string ProcessDisplayName(const std::vector<std::string>& argv, std::size_t max_bytes = 64);
std::string TerminalPlace(std::string_view cwd, std::string_view root);
bool DiskIsLow(const ServerWorkspace& workspace);
std::string FormatWorkspaceFigures(const ServerWorkspace& workspace);
std::string FormatFigures(const ActivityFigures& figures);

std::vector<std::string> StatusRoots(const ServerStatus& status);

std::string FormatServerStatusTable(const ServerStatus& status);

}
