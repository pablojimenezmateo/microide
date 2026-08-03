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

// One breakpoint from a cold-start spec. `line` is 1-based as authored; the
// 1-based -> 0-based conversion happens once, in the breakpoint-command
// executor, so every developer-facing surface (spec, commands, events) stays
// 1-based.
struct ControlSpecBreakpoint {
  std::string file;  // relative (to project root) or absolute, as authored
  std::size_t line = 0;
  bool enabled = true;
  std::optional<std::string> condition;
  std::optional<std::string> hit_condition;
  std::optional<std::string> log_message;
};

// One function breakpoint from a cold-start spec — breaks on a symbol name with
// no file/line. The right tool when the spec only knows a function name.
struct ControlSpecFunctionBreakpoint {
  std::string name;
  bool enabled = true;
  std::optional<std::string> condition;
};

// Parsed `--control-spec <file.json>` document. See ControlChannelHelpText() for
// the schema. On a parse error, `valid` is false and `parse_error` explains why.
struct ControlSpec {
  bool valid = false;
  std::string parse_error;
  std::optional<std::filesystem::path> project;
  // Transient setting overrides, applied first (never persisted). Escape hatch so
  // a spec can turn on control.enabled / debug.enabled through the chokepoint.
  std::vector<std::pair<std::string, std::string>> settings;
  std::vector<ControlSpecBreakpoint> breakpoints;
  std::vector<ControlSpecFunctionBreakpoint> function_breakpoints;
  std::vector<std::string> open;
  std::optional<std::string> launch;
  std::vector<std::string> commands;
};

// Per-section entry cap for a cold-start `--control-spec` document. Each section
// translates into synchronous command executions / JSONL `applied` events (and
// breakpoints into DAP `setBreakpoints` payloads), so a compact authored spec must
// not be able to enqueue an unbounded workload at startup. Exceeding the cap is a
// hard parse error (the spec is rejected whole). TD-2026-07-17A-038.
inline constexpr std::size_t kMaxControlSpecSectionEntries = 10000;

// The recognized top-level spec keys. Shared by the help text / drift guard.
std::span<const std::string_view> ControlSpecKeys();

// Decode spec JSON. Never throws; malformed input yields {valid=false,...}.
ControlSpec ParseControlSpec(std::string_view json);

// Translate a spec into the command lines that apply it, in order:
// breakpoints (set + modifiers + disable), file reveals, optional launch, then
// the spec's raw `commands`. Relative `file`/`open` paths resolve against
// `project_root`. Every produced line is safe to feed to ParseCommandLine /
// ExecuteCommandLine (arguments are quoted via QuoteCommandArg).
std::vector<std::string> ControlSpecToCommands(const ControlSpec& spec,
                                               const std::filesystem::path& project_root);

}  // namespace microide::workspace
