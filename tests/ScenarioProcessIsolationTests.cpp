#include "TestSupport.h"

#include "perf/ScenarioAggregateWire.h"

#include <cmath>
#include <cstring>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::tests::perf::Aggregate;
using microide::tests::perf::DecodeScenarioAggregate;
using microide::tests::perf::EncodeScenarioAggregate;
using microide::tests::perf::Iteration;
using microide::tests::perf::MetricSet;
using microide::tests::perf::PhaseMetricSet;

// TD-2026-08-06-152: a scenario's Aggregate now crosses a process boundary before
// it is gated, so the transport has to be EXACT. A gate compares a measured double
// against a baseline double with a percentage tolerance; a value that lost its last
// mantissa bit on the way back would read as a real move, and one that gained a
// digit would read as none.
//
// The fixture uses values chosen to break a lazy codec rather than round numbers:
// a double with a full mantissa, a negative one (net_heap_bytes legitimately is),
// the smallest normal, values that decimal text cannot represent, a 64-bit counter
// at its maximum, an empty string, and a name with an embedded NUL.
Aggregate MakeWireFixture() {
  Aggregate aggregate;
  aggregate.scenario_name = "wire_fixture";
  aggregate.smoke = true;
  // The child is the only process that knows which revision of the scenario ran,
  // and the parent is the one that gates. Not 1: a default that survives by
  // accident proves nothing (TD-2026-08-07-167).
  aggregate.measurement_revision = 7;
  aggregate.metrics = MetricSet{
      .p50_wall_ms = 0.1 + 0.2,  // 0.30000000000000004, not 0.3
      .p95_wall_ms = 1.0 / 3.0,
      .max_wall_ms = std::numeric_limits<double>::min(),
      .p50_allocations = 9007199254740993.0,  // past exact integer range
      .p95_allocations = -0.0,
      .max_allocations = 1e308,
      .p50_cpu_ms = 4.9406564584124654e-324,  // denormal
      .p95_cpu_ms = 2.2250738585072014e-308,
      .max_cpu_ms = -1.7976931348623157e308,
      .p50_rss_growth_bytes = 123456.789,
      .p95_rss_growth_bytes = 0.0,
      .max_rss_growth_bytes = 3.0,
      .mean_rss_growth_bytes = 1.0 / 7.0,
      .p50_net_heap_bytes = -28470.0,
      .p50_cpu_calibration_ns = 485793.5,
  };

  Iteration first;
  first.index = 0;
  first.metrics.wall_ms = 22.265300000000003;
  first.metrics.allocations = std::numeric_limits<std::uint64_t>::max();
  first.metrics.frees = 0;
  first.metrics.bytes_allocated = 1;
  first.metrics.bytes_freed = 2;
  first.metrics.cpu_ms = 1.0 / 3.0;
  first.metrics.cpu_calibration_ns = 485793;
  first.metrics.rss_growth_bytes = 4096;
  // Signed and legitimately negative: a scenario whose setup allocates and whose
  // measured window releases reports below zero.
  first.metrics.net_heap_bytes = -141047;
  first.phase_metrics.push_back(Iteration::PhaseMetrics{
      .name = std::string("phase\0with-nul", 14),
      .wall_ms = 0.1 + 0.2,
      .allocations = 89044,
      .frees = 89043,
      .bytes_allocated = 33169920,
      .bytes_freed = 33169919,
  });
  first.phase_metrics.push_back(Iteration::PhaseMetrics{.name = ""});
  first.perf_counters.emplace_back("document.edits", 1);
  first.perf_counters.emplace_back("editor.multi_range_undo_lines_spanned",
                                   std::numeric_limits<std::uint64_t>::max());

  Iteration second;
  second.index = 1;
  second.metrics.wall_ms = 0.0;

  aggregate.iterations.push_back(std::move(first));
  aggregate.iterations.push_back(std::move(second));
  aggregate.phases.push_back(PhaseMetricSet{.name = "phase.one",
                                            .p50_allocations = 1.0 / 3.0,
                                            .max_allocations = 1e308,
                                            .p50_wall_ms = 0.1 + 0.2,
                                            .iterations = 10});
  return aggregate;
}

bool SameDouble(double a, double b) {
  // Bit equality, not approximate equality: that is the whole claim.
  return std::memcmp(&a, &b, sizeof(double)) == 0;
}

