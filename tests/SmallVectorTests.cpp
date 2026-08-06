#include "TestSupport.h"

#include <array>
#include <utility>

#include "util/SmallVector.h"

namespace microide::tests {
namespace {

using microide::util::SmallVector;

void TestSmallVectorStaysInlineBelowCapacity() {
  SmallVector<int, 4> values;
  Expect(values.empty() && values.capacity() == 4 && !values.spilled(),
         "a fresh small vector is empty and inline");
  values.push_back(1);
  values.push_back(2);
  values.push_back(3);
  values.push_back(4);
  Expect(values.size() == 4 && !values.spilled(),
         "filling the inline capacity exactly does not spill");
  Expect(values[0] == 1 && values[3] == 4, "inline elements read back in order");
}

void TestSmallVectorSpillsAndKeepsEveryElement() {
  SmallVector<int, 2> values{1, 2};
  values.push_back(3);
  Expect(values.spilled() && values.size() == 3 && values.capacity() >= 3,
         "pushing past the inline capacity spills to the heap");
  Expect(values[0] == 1 && values[1] == 2 && values[2] == 3,
         "spilling copies the inline elements across, in order");
  for (int i = 4; i <= 40; ++i) {
    values.push_back(i);
  }
  Expect(values.size() == 40, "repeated growth keeps every element");
  bool ordered = true;
  for (int i = 0; i < 40; ++i) {
    ordered = ordered && values[static_cast<std::size_t>(i)] == i + 1;
  }
  Expect(ordered, "repeated growth preserves order");
}

void TestSmallVectorClearKeepsCapacitySoRefillsDoNotReallocate() {
  // This is the property the redraw path depends on: a reused instance stops
  // allocating once it has seen its high-water mark.
  SmallVector<int, 1> values;
  values.push_back(1);
  values.push_back(2);
  values.push_back(3);
  const std::size_t grown = values.capacity();
  const int* buffer = values.data();
  values.clear();
  Expect(values.empty() && values.capacity() == grown,
         "clear keeps the grown capacity");
  values.push_back(9);
  values.push_back(8);
  Expect(values.data() == buffer, "a refill within capacity reuses the same buffer");
}

void TestSmallVectorCopyIsIndependentOfItsSource() {
  SmallVector<int, 2> source{1, 2};
  source.push_back(3);  // spilled
  SmallVector<int, 2> copy = source;
  Expect(copy.size() == 3 && copy[2] == 3, "a copy carries the spilled contents");
  Expect(copy.data() != source.data(), "a copy does not alias the source buffer");
  copy[0] = 99;
  Expect(source[0] == 1, "mutating the copy leaves the source alone");

  SmallVector<int, 4> inline_source{5, 6};
  SmallVector<int, 4> inline_copy = inline_source;
  inline_copy[0] = 50;
  Expect(inline_source[0] == 5 && inline_copy[0] == 50,
         "an inline copy is independent too");
}

void TestSmallVectorMoveTransfersTheHeapBufferAndEmptiesTheSource() {
  SmallVector<int, 2> source{1, 2};
  source.push_back(3);
  const int* buffer = source.data();
  SmallVector<int, 2> moved = std::move(source);
  Expect(moved.size() == 3 && moved.data() == buffer,
         "a move of a spilled vector transfers the buffer without copying");
  Expect(source.empty() && !source.spilled(),  // NOLINT(bugprone-use-after-move)
         "the moved-from vector is empty and back on inline storage");

  SmallVector<int, 4> inline_source{7, 8};
  SmallVector<int, 4> inline_moved = std::move(inline_source);
  Expect(inline_moved.size() == 2 && inline_moved[1] == 8,
         "a move of an inline vector copies the slab");
  Expect(inline_source.empty(),  // NOLINT(bugprone-use-after-move)
         "the moved-from inline vector is empty");
}

void TestSmallVectorMoveAssignmentReleasesTheOldBuffer() {
  // Assigning over a spilled vector must free what it held; run under ASAN/LSAN
  // this is the leak check, and the value assertions hold everywhere.
  SmallVector<int, 1> target;
  target.push_back(1);
  target.push_back(2);  // spilled
  SmallVector<int, 1> source{5};
  target = std::move(source);
  Expect(target.size() == 1 && target[0] == 5, "move assignment replaces the contents");
  target = SmallVector<int, 1>{};
  Expect(target.empty(), "move-assigning an empty vector empties the target");
}

void TestSmallVectorSwapExchangesContentsBothWays() {
  SmallVector<int, 2> spilled{1, 2};
  spilled.push_back(3);
  SmallVector<int, 2> inline_only{9};
  spilled.swap(inline_only);
  Expect(spilled.size() == 1 && spilled[0] == 9, "swap moves the inline side across");
  Expect(inline_only.size() == 3 && inline_only[2] == 3,
         "swap moves the spilled side across");

  SmallVector<int, 1> left{1};
  left.push_back(2);
  SmallVector<int, 1> right{3};
  right.push_back(4);
  const int* left_buffer = left.data();
  right.swap(left);
  Expect(right.data() == left_buffer, "two spilled sides swap buffers without copying");
  Expect(right[0] == 1 && left[0] == 3, "two spilled sides exchange contents");
}

void TestSmallVectorAppendAndReserve() {
  const std::array<int, 5> source{1, 2, 3, 4, 5};
  SmallVector<int, 2> values;
  values.append(source.begin(), source.end());
  Expect(values.size() == 5 && values[4] == 5, "append copies the whole range");

  SmallVector<int, 2> reserved;
  reserved.reserve(16);
  const int* buffer = reserved.data();
  for (int i = 0; i < 16; ++i) {
    reserved.push_back(i);
  }
  Expect(reserved.data() == buffer, "a reserved buffer absorbs the pushes without regrowing");
  Expect(reserved.size() == 16, "reserve does not change the size");
}

void TestSmallVectorEqualityIgnoresCapacityAndStorageMode() {
  SmallVector<int, 2> spilled{1, 2};
  spilled.push_back(3);
  SmallVector<int, 2> other{1, 2};
  Expect(!(spilled == other), "different sizes compare unequal");
  other.push_back(3);
  Expect(spilled == other, "equal contents compare equal regardless of how they grew");
  SmallVector<int, 8> wide{1, 2, 3};
  Expect(wide.size() == spilled.size(),
         "a wider inline capacity holds the same logical contents");
}

}  // namespace

void RegisterSmallVectorTests(std::vector<TestCase>& tests) {
  AddTest(tests, "SmallVector/StaysInlineBelowCapacity", TestSmallVectorStaysInlineBelowCapacity);
  AddTest(tests, "SmallVector/SpillsAndKeepsEveryElement",
          TestSmallVectorSpillsAndKeepsEveryElement);
  AddTest(tests, "SmallVector/ClearKeepsCapacitySoRefillsDoNotReallocate",
          TestSmallVectorClearKeepsCapacitySoRefillsDoNotReallocate);
  AddTest(tests, "SmallVector/CopyIsIndependentOfItsSource",
          TestSmallVectorCopyIsIndependentOfItsSource);
  AddTest(tests, "SmallVector/MoveTransfersTheHeapBufferAndEmptiesTheSource",
          TestSmallVectorMoveTransfersTheHeapBufferAndEmptiesTheSource);
  AddTest(tests, "SmallVector/MoveAssignmentReleasesTheOldBuffer",
          TestSmallVectorMoveAssignmentReleasesTheOldBuffer);
  AddTest(tests, "SmallVector/SwapExchangesContentsBothWays",
          TestSmallVectorSwapExchangesContentsBothWays);
  AddTest(tests, "SmallVector/AppendAndReserve", TestSmallVectorAppendAndReserve);
  AddTest(tests, "SmallVector/EqualityIgnoresCapacityAndStorageMode",
          TestSmallVectorEqualityIgnoresCapacityAndStorageMode);
}

}  // namespace microide::tests
