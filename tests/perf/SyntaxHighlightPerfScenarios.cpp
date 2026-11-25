#include "perf/PerfHarness.h"

#include <cstddef>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "editor/RuntimeSyntaxRegistry.h"
#include "editor/SyntaxHighlighter.h"

namespace microide::tests::perf {
namespace {

constexpr std::size_t kHighlightLineCount = 4000;

std::vector<std::string> ReadFixtureLines(const std::filesystem::path& path,
                                          std::size_t max_lines) {
  std::vector<std::string> lines;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return lines;
  }
  lines.reserve(max_lines);
  std::string line;
  while (lines.size() < max_lines && std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(line);
  }
  return lines;
}

void RunHighlightOverFixture(ScenarioContext& context,
                             std::string_view scenario_name,
                             const std::filesystem::path& fixture,
                             std::string_view phase_name) {
  const std::vector<std::string> lines = ReadFixtureLines(fixture, kHighlightLineCount);
  if (lines.size() < kHighlightLineCount) {
    std::cerr << scenario_name << ": missing or short fixture " << fixture << "\n";
    return;
  }
  const std::string_view first_line = lines.front();
  {
    editor::SyntaxState warm = editor::SyntaxHighlighter::InitialState(fixture, lines);
    for (std::size_t i = 0; i < 64; ++i) {
      warm = editor::SyntaxHighlighter::HighlightLine(lines[i], fixture, warm, first_line).end_state;
    }
  }

  std::size_t token_checksum = 0;
  context.Measure(phase_name, [&]() {
    editor::SyntaxState state = editor::SyntaxHighlighter::InitialState(fixture, lines);
    for (const std::string& line : lines) {
      editor::HighlightedLine highlighted =
          editor::SyntaxHighlighter::HighlightLine(line, fixture, state, first_line);
      state = highlighted.end_state;
      token_checksum += highlighted.tokens.size();
    }
  });
  if (token_checksum == 0) {
    std::cerr << scenario_name << ": highlighter produced no tokens\n";
  }
}

void RunSyntaxHighlightCppLines(ScenarioContext& context) {
  RunHighlightOverFixture(context, "syntax_highlight_cpp_lines",
                          "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp",
                          "syntax.highlight_cpp_lines");
}

void RunSyntaxHighlightPythonLines(ScenarioContext& context) {
  RunHighlightOverFixture(context, "syntax_highlight_python_lines",
                          "tests/perf/fixtures/editor_essentials_50k_py/synthetic_kernel.py",
                          "syntax.highlight_python_lines");
}

void RunSyntaxAdvanceStateCppLines(ScenarioContext& context) {
  const std::filesystem::path fixture =
      "tests/perf/fixtures/editor_essentials_50k_cpp/synthetic_kernel.cpp";
  const std::vector<std::string> lines = ReadFixtureLines(fixture, kHighlightLineCount);
  if (lines.size() < kHighlightLineCount) {
    std::cerr << "syntax_advance_state_cpp_lines: missing or short fixture " << fixture << "\n";
    return;
  }
  const std::string_view first_line = lines.front();
  {
    editor::SyntaxState warm = editor::SyntaxHighlighter::InitialState(fixture, lines);
    for (std::size_t i = 0; i < 64; ++i) {
      warm = editor::SyntaxHighlighter::AdvanceState(lines[i], fixture, warm, first_line);
    }
  }
  std::size_t depth_checksum = 0;
  context.Measure("syntax.advance_state_cpp_lines", [&]() {
    editor::SyntaxState state = editor::SyntaxHighlighter::InitialState(fixture, lines);
    for (const std::string& line : lines) {
      state = editor::SyntaxHighlighter::AdvanceState(line, fixture, state, first_line);
      depth_checksum += state.region_depth;
    }
  });
  (void)depth_checksum;
}

void RunSyntaxDefinitionsReloadNoPlugins(ScenarioContext& context) {
  editor::runtime_syntax::EnsureInitialized();
  const std::vector<editor::runtime_syntax::RuntimeSyntaxDefinitionData> no_definitions;
  (void)editor::runtime_syntax::ReloadDefinitions(no_definitions, nullptr);
  context.Measure("syntax.reload_no_plugins", [&]() {
    for (int iter = 0; iter < 20; ++iter) {
      const editor::runtime_syntax::RuntimeSyntaxReloadResult result =
          editor::runtime_syntax::ReloadDefinitions(no_definitions, nullptr);
      volatile std::size_t sink = result.built_in_definition_count;
      (void)sink;
    }
  });
}

const ScenarioRegistration g_perf_syntax_highlight_cpp_lines({Scenario{
    .name = "syntax_highlight_cpp_lines",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p95_percent = tolerance::kJitterWallP95,
    .tolerance_max_percent = tolerance::kJitterWallMax,
    .run = RunSyntaxHighlightCppLines,
}});
const ScenarioRegistration g_perf_syntax_highlight_python_lines({Scenario{
    .name = "syntax_highlight_python_lines",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p95_percent = tolerance::kJitterWallP95,
    .tolerance_max_percent = tolerance::kJitterWallMax,
    .run = RunSyntaxHighlightPythonLines,
}});
const ScenarioRegistration g_perf_syntax_definitions_reload_no_plugins({Scenario{
    .name = "syntax_definitions_reload_no_plugins",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p95_percent = tolerance::kJitterWallP95,
    .tolerance_max_percent = tolerance::kJitterWallMax,
    .run = RunSyntaxDefinitionsReloadNoPlugins,
}});
const ScenarioRegistration g_perf_syntax_advance_state_cpp_lines({Scenario{
    .name = "syntax_advance_state_cpp_lines",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p95_percent = tolerance::kJitterWallP95,
    .tolerance_max_percent = tolerance::kJitterWallMax,
    .run = RunSyntaxAdvanceStateCppLines,
}});

}
}
