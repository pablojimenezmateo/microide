#include "perf/ScenarioAggregateWire.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace microide::tests::perf {

namespace {

// Length-prefixed little-endian binary, not JSON: doubles have to survive the
// pipe BIT-exactly. A metric that changes in its last mantissa bit because it
// round-tripped through decimal text would show up as a real move on a gate whose
// tolerance is a percentage of it, and the whole point of this transport is that
// the number the child measured is the number the parent gates.
class Writer {
 public:
  void U64(std::uint64_t value) {
    char bytes[8];
    for (int i = 0; i < 8; ++i) {
      bytes[i] = static_cast<char>((value >> (8 * i)) & 0xFFu);
    }
    out_.append(bytes, sizeof(bytes));
  }
  void I64(std::int64_t value) { U64(static_cast<std::uint64_t>(value)); }
  void F64(double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    U64(bits);
  }
  void Str(const std::string& value) {
    U64(value.size());
    out_.append(value);
  }
  const std::string& bytes() const { return out_; }

 private:
  std::string out_;
};

class Reader {
 public:
  explicit Reader(std::string_view bytes) : bytes_(bytes) {}
  bool ok() const { return ok_; }

  std::uint64_t U64() {
    if (!ok_ || offset_ + 8 > bytes_.size()) {
      ok_ = false;
      return 0;
    }
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
      value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes_[offset_ + i]))
               << (8 * i);
    }
    offset_ += 8;
    return value;
  }
  std::int64_t I64() { return static_cast<std::int64_t>(U64()); }
  double F64() {
    const std::uint64_t bits = U64();
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }
  std::string Str() {
    const std::uint64_t length = U64();
    // A length past the end can only mean a truncated or corrupt stream; refuse
    // rather than allocating whatever the bytes happened to say.
    if (!ok_ || length > bytes_.size() - offset_) {
      ok_ = false;
      return {};
    }
    std::string value(bytes_.substr(offset_, static_cast<std::size_t>(length)));
    offset_ += static_cast<std::size_t>(length);
    return value;
  }

 private:
  std::string_view bytes_;
  std::size_t offset_ = 0;
  bool ok_ = true;
};

void WriteMetricSnapshot(Writer& w, const MetricSnapshot& m) {
  w.F64(m.wall_ms);
  w.U64(m.allocations);
  w.U64(m.frees);
  w.U64(m.bytes_allocated);
  w.U64(m.bytes_freed);
  w.F64(m.cpu_ms);
  w.U64(m.cpu_calibration_ns);
  w.U64(m.rss_growth_bytes);
  w.I64(m.net_heap_bytes);
}

MetricSnapshot ReadMetricSnapshot(Reader& r) {
  MetricSnapshot m;
  m.wall_ms = r.F64();
  m.allocations = r.U64();
  m.frees = r.U64();
  m.bytes_allocated = r.U64();
  m.bytes_freed = r.U64();
  m.cpu_ms = r.F64();
  m.cpu_calibration_ns = r.U64();
  m.rss_growth_bytes = r.U64();
  m.net_heap_bytes = r.I64();
  return m;
}

void WriteMetricSet(Writer& w, const MetricSet& m) {
  for (double value : {m.p50_wall_ms, m.p95_wall_ms, m.max_wall_ms, m.p50_allocations,
                       m.p95_allocations, m.max_allocations, m.p50_cpu_ms, m.p95_cpu_ms,
                       m.max_cpu_ms, m.p50_rss_growth_bytes, m.p95_rss_growth_bytes,
                       m.max_rss_growth_bytes, m.mean_rss_growth_bytes, m.p50_net_heap_bytes,
                       m.p50_cpu_calibration_ns}) {
    w.F64(value);
  }
}

MetricSet ReadMetricSet(Reader& r) {
  MetricSet m;
  m.p50_wall_ms = r.F64();
  m.p95_wall_ms = r.F64();
  m.max_wall_ms = r.F64();
  m.p50_allocations = r.F64();
  m.p95_allocations = r.F64();
  m.max_allocations = r.F64();
  m.p50_cpu_ms = r.F64();
  m.p95_cpu_ms = r.F64();
  m.max_cpu_ms = r.F64();
  m.p50_rss_growth_bytes = r.F64();
  m.p95_rss_growth_bytes = r.F64();
  m.max_rss_growth_bytes = r.F64();
  m.mean_rss_growth_bytes = r.F64();
  m.p50_net_heap_bytes = r.F64();
  m.p50_cpu_calibration_ns = r.F64();
  return m;
}

}  // namespace

