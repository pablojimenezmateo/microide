#include "TestSupport.h"

#include "util/JsonValue.h"

#include <string>

namespace microide::tests {
namespace {

using microide::util::JsonValue;
using microide::util::ParseJson;
using microide::util::SerializeJson;

// A pathologically deep nesting must be rejected as a parse error rather than
// overflowing the native stack. This is the headless crash vector: the same
// ParseJson chokepoint is fed by --control-spec files, socket request lines, and
// instance descriptor files, so a depth guard here closes all of them at once.
void TestDeeplyNestedArrayRejected() {
  const int kDepth = 200000;
  std::string payload;
  payload.reserve(static_cast<std::size_t>(kDepth) * 2);
  payload.append(static_cast<std::size_t>(kDepth), '[');
  payload.append(static_cast<std::size_t>(kDepth), ']');

  const auto parsed = ParseJson(payload);
  Expect(!parsed.has_value(), "deeply nested array should reject, not crash");
}

void TestDeeplyNestedObjectRejected() {
  const int kDepth = 200000;
  std::string payload;
  payload.reserve(static_cast<std::size_t>(kDepth) * 8);
  for (int i = 0; i < kDepth; ++i) payload += "{\"a\":";
  payload += "1";
  payload.append(static_cast<std::size_t>(kDepth), '}');

  const auto parsed = ParseJson(payload);
  Expect(!parsed.has_value(), "deeply nested object should reject, not crash");
}

// Legitimate, modestly-nested payloads must still parse — the guard should only
// trip well beyond any real control/DAP/LSP message.
void TestModestNestingStillParses() {
  const int kDepth = 50;
  std::string payload;
  payload.append(static_cast<std::size_t>(kDepth), '[');
  payload += "42";
  payload.append(static_cast<std::size_t>(kDepth), ']');

  const auto parsed = ParseJson(payload);
  Expect(parsed.has_value(), "modest nesting within the limit should parse");

  // Walk down to confirm the structure round-tripped.
  const JsonValue* cursor = &*parsed;
  for (int i = 0; i < kDepth; ++i) {
    Expect(cursor->IsArray() && cursor->AsArray().size() == 1,
           "each level should be a single-element array");
    cursor = &cursor->AsArray()[0];
  }
  Expect(cursor->IsInt() && cursor->AsInt() == 42, "innermost value should be 42");
}

// Serialization must also be depth-bounded: a round-trip of a legal payload
// stays intact, and the serializer never recurses without limit.
void TestSerializeRoundTripsModestNesting() {
  const std::string payload = "[[[[[1]]]]]";
  const auto parsed = ParseJson(payload);
  Expect(parsed.has_value(), "small nested array should parse");
  Expect(SerializeJson(*parsed) == payload, "round-trip should preserve structure");
}

}  // namespace

void RegisterJsonValueTests(std::vector<TestCase>& tests) {
  AddTest(tests, "JsonValue/DeeplyNestedArrayRejected", TestDeeplyNestedArrayRejected);
  AddTest(tests, "JsonValue/DeeplyNestedObjectRejected", TestDeeplyNestedObjectRejected);
  AddTest(tests, "JsonValue/ModestNestingStillParses", TestModestNestingStillParses);
  AddTest(tests, "JsonValue/SerializeRoundTripsModestNesting",
          TestSerializeRoundTripsModestNesting);
}

}  // namespace microide::tests
