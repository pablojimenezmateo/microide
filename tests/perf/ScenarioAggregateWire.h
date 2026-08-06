#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "perf/PerfHarness.h"

namespace microide::tests::perf {

// The wire form of a scenario's `Aggregate`, used to bring it back from the child
// process each scenario now runs in (TD-2026-08-06-152).
//
// Length-prefixed little-endian binary rather than JSON, because a gate compares
// doubles against doubles with a percentage tolerance: a value that lost its last
// mantissa bit round-tripping through decimal text would read as a real move, and
// the whole point of this transport is that the number the child measured is the
// number the parent gates.
//
// Deliberately a separate translation unit from the fork/exec side: this half has
// no dependency on `PerfHarness` at all, which is what lets `microide_tests` link
// it and assert the round trip without pulling in the SDL driver and every
// registered scenario.
[[nodiscard]] std::string EncodeScenarioAggregate(const Aggregate& aggregate);

// nullopt when the stream is truncated or malformed. Never a half-populated
// Aggregate: that would be gated as if it were a measurement.
[[nodiscard]] std::optional<Aggregate> DecodeScenarioAggregate(std::string_view bytes);

}  // namespace microide::tests::perf
