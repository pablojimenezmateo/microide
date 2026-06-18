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

}  // namespace

void RegisterControlSpecTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ControlSpec/ParseFullSpec", TestParseFullSpec);
  AddTest(tests, "ControlSpec/ParseRejectsBadInput", TestParseRejectsBadInput);
  AddTest(tests, "ControlSpec/ToCommandsResolvesAndConverts", TestToCommandsResolvesAndConverts);
}

}  // namespace microide::tests
