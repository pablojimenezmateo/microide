#pragma once

#include <string>

namespace microide::tests::perf {

// Result of an affinity attempt, for the report metadata and the startup banner.
struct CpuAffinityPlan {
  // Human-readable description of what the process is pinned to, e.g.
  // "0-3,12-15 (8 cpus @ 5157 MHz)", "off", or "homogeneous (no pinning needed)".
  std::string description;
  // True when the process is measuring on a deliberately chosen CPU set. False
  // means the scheduler is free to move it, which on a heterogeneous machine is
  // a measurement variable the harness does not control.
  bool pinned = false;
};

// Pin the calling process to the fastest CPU cluster when the machine is
// heterogeneous.
//
// Hybrid CPUs (Intel P/E, AMD Zen5 + Zen5c) run the same code at very different
// speeds, and the scheduler picks. On perf-runner-v1 (Ryzen AI 9 HX 370: 8
// threads at 5.16 GHz, 16 at 3.29 GHz) that is worth up to 2.4x on a scenario:
// `debug_value_tree_paging` measured 3.18-4.31 ms pinned to the fast cluster and
// 7.63-7.76 ms pinned to the dense one, and unpinned it wandered across both --
// enough to fail its own baseline recorded minutes earlier, with byte-identical
// allocation counts. Wall gates cannot mean anything while that is unbounded, and
// widening every small scenario's tolerance to cover it would give up the signal
// instead of removing the noise.
//
// `request` is "auto" (pin to the fastest cluster, no-op on a homogeneous
// machine), "off", or an explicit CPU list like "0-3,12-15".
CpuAffinityPlan ApplyPerfCpuAffinity(const std::string& request);

}  // namespace microide::tests::perf
