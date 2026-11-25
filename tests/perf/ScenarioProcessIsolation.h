#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "perf/PerfHarness.h"

namespace microide::tests::perf {

[[nodiscard]] bool ScenarioProcessIsolationAvailable();

[[nodiscard]] std::optional<Aggregate> RunScenarioInChildProcess(
    const Scenario& scenario, const PerfHarness::RunOptions& options, bool* selected,
    std::string* error);


}
