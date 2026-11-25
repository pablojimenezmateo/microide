#include "perf/PerfHarness.h"

#include <cstdlib>
#include <string_view>
#include <vector>

#include "util/Parse.h"

namespace microide::tests::perf {
namespace {

constexpr std::size_t kBaseBlocks = 2000;
constexpr std::size_t kBlockBytes = 4096;

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
  unsigned long long checksum = 0;
  for (std::size_t i = 0; i < blocks; ++i) {
    std::vector<unsigned char> block(kBlockBytes);
    block[i % kBlockBytes] = static_cast<unsigned char>(i & 0xFF);
    checksum += block[i % kBlockBytes];
  }
  static volatile unsigned long long sink = 0;
  sink += checksum;
}

const ScenarioRegistration g_perf_gate_canary({Scenario{
    .name = "perf_gate_canary",
    .smoke = false,
    .baseline_gated = true,
    .run_by_default = true,
    .tolerance_p50_percent = 100000.0,
    .tolerance_p95_percent = 100000.0,
    .tolerance_max_percent = 100000.0,
    .tolerance_alloc_p50_percent = tolerance::kExactAllocP50,
    .tolerance_alloc_p95_percent = tolerance::kExactAllocP95,
    .tolerance_alloc_max_percent = tolerance::kExactAllocMax,
    .run = RunPerfGateCanary,
}});

}
}
