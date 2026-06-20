#include "TestSupport.h"

#include <algorithm>
#include <string>
#include <vector>

#include "workspace/ControlSpec.h"

namespace microide::tests {
namespace {

using microide::workspace::ControlSpec;
using microide::workspace::ControlSpecToCommands;
using microide::workspace::ParseControlSpec;

bool ContainsCommand(const std::vector<std::string>& commands, std::string_view needle) {
  return std::any_of(commands.begin(), commands.end(), [&](const std::string& command) {
    return command.find(needle) != std::string::npos;
  });
}

void TestParseFullSpec() {
  const ControlSpec spec = ParseControlSpec(R"({
    "project": "/tmp/proj",
    "breakpoints": [
      {"file": "src/main.cpp", "line": 42},
      {"file": "src/util.cpp", "line": 7, "condition": "x > 10", "enabled": false}
    ],
    "open": ["README.md"],
    "launch": "Debug",
    "commands": ["sidebar-hide"]
  })");
  Expect(spec.valid, "spec should parse");
  Expect(spec.project.has_value() && spec.project->generic_string() == "/tmp/proj",
         "project should parse");
  Expect(spec.breakpoints.size() == 2, "two breakpoints expected");
  Expect(spec.breakpoints[0].line == 42, "line should stay 1-based in the spec");
  Expect(spec.breakpoints[1].condition.has_value() && *spec.breakpoints[1].condition == "x > 10",
         "condition should parse");
  Expect(!spec.breakpoints[1].enabled, "enabled:false should parse");
  Expect(spec.open.size() == 1 && spec.open[0] == "README.md", "open list should parse");
  Expect(spec.launch.has_value() && *spec.launch == "Debug", "launch should parse");
  Expect(spec.commands.size() == 1 && spec.commands[0] == "sidebar-hide",
         "raw commands should parse");
}

void TestParseRejectsBadInput() {
  Expect(!ParseControlSpec("nope").valid, "garbage should be rejected");
  Expect(!ParseControlSpec("[]").valid, "non-object should be rejected");
  Expect(!ParseControlSpec(R"({"breakpoints":[{"file":"a","line":0}]})").valid,
         "line < 1 should be rejected");
  Expect(!ParseControlSpec(R"({"breakpoints":[{"line":3}]})").valid,
         "missing file should be rejected");
}

void TestToCommandsResolvesAndConverts() {
  const ControlSpec spec = ParseControlSpec(R"({
    "breakpoints": [
      {"file": "src/main.cpp", "line": 42, "condition": "x > 10"},
      {"file": "src/log.cpp", "line": 5, "logMessage": "hit", "enabled": false}
    ]
  })");
  Expect(spec.valid, "spec should parse");
  const std::vector<std::string> commands = ControlSpecToCommands(spec, "/home/u/proj");
  // Relative path resolved against the project root; line stays 1-based for the
  // command (the executor converts to 0-based).
  Expect(ContainsCommand(commands, "breakpoint-set /home/u/proj/src/main.cpp 42"),
         "breakpoint-set should carry the resolved path and 1-based line");
  Expect(ContainsCommand(commands, "breakpoint-condition /home/u/proj/src/main.cpp 42 'x > 10'"),
         "condition should be quoted and emitted");
  Expect(ContainsCommand(commands, "breakpoint-logmessage /home/u/proj/src/log.cpp 5 hit"),
         "logmessage should be emitted");
  Expect(ContainsCommand(commands, "breakpoint-disable /home/u/proj/src/log.cpp 5"),
         "disabled breakpoint should emit a disable command");
  // With no explicit open list, the first breakpoint file is revealed at its line.
  Expect(ContainsCommand(commands, "open /home/u/proj/src/main.cpp"),
         "first breakpoint file should be opened");
  Expect(ContainsCommand(commands, "goto 42"), "first breakpoint line should be revealed");
}

