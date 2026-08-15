// Performance scenarios for the per-line syntax-highlight hot path.
//
// `runtime_syntax::HighlightLine` is the dominant main-thread cost of scrolling
// a highlighted file through fresh content: every newly exposed line is a token
// cache miss, and the render path resolves it synchronously (the off-thread
// prefetch cannot land inside the same frame that scrolled). The ranked scope
// summary put it at the top of `editor_sticky_scroll_scroll`, yet the function
// had no scenario of its own -- the interactive scenarios that exercise it also
// pay file open, layout, render and present, so a 2x change in the highlighter
// moved them only a few percent and no gate would have caught a regression.
//
// These are pure-unit micro-benchmarks: they read a fixture buffer once and call
// the highlighter directly, threading `SyntaxState` line to line exactly as the
// viewport's cache does. Deterministic and allocation-stable, so they are gated.
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

// Lines highlighted per measured pass. Well above a screenful so per-call
// overhead is averaged out, and small enough that a pass stays in the tens of
// milliseconds on the reference runner.
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

// Highlight `lines` in order, threading the end state forward the way
// TextViewport's per-line cache does on a cold scroll.
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
  // Warm the lazy per-definition regex compile outside the measured window; the
  // production path pays it once per filetype per session, not per line.
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
      // Consume the tokens so the whole call cannot be optimized away.
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

// The state-only replay the checkpoint chain runs (want_tokens = false). It skips
// the pattern rules entirely and only tracks region open/close, so it must stay
// far cheaper than the token path -- a regression that made it re-run pattern
// rules would be invisible in the token scenarios above.
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

// A reload with NO plugin definitions must ALIAS the built-in registry, not copy
// it. `GetRegistry()` returns `BuiltInRegistry()` exactly when `MutableRegistry()`
// is empty, so leaving it empty is the whole mechanism — and for a long time
// `BuildRegistry` cloned the built-in tables into it anyway, which silently
// disabled the alias and cost 1.7 ms of shell thread at every launch and at every
// project switch whose syntax fingerprint moved.
//
// A pure-unit gate, because nothing else can see this: no interactive scenario
// reloads syntax definitions (the fingerprint check early-returns after the
// first), and the launch cost lives in the real binary, outside the harness. The
// allocation count is the oracle — a re-introduced clone is ~3,600 rules and two
// rule-index maps per definition, so it moves this from tens to tens of
// thousands. The wall half is incidental.
void RunSyntaxDefinitionsReloadNoPlugins(ScenarioContext& context) {
  editor::runtime_syntax::EnsureInitialized();
  const std::vector<editor::runtime_syntax::RuntimeSyntaxDefinitionData> no_definitions;
  // Warm: the first reload after EnsureInitialized may still be publishing the
  // built-in registry, and the measured loop wants the steady state.
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

// Wall envelopes follow the tech-debt coverage precedent: allocation counts are
// exactly deterministic here (one token vector per line) and stay the tight
// complexity oracle, while the wall envelope absorbs this runner's scheduler
// jitter on tens-of-milliseconds work.
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

}  // namespace
}  // namespace microide::tests::perf
