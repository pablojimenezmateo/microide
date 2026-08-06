#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "perf/PerfHarness.h"

namespace microide::tests::perf {

// Run one scenario in a FRESH CHILD PROCESS and bring its Aggregate back.
//
// Every metric the harness records is a property of the process, not of the
// scenario, as long as all ~100 scenarios share one (TD-2026-08-06-152):
//
//  - resident growth read 33 KB per iteration for `diff_stage_selected_lines`
//    solo and 265 KB behind a 50-scenario prefix, with its actual retention
//    byte-identical in both, so the committed number was only comparable against
//    a run whose prefix was the same suite in the same order — adding, removing
//    or reordering a scenario silently re-levelled every downstream resident
//    gate, and the only signal was a red gate on unrelated code;
//  - allocation counts, the suite's deterministic oracle, carried a measured
//    680-allocation offset from process state alone (TD-2026-08-06-139), which
//    makes "deterministic" a claim about a fixed prefix rather than about the
//    scenario.
//
// A child per scenario makes each metric mean what its name says. The fork
// happens BEFORE the child touches SDL, the shell or any thread — the parent
// stays a pure driver, because fork in a multithreaded process is not safe — and
// the child `_exit`s after writing, so no atexit handler or destructor of the
// parent's runs twice.
//
// Returns nullopt when the scenario was not selected (same contract as
// `PerfHarness::RunScenario`) or when it failed; `error` is filled on failure,
// carrying the child's own harness error across the process boundary.
[[nodiscard]] bool ScenarioProcessIsolationAvailable();

[[nodiscard]] std::optional<Aggregate> RunScenarioInChildProcess(
    const Scenario& scenario, const PerfHarness::RunOptions& options, bool* selected,
    std::string* error);

// The wire codec, exposed so a test can assert the round trip is EXACT rather
// than merely plausible. Every double crosses the pipe as its bit pattern: a
// metric that changed in its last mantissa bit because it went through decimal
// text would read as a real move on a gate whose tolerance is a percentage of it.
[[nodiscard]] std::string EncodeAggregateForTesting(const Aggregate& aggregate);
[[nodiscard]] std::optional<Aggregate> DecodeAggregateForTesting(std::string_view bytes);

}  // namespace microide::tests::perf
