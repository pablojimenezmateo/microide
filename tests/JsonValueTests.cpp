#include "TestSupport.h"

#include "util/JsonValue.h"
#include "util/StringUtil.h"

#include <cmath>
#include <limits>
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

// An integer literal that overflows int64 (a realistic uint64-range id/handle/
// address from a DAP adapter or LSP server) must not abort the entire message
// parse; it falls back to a lossy double so the surrounding value still resolves.
void TestOutOfRangeIntegerFallsBackToDouble() {
  const auto parsed = ParseJson("{\"id\":9223372036854775808}");  // 2^63
  Expect(parsed.has_value(), "uint64-range integer must not reject the whole document");
  const JsonValue& id = (*parsed)["id"];
  Expect(id.IsDouble(), "over-range integer should decode as a double");
  Expect(id.AsDouble() > 9.2e18, "double fallback should preserve rough magnitude");
}

// A lone low surrogate must be rejected, not emitted as invalid (CESU-8) UTF-8.
// This mirrors the parser's existing rejection of a lone high surrogate and keeps
// every host string satisfying the valid-UTF-8 invariant.
void TestLoneLowSurrogateRejected() {
  Expect(!ParseJson("\"\\udc00\"").has_value(),
         "lone low surrogate should be rejected as a parse error");
  Expect(!ParseJson("\"\\ud800\"").has_value(),
         "lone high surrogate should stay rejected");
  // A well-formed surrogate pair must still decode and stay valid UTF-8.
  const auto pair = ParseJson("\"\\ud83d\\ude00\"");  // U+1F600
  Expect(pair.has_value() && pair->IsString(), "valid surrogate pair should parse");
  Expect(microide::util::IsValidUtf8(pair->AsString()),
         "decoded surrogate pair must be valid UTF-8");
}

// The parse and serialize fast paths bulk-copy escape-free runs and drop into the
// escape switch only at boundaries; exercise strings that mix long clean runs with
// every escape kind, control chars, and unicode to prove the boundaries are exact.
void TestStringEscapeRunsRoundTrip() {
  // Build a value carrying an escape-heavy string, serialize it, parse it back, and
  // confirm the decoded bytes are identical.
  const std::string original =
      std::string("plain text run \"quote\" and \\backslash\\ then\n\ttabs\r\n") +
      std::string("\x01\x1f control ") + "unicode: \xE2\x9C\x93 end";
  microide::util::JsonObject obj;
  obj["s"] = JsonValue(original);
  const std::string serialized = SerializeJson(JsonValue(std::move(obj)));
  const auto reparsed = ParseJson(serialized);
  Expect(reparsed.has_value(), "escape-heavy string should round-trip through serialize+parse");
  Expect((*reparsed)["s"].AsString() == original,
         "decoded string bytes must exactly match the original");

  // Direct parse of a literal covering an escape at the very start, adjacent
  // escapes, and a trailing clean run.
  const auto direct = ParseJson("\"\\t\\\\\\\"mid\\n\\/end\"");
  Expect(direct.has_value() && direct->IsString(), "adjacent escapes should parse");
  Expect(direct->AsString() == std::string("\t\\\"mid\n/end"),
         "adjacent-escape decoding must be exact");

  // An empty string and a string that is a single escape.
  Expect(ParseJson("\"\"")->AsString().empty(), "empty string should decode to empty");
  Expect(ParseJson("\"\\n\"")->AsString() == "\n", "single-escape string should decode");
}

// A non-finite double has no JSON form; the serializer emits null rather than the
// bare token nan/inf that this parser (and any strict peer) would reject.
void TestNonFiniteDoubleSerializesAsNull() {
  const JsonValue nan_value{std::numeric_limits<double>::quiet_NaN()};
  const JsonValue inf_value{std::numeric_limits<double>::infinity()};
  Expect(SerializeJson(nan_value) == "null", "NaN should serialize as null");
  Expect(SerializeJson(inf_value) == "null", "infinity should serialize as null");
  // And the emitted text must round-trip back through the parser.
  Expect(ParseJson(SerializeJson(nan_value)).has_value(),
         "serialized non-finite output must be parseable");
}

// The additive mutable accessors let owners move data out instead of copying (the
// LSP completion/code-action parsers use them). They must match the const API's
// presence semantics and return nullptr on a type mismatch.
void TestMutableAccessorsMoveOutAndTypeGuard() {
  JsonValue root = ParseJson(R"({"items":[{"label":"alpha","n":3}],"name":"root"})").value();

  // MutableAt: present key -> pointer; absent key / non-object -> nullptr.
  Expect(root.MutableAt("missing") == nullptr, "MutableAt returns nullptr for an absent key");
  JsonValue* name = root.MutableAt("name");
  Expect(name != nullptr && name->IsString(), "MutableAt resolves a present key");

  // MutableString on the wrong alternative is nullptr; on a string it aliases it.
  Expect(root.MutableAt("items")->MutableString() == nullptr,
         "MutableString returns nullptr for a non-string");
  std::string moved = std::move(*name->MutableString());
  Expect(moved == "root", "MutableString exposes the stored string for moving");
  Expect(root["name"].AsString().empty(), "the moved-from string is left empty in place");

  // MutableArray + nested move-out from an element.
  JsonValue* items = root.MutableAt("items");
  Expect(items != nullptr && items->MutableArray() != nullptr, "MutableArray resolves an array");
  Expect(root.MutableAt("name")->MutableArray() == nullptr,
         "MutableArray returns nullptr for a non-array");
  JsonValue& first = (*items->MutableArray())[0];
  std::string label = std::move(*first.MutableAt("label")->MutableString());
  Expect(label == "alpha", "an element's string field moves out via the mutable chain");
  Expect(first["n"].AsInt() == 3, "unrelated fields are untouched by the move-out");
}

}  // namespace

void RegisterJsonValueTests(std::vector<TestCase>& tests) {
  AddTest(tests, "JsonValue/MutableAccessorsMoveOutAndTypeGuard",
          TestMutableAccessorsMoveOutAndTypeGuard);
  AddTest(tests, "JsonValue/DeeplyNestedArrayRejected", TestDeeplyNestedArrayRejected);
  AddTest(tests, "JsonValue/DeeplyNestedObjectRejected", TestDeeplyNestedObjectRejected);
  AddTest(tests, "JsonValue/ModestNestingStillParses", TestModestNestingStillParses);
  AddTest(tests, "JsonValue/SerializeRoundTripsModestNesting",
          TestSerializeRoundTripsModestNesting);
  AddTest(tests, "JsonValue/OutOfRangeIntegerFallsBackToDouble",
          TestOutOfRangeIntegerFallsBackToDouble);
  AddTest(tests, "JsonValue/LoneLowSurrogateRejected", TestLoneLowSurrogateRejected);
  AddTest(tests, "JsonValue/StringEscapeRunsRoundTrip", TestStringEscapeRunsRoundTrip);
  AddTest(tests, "JsonValue/NonFiniteDoubleSerializesAsNull",
          TestNonFiniteDoubleSerializesAsNull);
}

}  // namespace microide::tests
