#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "perf/PerfHarness.h"

namespace microide::tests::perf {

[[nodiscard]] std::string EncodeScenarioAggregate(const Aggregate& aggregate);

[[nodiscard]] std::optional<Aggregate> DecodeScenarioAggregate(std::string_view bytes);

}
