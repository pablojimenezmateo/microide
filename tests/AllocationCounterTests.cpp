#include "TestSupport.h"

#include <new>
#include <string>
#include <vector>

#include "perf/AllocationCounter.h"

namespace microide::tests {
namespace {

void TestAllocationCounterTracksDelta() {
#if MICROIDE_PERF_HARNESS_BUILD
  const perf::AllocationSnapshot before = perf::Allocations::Snapshot();
  void* data = ::operator new(static_cast<std::size_t>(4096));
  ::operator delete(data);
  const perf::AllocationDelta delta = perf::Allocations::DeltaSince(before);
  Expect(delta.allocations >= 1, "allocation counter should observe at least one allocation");
  Expect(delta.frees >= 1, "allocation counter should observe at least one free");
#else
  const perf::AllocationSnapshot before = perf::Allocations::Snapshot();
  void* data = ::operator new(static_cast<std::size_t>(4096));
  ::operator delete(data);
  const perf::AllocationDelta delta = perf::Allocations::DeltaSince(before);
  Expect(delta.allocations == 0 && delta.frees == 0,
         "allocation counter should remain disabled outside perf harness builds");
#endif
}

}  // namespace

void RegisterAllocationCounterTests(std::vector<TestCase>& tests) {
  AddTest(tests, "AllocationCounter/TracksDelta", TestAllocationCounterTracksDelta);
}

}  // namespace microide::tests
