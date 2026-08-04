// Vacuity probe for the perf gate itself.
//
// Every other scenario in this directory asks "is the product still fast?". This
// one asks the prior question: "if the product got slower, would this harness
// actually say so?" Nothing answered that before. PerfBaselineTests covers
// CompareToBaseline with hand-written numbers, which proves the comparison
// arithmetic and nothing about the pipeline that feeds it — a scenario whose
// measurement, aggregation, baseline load, or exit code is broken reports a clean
// run, and a clean run is indistinguishable from a real pass.
//
// That is not a hypothetical failure mode here. The wall numbers on this suite
// have already been silently wrong twice: an xvfb-wrapped video lane inflated the
// frame-pumping scenarios 2-12x, and hybrid-CPU placement added another 2.4x.
// Both looked exactly like code regressions and both cost real sessions. The
// lesson from the architecture lint applies unchanged — a green run is evidence
// only if the thing producing it can be shown to go red. See
// dev-docs/project/validation-traps.md.
//
// How it is used: tools/run-checks.sh perf-canary runs this scenario twice, once
// clean and once with MICROIDE_PERF_CANARY_INFLATE set. The clean run must pass
// and the inflated run must FAIL. An inflated run that passes means the gate has
// stopped gating, and that check is what makes it loud.
//
// Design constraints that keep the committed baseline portable to any machine,
// including a CI runner that is not perf-runner-v1:
//
//   - The workload's allocation count is fixed by THIS file, not by the standard
//     library: one std::vector construction per block, so exactly kBaseBlocks
//     allocations per iteration. No container growth policy, no reserve
//     heuristics, nothing version-dependent. Allocation counts are the repo's
//     deterministic oracle (see Scenario in PerfHarness.h) and this scenario is
//     built so they stay exactly that.
//   - The wall/CPU envelopes in the committed baseline are deliberately enormous.
//     Wall time here is machine-specific and irrelevant: the canary's job is to
//     prove the ALLOCATION gate fires, so wall is left effectively ungated rather
//     than made into a cross-machine flake. The committed baseline also carries no
//     cpu/rss metrics, which leaves those ungated by the has_*_metrics rule in
//     Baseline.h.
//
// Inflating by N multiplies the allocation count by exactly N. The default
// alloc_p50 tolerance is 10%, so any N >= 2 must trip it by a wide margin. The
// check uses 4.

#include "perf/PerfHarness.h"

#include <cstdlib>
#include <string_view>
#include <vector>

#include "util/Parse.h"

namespace microide::tests::perf {
namespace {

// One allocation each, sized so the zero-fill is real work rather than noise.
constexpr std::size_t kBaseBlocks = 2000;
constexpr std::size_t kBlockBytes = 4096;

// Multiplier applied to the workload, read once per process from
// MICROIDE_PERF_CANARY_INFLATE. 1 (the default, and the value the committed
// baseline was recorded at) means "measure the honest workload".
//
// Resolved through util::ParseSize per the repo's numeric-parsing invariant, and
// clamped: a typo'd value must not turn the canary into a multi-minute hang. A
// value that does not parse falls back to 1, which fails safe — the inflated run
// then measures the clean workload, passes the gate, and the check reports the
// gate as vacuous rather than quietly skipping itself.
std::size_t InflationFactor() {
  static const std::size_t factor = [] {
    const char* raw = std::getenv("MICROIDE_PERF_CANARY_INFLATE");
    if (raw == nullptr) {
      return std::size_t{1};
    }
    const auto parsed = util::ParseSize(std::string_view(raw));
    if (!parsed.has_value() || *parsed < 1) {
      return std::size_t{1};
    }
    return *parsed > 64 ? std::size_t{64} : *parsed;
  }();
  return factor;
}

void RunPerfGateCanary(ScenarioContext&) {
  const std::size_t blocks = kBaseBlocks * InflationFactor();
  // Accumulated and consumed below so the loop cannot be optimized away; the
  // vector is constructed inside the loop so each pass is exactly one
  // allocation and one free.
  unsigned long long checksum = 0;
  for (std::size_t i = 0; i < blocks; ++i) {
    std::vector<unsigned char> block(kBlockBytes);
    block[i % kBlockBytes] = static_cast<unsigned char>(i & 0xFF);
    checksum += block[i % kBlockBytes];
  }
  // Consume the result. A volatile sink is the portable way to keep the compiler
  // from deleting the whole loop as dead; without it the canary could measure
  // nothing at all and still "pass", which is the exact failure it exists to catch.
  // `+=` rather than `=` on purpose: a volatile that is only ever written is a
  // set-but-not-used variable, which -Wunused-but-set-variable rejects under the
  // -Werror lane added alongside this file.
  static volatile unsigned long long sink = 0;
  sink += checksum;
}

const ScenarioRegistration g_perf_gate_canary({Scenario{
    .name = "perf_gate_canary",
    // Not a smoke scenario: this measures the harness, not the product, so it has
    // no place in the quick pre-flight set.
    .smoke = false,
    .baseline_gated = true,
    // Runs in the full suite. It is cheap, and a canary that only runs when
    // someone remembers to ask for it is the thing it was written to prevent.
    .run_by_default = true,
    // Wall and CPU are intentionally wide open (see the header comment): this
    // scenario gates on allocations, which are exact. These are percentages, so
    // 100000 means "100x slower is still not a failure" — wall regressions in a
    // synthetic memset loop say nothing about the product.
    .tolerance_p50_percent = 100000.0,
    .tolerance_p95_percent = 100000.0,
    .tolerance_max_percent = 100000.0,
    .tolerance_alloc_p50_percent = tolerance::kExactAllocP50,
    .tolerance_alloc_p95_percent = tolerance::kExactAllocP95,
    .tolerance_alloc_max_percent = tolerance::kExactAllocMax,
    .run = RunPerfGateCanary,
}});

}  // namespace
}  // namespace microide::tests::perf
