#include "TestSupport.h"

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

#include "util/FlatDedupSet.h"

namespace microide::tests {
namespace {

using microide::util::FlatDedupSet;

void TestFlatDedupSetKeepsFirstOccurrenceOrder() {
  FlatDedupSet<std::string> seen(8);
  Expect(seen.Insert("b"), "a new key inserts");
  Expect(seen.Insert("a"), "a second new key inserts");
  Expect(!seen.Insert("b"), "a repeat of the first key is rejected");
  Expect(seen.Insert("c"), "a third new key inserts");
  Expect(!seen.Insert("a"), "a repeat of the second key is rejected");
  Expect(seen.size() == 3, "only distinct keys are stored");
  Expect(seen.keys() == std::vector<std::string>({"b", "a", "c"}),
         "keys read back in first-occurrence order, not hash order");
}

void TestFlatDedupSetContainsMatchesInsert() {
  FlatDedupSet<std::string> seen(4);
  Expect(!seen.Contains("x"), "an empty set contains nothing");
  seen.Insert("x");
  Expect(seen.Contains("x"), "an inserted key is found");
  Expect(!seen.Contains("y"), "a key that was never inserted is not found");
}

// The whole point of the container is that the table is sized once from the
// caller's bound. An UNDER-estimated bound must still be correct — it grows
// rather than filling up and probing forever — so exercise that path directly.
void TestFlatDedupSetGrowsPastAnUnderEstimatedBound() {
  FlatDedupSet<std::string> seen(2);
  constexpr std::size_t kCount = 5000;
  for (std::size_t i = 0; i < kCount; ++i) {
    Expect(seen.Insert("key_" + std::to_string(i)), "every distinct key inserts");
  }
  Expect(seen.size() == kCount, "growth preserved every key");
  for (std::size_t i = 0; i < kCount; ++i) {
    Expect(!seen.Insert("key_" + std::to_string(i)), "no key survives a rehash twice");
  }
  Expect(seen.size() == kCount, "the second pass added nothing");
  // Order survives growth: keys live in their own append-only vector, so a
  // rehash moves slots and never the key storage.
  Expect(seen.keys().front() == "key_0" && seen.keys().back() == "key_4999",
         "first-occurrence order survives the rehash");
}

// Colliding hashes must not merge distinct keys: the slot stores the hash, and a
// probe that matches on hash alone would fuse two different strings into one.
void TestFlatDedupSetDistinguishesKeysWithEqualHashes() {
  struct CollidingHash {
    std::size_t operator()(const std::string&) const { return 7; }
  };
  FlatDedupSet<std::string, CollidingHash> seen(16);
  Expect(seen.Insert("alpha"), "the first colliding key inserts");
  Expect(seen.Insert("beta"), "a different key with the same hash still inserts");
  Expect(!seen.Insert("alpha"), "the repeat is still rejected");
  Expect(seen.size() == 2, "equal hashes do not merge distinct keys");
}

// Agreement with the container it replaces, over a set with heavy duplication.
void TestFlatDedupSetAgreesWithUnorderedSet() {
  std::vector<std::string> input;
  for (std::size_t i = 0; i < 4000; ++i) {
    input.push_back("v" + std::to_string(i % 1300));
  }
  FlatDedupSet<std::string> flat(input.size());
  std::unordered_set<std::string> reference;
  std::vector<std::string> flat_order;
  std::vector<std::string> reference_order;
  for (const std::string& key : input) {
    if (flat.Insert(key)) {
      flat_order.push_back(key);
    }
    if (reference.insert(key).second) {
      reference_order.push_back(key);
    }
  }
  Expect(flat_order == reference_order,
         "the flat set accepts and rejects exactly what unordered_set does, in the same order");
  Expect(flat.keys() == reference_order, "the stored key order matches the accept order");
}

// A duplicate must not consume the caller's moved-from argument: RankedUnion
// passes a freshly built key by value and relies on the rejection path leaving
// nothing behind.
void TestFlatDedupSetRejectionDoesNotStoreTheKey() {
  FlatDedupSet<std::string> seen(8);
  std::string key = "shared-key-long-enough-to-heap-allocate";
  Expect(seen.Insert(key), "the first insert takes a copy");
  Expect(!seen.Insert(key), "the duplicate is rejected");
  Expect(seen.size() == 1 && seen.keys()[0] == "shared-key-long-enough-to-heap-allocate",
         "the rejected insert left the stored key intact");
}

}  // namespace

void RegisterFlatDedupSetTests(std::vector<TestCase>& tests) {
  AddTest(tests, "FlatDedupSet/KeepsFirstOccurrenceOrder",
          TestFlatDedupSetKeepsFirstOccurrenceOrder);
  AddTest(tests, "FlatDedupSet/ContainsMatchesInsert", TestFlatDedupSetContainsMatchesInsert);
  AddTest(tests, "FlatDedupSet/GrowsPastAnUnderEstimatedBound",
          TestFlatDedupSetGrowsPastAnUnderEstimatedBound);
  AddTest(tests, "FlatDedupSet/DistinguishesKeysWithEqualHashes",
          TestFlatDedupSetDistinguishesKeysWithEqualHashes);
  AddTest(tests, "FlatDedupSet/AgreesWithUnorderedSet", TestFlatDedupSetAgreesWithUnorderedSet);
  AddTest(tests, "FlatDedupSet/RejectionDoesNotStoreTheKey",
          TestFlatDedupSetRejectionDoesNotStoreTheKey);
}

}  // namespace microide::tests
