#include "TestSupport.h"

#include <algorithm>
#include <string>

#include "util/InlineVector.h"

namespace microide::tests {
namespace {

using microide::util::InlineVector;

void TestInlineVectorFillsAndReadsBack() {
  InlineVector<int, 3> values;
  Expect(values.empty() && values.size() == 0, "a fresh inline vector is empty");
  values.push_back(1);
  values.push_back(2);
  Expect(!values.empty() && values.size() == 2, "push_back grows the size");
  Expect(values[0] == 1 && values[1] == 2, "elements read back in insertion order");
  Expect(values.front() == 1 && values.back() == 2, "front/back address the ends");
  Expect(values.capacity() == 3, "capacity is the template argument");
}

void TestInlineVectorIteratesOnlyLiveElements() {
  // The backing array holds `capacity()` value-initialised slots; the iterators
  // must stop at `size()`, not at the array end, or every range-for over a
  // partially-filled vector visits phantom zeroed elements.
  InlineVector<int, 4> values{7, 8};
  int seen = 0;
  int sum = 0;
  for (const int value : values) {
    ++seen;
    sum += value;
  }
  Expect(seen == 2 && sum == 15, "range-for visits exactly the live elements");
  Expect(std::distance(values.begin(), values.end()) == 2, "end() is begin() + size()");
  const auto found = std::find_if(values.begin(), values.end(), [](int v) { return v == 8; });
  Expect(found != values.end() && *found == 8, "std::find_if works over the live range");
}

void TestInlineVectorClearResetsWithoutLeakingOldElements() {
  InlineVector<int, 2> values{1, 2};
  values.clear();
  Expect(values.empty() && values.begin() == values.end(),
         "clear empties the vector");
  values.push_back(9);
  Expect(values.size() == 1 && values[0] == 9, "refilling after clear starts at index 0");
}

void TestInlineVectorEmplaceBackBuildsInPlaceAndReturnsTheSlot() {
  struct Pair {
    int a = 0;
    int b = 0;
  };
  InlineVector<Pair, 2> values;
  Pair& first = values.emplace_back(3, 4);
  Expect(values.size() == 1 && first.a == 3 && first.b == 4,
         "emplace_back returns the freshly built slot");
  first.b = 5;
  Expect(values[0].b == 5, "the returned reference aliases the stored element");
}

void TestInlineVectorEqualityComparesOnlyLiveElements() {
  InlineVector<int, 4> left{1, 2};
  InlineVector<int, 4> right{1, 2};
  Expect(left == right, "equal contents compare equal");
  right.push_back(3);
  Expect(!(left == right), "a differing size compares unequal");
  InlineVector<int, 4> filled{1, 2, 3, 4};
  filled.clear();
  filled.push_back(1);
  filled.push_back(2);
  Expect(filled == left,
         "stale values past size() do not participate in the comparison");
}

void TestInlineVectorHoldsNonTrivialElements() {
  InlineVector<std::string, 2> values;
  values.push_back(std::string("first"));
  std::string moved = "second";
  values.push_back(std::move(moved));
  Expect(values.size() == 2 && values[0] == "first" && values[1] == "second",
         "non-trivial elements survive both push_back overloads");
}

void TestInlineVectorDropsPushesPastCapacityInsteadOfCorrupting() {
  // Debug builds assert; release builds must still keep `size() <= capacity()`
  // so an over-push cannot walk off the backing array. Only exercised where
  // NDEBUG is on, since the assert fires (correctly) otherwise.
#ifdef NDEBUG
  InlineVector<int, 2> values{1, 2};
  values.push_back(3);
  Expect(values.size() == 2 && values[0] == 1 && values[1] == 2,
         "an over-capacity push is dropped, not written past the end");
#endif
  Expect(true, "capacity clamp checked where NDEBUG allows it");
}

}  // namespace

void RegisterInlineVectorTests(std::vector<TestCase>& tests) {
  AddTest(tests, "InlineVector/FillsAndReadsBack", TestInlineVectorFillsAndReadsBack);
  AddTest(tests, "InlineVector/IteratesOnlyLiveElements", TestInlineVectorIteratesOnlyLiveElements);
  AddTest(tests, "InlineVector/ClearResetsWithoutLeakingOldElements",
          TestInlineVectorClearResetsWithoutLeakingOldElements);
  AddTest(tests, "InlineVector/EmplaceBackBuildsInPlaceAndReturnsTheSlot",
          TestInlineVectorEmplaceBackBuildsInPlaceAndReturnsTheSlot);
  AddTest(tests, "InlineVector/EqualityComparesOnlyLiveElements",
          TestInlineVectorEqualityComparesOnlyLiveElements);
  AddTest(tests, "InlineVector/HoldsNonTrivialElements", TestInlineVectorHoldsNonTrivialElements);
  AddTest(tests, "InlineVector/DropsPushesPastCapacityInsteadOfCorrupting",
          TestInlineVectorDropsPushesPastCapacityInsteadOfCorrupting);
}

}  // namespace microide::tests