std::string EncodeScenarioAggregate(const Aggregate& aggregate) {
  Writer w;
  w.Str(aggregate.scenario_name);
  w.U64(aggregate.smoke ? 1u : 0u);
  // Carried across the fork like everything else: a skip declared in the child
  // has to reach the parent, which is the process that decides the run's verdict
  // (TD-2026-08-10-170). Without it an isolated run would silently regain the
  // vacuous pass the skip exists to prevent.
  w.Str(aggregate.skip_reason);
  // The child runs the scenario, so the child is the only process that knows
  // which revision of it ran. The parent gates.
  w.U64(aggregate.measurement_revision);
  WriteMetricSet(w, aggregate.metrics);
  w.U64(aggregate.iterations.size());
  for (const Iteration& iteration : aggregate.iterations) {
    w.U64(iteration.index);
    WriteMetricSnapshot(w, iteration.metrics);
    w.U64(iteration.phase_metrics.size());
    for (const Iteration::PhaseMetrics& phase : iteration.phase_metrics) {
      w.Str(phase.name);
      w.F64(phase.wall_ms);
      w.U64(phase.allocations);
      w.U64(phase.frees);
      w.U64(phase.bytes_allocated);
      w.U64(phase.bytes_freed);
    }
    w.U64(iteration.perf_counters.size());
    for (const auto& [name, value] : iteration.perf_counters) {
      w.Str(name);
      w.U64(value);
    }
  }
  w.U64(aggregate.phases.size());
  for (const PhaseMetricSet& phase : aggregate.phases) {
    w.Str(phase.name);
    w.F64(phase.p50_allocations);
    w.F64(phase.max_allocations);
    w.F64(phase.p50_wall_ms);
    w.U64(phase.iterations);
  }
  return w.bytes();
}

std::optional<Aggregate> DecodeScenarioAggregate(std::string_view bytes) {
  Reader r(bytes);
  Aggregate aggregate;
  aggregate.scenario_name = r.Str();
  aggregate.smoke = r.U64() != 0;
  aggregate.skip_reason = r.Str();
  aggregate.measurement_revision = static_cast<std::size_t>(r.U64());
  aggregate.metrics = ReadMetricSet(r);
  const std::uint64_t iteration_count = r.U64();
  if (!r.ok()) {
    return std::nullopt;
  }
  aggregate.iterations.reserve(static_cast<std::size_t>(iteration_count));
  for (std::uint64_t i = 0; i < iteration_count && r.ok(); ++i) {
    Iteration iteration;
    iteration.index = static_cast<std::size_t>(r.U64());
    iteration.metrics = ReadMetricSnapshot(r);
    const std::uint64_t phase_count = r.U64();
    for (std::uint64_t p = 0; p < phase_count && r.ok(); ++p) {
      Iteration::PhaseMetrics phase;
      phase.name = r.Str();
      phase.wall_ms = r.F64();
      phase.allocations = r.U64();
      phase.frees = r.U64();
      phase.bytes_allocated = r.U64();
      phase.bytes_freed = r.U64();
      iteration.phase_metrics.push_back(std::move(phase));
    }
    const std::uint64_t counter_count = r.U64();
    for (std::uint64_t c = 0; c < counter_count && r.ok(); ++c) {
      std::string name = r.Str();
      const std::uint64_t value = r.U64();
      iteration.perf_counters.emplace_back(std::move(name), value);
    }
    aggregate.iterations.push_back(std::move(iteration));
  }
  const std::uint64_t phase_set_count = r.U64();
  for (std::uint64_t p = 0; p < phase_set_count && r.ok(); ++p) {
    PhaseMetricSet phase;
    phase.name = r.Str();
    phase.p50_allocations = r.F64();
    phase.max_allocations = r.F64();
    phase.p50_wall_ms = r.F64();
    phase.iterations = static_cast<std::size_t>(r.U64());
    aggregate.phases.push_back(std::move(phase));
  }
  if (!r.ok()) {
    return std::nullopt;
  }
  return aggregate;
}

}  // namespace microide::tests::perf
