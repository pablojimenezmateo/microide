#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microide::workspace {

struct ControlSpecBreakpoint {
  std::string file;
  std::size_t line = 0;
  bool enabled = true;
  std::optional<std::string> condition;
  std::optional<std::string> hit_condition;
  std::optional<std::string> log_message;
};

struct ControlSpecFunctionBreakpoint {
  std::string name;
  bool enabled = true;
  std::optional<std::string> condition;
};

struct ControlSpec {
  bool valid = false;
  std::string parse_error;
  std::optional<std::filesystem::path> project;
  std::vector<std::pair<std::string, std::string>> settings;
  std::vector<ControlSpecBreakpoint> breakpoints;
  std::vector<ControlSpecFunctionBreakpoint> function_breakpoints;
  std::vector<std::string> open;
  std::optional<std::string> launch;
  std::vector<std::string> commands;
};

inline constexpr std::size_t kMaxControlSpecSectionEntries = 10000;

std::span<const std::string_view> ControlSpecKeys();

ControlSpec ParseControlSpec(std::string_view json);

std::vector<std::string> ControlSpecToCommands(const ControlSpec& spec,
                                               const std::filesystem::path& project_root);

}
