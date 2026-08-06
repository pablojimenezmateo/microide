#include "perf/ScenarioProcessIsolation.h"

#include "perf/AllocationCounter.h"

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#define MICROIDE_PERF_CAN_FORK 1
#else
#define MICROIDE_PERF_CAN_FORK 0
#endif

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

std::string EncodeAggregate(const Aggregate& aggregate) {
  Writer w;
  w.Str(aggregate.scenario_name);
  w.U64(aggregate.smoke ? 1u : 0u);
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

std::optional<Aggregate> DecodeAggregate(std::string_view bytes) {
  Reader r(bytes);
  Aggregate aggregate;
  aggregate.scenario_name = r.Str();
  aggregate.smoke = r.U64() != 0;
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

#if MICROIDE_PERF_CAN_FORK
bool WriteAll(int fd, std::string_view bytes) {
  while (!bytes.empty()) {
    const ssize_t written = ::write(fd, bytes.data(), bytes.size());
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    bytes.remove_prefix(static_cast<std::size_t>(written));
  }
  return true;
}

// Drain to EOF BEFORE waiting on the child. A pipe holds ~64 KiB, and a
// ten-iteration aggregate with per-iteration counters is comfortably past that,
// so waiting first deadlocks: the child blocks in write, the parent in waitpid.
bool ReadAll(int fd, std::string* out) {
  char buffer[16384];
  while (true) {
    const ssize_t got = ::read(fd, buffer, sizeof(buffer));
    if (got == 0) {
      return true;
    }
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    out->append(buffer, static_cast<std::size_t>(got));
  }
}
#endif

}  // namespace

std::string EncodeAggregateForTesting(const Aggregate& aggregate) {
  return EncodeAggregate(aggregate);
}

std::optional<Aggregate> DecodeAggregateForTesting(std::string_view bytes) {
  return DecodeAggregate(bytes);
}

bool ScenarioProcessIsolationAvailable() { return MICROIDE_PERF_CAN_FORK != 0; }

std::optional<Aggregate> RunScenarioInChildProcess(const Scenario& scenario,
                                                   const PerfHarness::RunOptions& options,
                                                   bool* selected, std::string* error) {
  const auto fail = [&](std::string message) -> std::optional<Aggregate> {
    if (error != nullptr) {
      *error = std::move(message);
    }
    return std::nullopt;
  };
  if (selected != nullptr) {
    *selected = true;
  }

#if !MICROIDE_PERF_CAN_FORK
  (void)scenario;
  (void)options;
  return fail("per-scenario process isolation is not available on this platform");
#else
  int fds[2] = {-1, -1};
  // CLOEXEC on the creating call, per the repository's descriptor rule: a
  // scenario spawns terminal shells, language servers and git, and an inherited
  // report pipe would keep the parent's read blocked past the child's exit.
  if (::pipe2(fds, O_CLOEXEC) != 0) {
    return fail(std::string("pipe2 failed: ") + std::strerror(errno));
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    const std::string message = std::string("fork failed: ") + std::strerror(errno);
    ::close(fds[0]);
    ::close(fds[1]);
    return fail(message);
  }

  if (pid == 0) {
    ::close(fds[0]);
    Writer header;
    std::string payload;
    // Everything below runs in a process that has never initialised SDL, the
    // shell, or a thread. `RunScenario` does all of that from scratch and tears
    // it down again before returning.
    const std::optional<Aggregate> aggregate = PerfHarness::RunScenario(scenario, options);
    if (aggregate.has_value()) {
      header.U64(1);
      payload = EncodeAggregate(*aggregate);
    } else {
      // Distinguish "not selected" (no error text) from a real failure, so the
      // parent can reproduce RunScenario's own contract exactly.
      header.U64(PerfHarness::LastError().empty() ? 2 : 0);
      Writer message;
      message.Str(std::string(PerfHarness::LastError()));
      payload = message.bytes();
    }
    // The allocation tracer's table lives in THIS address space, and `_exit` runs
    // no atexit handler, so the parent's end-of-main dump would print an empty
    // (and, with a phase filter, actively misleading) table. Dump here instead.
    Allocations::DumpTracedAllocationSites();
    const bool wrote = WriteAll(fds[1], header.bytes()) && WriteAll(fds[1], payload);
    ::close(fds[1]);
    // _exit, never exit: the parent's atexit handlers and static destructors are
    // in this address space too, and running them here would (at best) delete the
    // parent's artifacts and (at worst) flush its buffers twice.
    ::_exit(wrote ? 0 : 1);
  }

  ::close(fds[1]);
  std::string bytes;
  const bool read_ok = ReadAll(fds[0], &bytes);
  ::close(fds[0]);

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return fail(std::string("waitpid failed: ") + std::strerror(errno));
    }
  }

  if (!read_ok) {
    return fail("failed to read the scenario child's report");
  }
  if (WIFSIGNALED(status)) {
    return fail("scenario child died on signal " + std::to_string(WTERMSIG(status)));
  }
  if (bytes.size() < 8) {
    return fail("scenario child produced no report (exit status " +
                std::to_string(WEXITSTATUS(status)) + ")");
  }

  Reader header(std::string_view(bytes).substr(0, 8));
  const std::uint64_t kind = header.U64();
  const std::string_view body = std::string_view(bytes).substr(8);
  if (kind == 1) {
    std::optional<Aggregate> decoded = DecodeAggregate(body);
    if (!decoded.has_value()) {
      return fail("scenario child's report was truncated or malformed");
    }
    return decoded;
  }
  Reader message(body);
  const std::string text = message.Str();
  if (kind == 2) {
    // Not selected: the same nullopt-with-no-error contract RunScenario has.
    if (selected != nullptr) {
      *selected = false;
    }
    if (error != nullptr) {
      error->clear();
    }
    return std::nullopt;
  }
  return fail(text.empty() ? std::string("scenario failed in child process") : text);
#endif
}

}  // namespace microide::tests::perf