void TestScenarioAggregateSurvivesTheWireExactly() {
  const Aggregate original = MakeWireFixture();
  const std::string bytes = EncodeScenarioAggregate(original);
  const auto decoded = DecodeScenarioAggregate(bytes);
  Expect(decoded.has_value(), "a well-formed aggregate must decode");

  Expect(decoded->scenario_name == original.scenario_name, "scenario name round-trips");
  Expect(decoded->smoke == original.smoke, "smoke flag round-trips");
  Expect(decoded->measurement_revision == original.measurement_revision,
         "measurement revision round-trips");

  const auto& m = original.metrics;
  const auto& d = decoded->metrics;
  const std::pair<double, double> metric_pairs[] = {
      {m.p50_wall_ms, d.p50_wall_ms},
      {m.p95_wall_ms, d.p95_wall_ms},
      {m.max_wall_ms, d.max_wall_ms},
      {m.p50_allocations, d.p50_allocations},
      {m.p95_allocations, d.p95_allocations},
      {m.max_allocations, d.max_allocations},
      {m.p50_cpu_ms, d.p50_cpu_ms},
      {m.p95_cpu_ms, d.p95_cpu_ms},
      {m.max_cpu_ms, d.max_cpu_ms},
      {m.p50_rss_growth_bytes, d.p50_rss_growth_bytes},
      {m.p95_rss_growth_bytes, d.p95_rss_growth_bytes},
      {m.max_rss_growth_bytes, d.max_rss_growth_bytes},
      {m.mean_rss_growth_bytes, d.mean_rss_growth_bytes},
      {m.p50_net_heap_bytes, d.p50_net_heap_bytes},
      {m.p50_cpu_calibration_ns, d.p50_cpu_calibration_ns},
  };
  for (std::size_t i = 0; i < std::size(metric_pairs); ++i) {
    Expect(SameDouble(metric_pairs[i].first, metric_pairs[i].second),
           "metric " + std::to_string(i) + " must survive the wire bit-for-bit");
  }
  // -0.0 must stay -0.0: `==` would not notice, which is why the check is memcmp.
  Expect(std::signbit(decoded->metrics.p95_allocations),
         "negative zero keeps its sign across the wire");

  Expect(decoded->iterations.size() == original.iterations.size(), "iteration count round-trips");
  const Iteration& oi = original.iterations[0];
  const Iteration& di = decoded->iterations[0];
  Expect(di.index == oi.index, "iteration index round-trips");
  Expect(di.metrics.allocations == oi.metrics.allocations,
         "a uint64 at its maximum round-trips");
  Expect(di.metrics.net_heap_bytes == oi.metrics.net_heap_bytes,
         "a NEGATIVE net_heap_bytes round-trips (unsigned round-tripping would clamp it)");
  Expect(SameDouble(di.metrics.wall_ms, oi.metrics.wall_ms), "iteration wall round-trips");
  Expect(SameDouble(di.metrics.cpu_ms, oi.metrics.cpu_ms), "iteration cpu round-trips");

  Expect(di.phase_metrics.size() == oi.phase_metrics.size(), "phase metrics count round-trips");
  Expect(di.phase_metrics[0].name == oi.phase_metrics[0].name,
         "a phase name containing a NUL survives (a C-string codec would truncate it)");
  Expect(di.phase_metrics[0].allocations == oi.phase_metrics[0].allocations,
         "phase allocations round-trip");
  Expect(di.phase_metrics[1].name.empty(), "an empty phase name stays empty, not missing");

  Expect(di.perf_counters.size() == oi.perf_counters.size(), "counter count round-trips");
  Expect(di.perf_counters[1].first == oi.perf_counters[1].first &&
             di.perf_counters[1].second == oi.perf_counters[1].second,
         "a counter at uint64 max round-trips");

  Expect(decoded->phases.size() == 1 && decoded->phases[0].name == "phase.one" &&
             decoded->phases[0].iterations == 10 &&
             SameDouble(decoded->phases[0].p50_allocations, 1.0 / 3.0),
         "the aggregated phase set round-trips");
}

// A truncated or corrupt stream must be REFUSED, not silently decoded into a
// half-populated Aggregate — which would be gated as if it were a measurement.
void TestScenarioAggregateRejectsATruncatedStream() {
  const std::string bytes = EncodeScenarioAggregate(MakeWireFixture());
  Expect(!bytes.empty(), "the fixture must encode to something");
  for (std::size_t cut = 0; cut < bytes.size(); cut += 7) {
    const auto decoded = DecodeScenarioAggregate(std::string_view(bytes).substr(0, cut));
    Expect(!decoded.has_value(),
           "a stream truncated at " + std::to_string(cut) + " bytes must be refused");
  }
  Expect(DecodeScenarioAggregate(bytes).has_value(),
         "the untruncated stream must still decode, or the loop above proves nothing");

  // A length field claiming more than the stream holds must not be trusted.
  std::string corrupt = bytes;
  for (int i = 0; i < 8; ++i) {
    corrupt[static_cast<std::size_t>(i)] = static_cast<char>(0xFF);
  }
  Expect(!DecodeScenarioAggregate(corrupt).has_value(),
         "a string length past the end of the stream must be refused");
}

}  // namespace

void RegisterScenarioProcessIsolationTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ScenarioProcessIsolation/AggregateSurvivesTheWireExactly",
          TestScenarioAggregateSurvivesTheWireExactly);
  AddTest(tests, "ScenarioProcessIsolation/AggregateRejectsATruncatedStream",
          TestScenarioAggregateRejectsATruncatedStream);
}

}  // namespace microide::tests
