#pragma once

#include <string>

namespace microide::tests::perf {

struct CpuAffinityPlan {
  std::string description;
  bool pinned = false;
};

CpuAffinityPlan ApplyPerfCpuAffinity(const std::string& request);

}