void TestParseFunctionBreakpoints() {
  const ControlSpec spec = ParseControlSpec(R"({
    "functionBreakpoints": [
      {"name": "main"},
      {"name": "process", "condition": "n > 3", "enabled": false}
    ]
  })");
  Expect(spec.valid, "spec with function breakpoints should parse");
  Expect(spec.function_breakpoints.size() == 2, "two function breakpoints expected");
  Expect(spec.function_breakpoints[0].name == "main", "first function name should parse");
  Expect(spec.function_breakpoints[1].condition.has_value() &&
             *spec.function_breakpoints[1].condition == "n > 3",
         "function breakpoint condition should parse");
  Expect(!spec.function_breakpoints[1].enabled, "enabled:false should parse");

  const std::vector<std::string> commands = ControlSpecToCommands(spec, "/home/u/proj");
  Expect(ContainsCommand(commands, "breakpoint-function-add main"),
         "an add command should be emitted for the function name");
  Expect(ContainsCommand(commands, "breakpoint-function-condition process 'n > 3'"),
         "a condition command should be emitted and quoted");
  Expect(ContainsCommand(commands, "breakpoint-function-toggle process"),
         "a disabled function breakpoint should toggle off after add");
}

void TestParseFunctionBreakpointsRejectsBadInput() {
  Expect(!ParseControlSpec(R"({"functionBreakpoints": [{"name": ""}]})").valid,
         "empty function name should be rejected");
  Expect(!ParseControlSpec(R"({"functionBreakpoints": [{}]})").valid,
         "missing function name should be rejected");
  Expect(!ParseControlSpec(R"({"functionBreakpoints": "nope"})").valid,
         "non-array functionBreakpoints should be rejected");
}

void TestParseSettingsArrayForm() {
  const ControlSpec spec = ParseControlSpec(R"({
    "settings": [["control.enabled", "true"], ["debug.enabled", "true"]],
    "breakpoints": [{"file": "src/main.cpp", "line": 1}]
  })");
  Expect(spec.valid, "spec with settings array should parse");
  Expect(spec.settings.size() == 2, "two settings expected");
  Expect(spec.settings[0] == std::pair<std::string, std::string>("control.enabled", "true"),
         "first setting should preserve id/value/order");
  Expect(spec.settings[1] == std::pair<std::string, std::string>("debug.enabled", "true"),
         "second setting should preserve id/value/order");
}

void TestParseSettingsObjectForm() {
  const ControlSpec spec = ParseControlSpec(R"({"settings": {"control.enabled": "true"}})");
  Expect(spec.valid, "spec with settings object should parse");
  Expect(spec.settings.size() == 1 &&
             spec.settings[0] == std::pair<std::string, std::string>("control.enabled", "true"),
         "object-form setting should parse");
}

void TestParseSettingsRejectsBadShape() {
  Expect(!ParseControlSpec(R"({"settings": [["only-one"]]})").valid,
         "settings pair with one element should be rejected");
  Expect(!ParseControlSpec(R"({"settings": {"k": 1}})").valid,
         "non-string settings value should be rejected");
  Expect(!ParseControlSpec(R"({"settings": "nope"})").valid,
         "scalar settings should be rejected");
}

}  // namespace

void RegisterControlSpecTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ControlSpec/ParseFullSpec", TestParseFullSpec);
  AddTest(tests, "ControlSpec/ParseRejectsBadInput", TestParseRejectsBadInput);
  AddTest(tests, "ControlSpec/ToCommandsResolvesAndConverts", TestToCommandsResolvesAndConverts);
  AddTest(tests, "ControlSpec/ParseFunctionBreakpoints", TestParseFunctionBreakpoints);
  AddTest(tests, "ControlSpec/ParseFunctionBreakpointsRejectsBadInput",
          TestParseFunctionBreakpointsRejectsBadInput);
  AddTest(tests, "ControlSpec/ParseSettingsArrayForm", TestParseSettingsArrayForm);
  AddTest(tests, "ControlSpec/ParseSettingsObjectForm", TestParseSettingsObjectForm);
  AddTest(tests, "ControlSpec/ParseSettingsRejectsBadShape", TestParseSettingsRejectsBadShape);
}

}  // namespace microide::tests
