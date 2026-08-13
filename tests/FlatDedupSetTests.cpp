#include "TestSupport.h"

#include <cstddef>
#include <string>
#include <string_view>
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

// Intern is the value-map half of the container: the id it returns must be the
// key's index in keys(), dense and stable, so a caller can hang a parallel
// vector off it (the compare path's occurrence counts).
void TestFlatDedupSetInternAssignsDenseFirstOccurrenceIds() {
  FlatDedupSet<std::string> ids(8);
  Expect(ids.Intern("b") == 0, "the first key gets id 0");
  Expect(ids.Intern("a") == 1, "the second distinct key gets the next id");
  Expect(ids.Intern("b") == 0, "a repeat returns the id it was given first");
  Expect(ids.Intern("c") == 2, "ids stay dense across repeats");
  Expect(ids.size() == 3, "a repeat stored nothing");
  for (std::size_t id = 0; id < ids.size(); ++id) {
    Expect(ids.Intern(ids.keys()[id]) == id, "every id indexes its own key in keys()");
  }
}

// Intern and Insert are the same probe. Growth past the caller's bound must not
// renumber anything a parallel vector is already indexed by.
void TestFlatDedupSetInternIdsSurviveGrowth() {
  FlatDedupSet<std::string> ids(2);
  constexpr std::size_t kCount = 3000;
  for (std::size_t i = 0; i < kCount; ++i) {
    Expect(ids.Intern("key_" + std::to_string(i)) == i, "each new key gets the next id");
  }
  for (std::size_t i = 0; i < kCount; ++i) {
    Expect(ids.Intern("key_" + std::to_string(i)) == i, "ids are unchanged by the rehashes");
  }
  Expect(ids.size() == kCount, "the second pass added nothing");
}

void TestFlatDedupSetInternAgreesWithInsert() {
  FlatDedupSet<std::string> interned(64);
  FlatDedupSet<std::string> inserted(64);
  for (std::size_t i = 0; i < 500; ++i) {
    const std::string key = "v" + std::to_string(i % 97);
    const std::size_t size_before = interned.size();
    const bool is_new = interned.Intern(key) == size_before;
    Expect(is_new == inserted.Insert(key), "Intern and Insert agree on what is new");
  }
  Expect(interned.keys() == inserted.keys(), "both paths store the same keys in the same order");
}

// TD-2026-08-13-199: FlatKeyIndex dedupes against strings the CALLER owns, so
// the index stores no keys at all. The load-bearing property is that it stays
// correct while the owning vector reallocates under it — which is exactly what
// rules out a string_view index (a short string lives inside its std::string).
void TestFlatKeyIndexDedupesWithoutStoringKeys() {
  std::vector<std::string> ids;
  auto index = util::FlatKeyIndex(
      4, [&ids](std::size_t i) -> std::string_view { return ids[i]; });

  const auto add = [&](std::string id) {
    if (index.Find(id) != util::kFlatKeyNotFound) {
      return false;
    }
    ids.push_back(std::move(id));
    index.Insert(ids.size() - 1);
    return true;
  };

  Expect(add("editor.wrap"), "a new id is appended");
  Expect(!add("editor.wrap"), "a repeat id is deduped");
  Expect(add("editor.tab_size"), "a second distinct id is appended");
  Expect(ids.size() == 2, "only distinct ids reach the owner");
  Expect(index.Find("editor.wrap") == 0, "the first id keeps its index");
  Expect(index.Find("editor.tab_size") == 1, "the second id keeps its index");
  Expect(index.Find("editor.absent") == util::kFlatKeyNotFound, "an unknown id is not found");
}

void TestFlatKeyIndexSurvivesOwnerReallocation() {
  std::vector<std::string> ids;
  // Seeded deliberately small so both the vector and the slot table grow several
  // times during the run: every short id moves with its std::string, and every
  // recorded index is rehashed through the accessor.
  auto index = util::FlatKeyIndex(
      2, [&ids](std::size_t i) -> std::string_view { return ids[i]; });

  constexpr std::size_t kCount = 500;
  for (std::size_t i = 0; i < kCount; ++i) {
    std::string id = "id." + std::to_string(i);  // short: lives in the string's inline buffer
    Expect(index.Find(id) == util::kFlatKeyNotFound, "each generated id is distinct");
    ids.push_back(std::move(id));
    index.Insert(ids.size() - 1);
  }
  Expect(ids.size() == kCount, "every distinct id was appended");
  for (std::size_t i = 0; i < kCount; ++i) {
    Expect(index.Find("id." + std::to_string(i)) == i,
           "every id still resolves to its index after growth");
  }
  Expect(index.Find("id." + std::to_string(kCount)) == util::kFlatKeyNotFound,
         "an id that was never inserted is still not found");
}

}  // namespace

void RegisterFlatDedupSetTests(std::vector<TestCase>& tests) {
  AddTest(tests, "FlatKeyIndex/DedupesWithoutStoringKeys",
          TestFlatKeyIndexDedupesWithoutStoringKeys);
  AddTest(tests, "FlatKeyIndex/SurvivesOwnerReallocation",
          TestFlatKeyIndexSurvivesOwnerReallocation);
  AddTest(tests, "FlatDedupSet/InternAssignsDenseFirstOccurrenceIds",
          TestFlatDedupSetInternAssignsDenseFirstOccurrenceIds);
  AddTest(tests, "FlatDedupSet/InternIdsSurviveGrowth", TestFlatDedupSetInternIdsSurviveGrowth);
  AddTest(tests, "FlatDedupSet/InternAgreesWithInsert", TestFlatDedupSetInternAgreesWithInsert);
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
