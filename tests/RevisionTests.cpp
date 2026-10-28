#include "TestSupport.h"

#include <cstdint>
#include <limits>

#include "util/Revision.h"

namespace microide::tests {
namespace {

void TestRevisionStartsUnseenAndCounts() {
  util::Revision revision;
  Expect(revision.value() == 0, "a fresh revision is 0, the value a cache reads as never seen");
  revision.Bump();
  revision.Bump();
  Expect(revision.value() == 2, "each bump advances by one");
  Expect(util::Revision(1).value() == 1, "a revision can start past the unseen value");
}

void TestRevisionWrapsPastZero() {
  util::Revision revision(std::numeric_limits<std::uint64_t>::max());
  revision.Bump();
  Expect(revision.value() == 1,
         "a wrapping bump skips 0, so a cache stamped 0 cannot read the new state as seen");
  revision.Reset(7);
  Expect(revision.value() == 7, "reset sets the value");
}

}  // namespace

void RegisterRevisionTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Revision/StartsUnseenAndCounts", TestRevisionStartsUnseenAndCounts);
  AddTest(tests, "Revision/WrapsPastZero", TestRevisionWrapsPastZero);
}

}  // namespace microide::tests
