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

// AsInt() on an out-of-int64-range double (produced by the integer-overflow
// fallback above) must clamp instead of executing the undefined static_cast<int64>.
// This is reachable for a uint64-range DAP variablesReference / handle.
void TestAsIntClampsOutOfRangeDouble() {
  const auto parsed = ParseJson("{\"ref\":18446744073709551615}");  // ~1.8e19, past 2^63
  Expect(parsed.has_value(), "over-range integer document must parse");
  const JsonValue& ref = (*parsed)["ref"];
  Expect(ref.IsDouble(), "the over-range value should be stored as a double");
  Expect(ref.AsInt() == std::numeric_limits<std::int64_t>::max(),
         "AsInt() must clamp a too-large double to int64 max (no UB)");

  const auto negative = ParseJson("{\"n\":-99999999999999999999}");  // past -2^63
  Expect(negative.has_value(), "over-range negative integer document must parse");
  Expect((*negative)["n"].AsInt() == std::numeric_limits<std::int64_t>::min(),
         "AsInt() must clamp a too-negative double to int64 min");

  // A NaN double (constructed directly) must fall through to the caller's fallback.
  JsonValue nan_value(std::nan(""));
  Expect(nan_value.AsInt(-7) == -7, "AsInt() on NaN must return the fallback, not trap");
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
// Regression: a syntactically valid float literal whose magnitude overflows
// double must not abort the whole message parse (it previously returned nullopt
// from ParseNumber, silently dropping the entire LSP/DAP message). It must
// resolve the surrounding object with a best-effort non-finite value, mirroring
// the int64-overflow fallback.
void TestOverRangeFloatDoesNotAbortParse() {
  const auto obj = ParseJson("{\"timeout\":1e400,\"ok\":true}");
  Expect(obj.has_value(), "over-range float must not abort the enclosing parse");
  Expect(obj->IsObject(), "parsed value should be the enclosing object");
  Expect((*obj)["ok"].IsBool() && (*obj)["ok"].AsBool(),
         "sibling members must still resolve after an over-range float");
  Expect((*obj)["timeout"].IsDouble(),
         "over-range float should be stored as a (non-finite) double");
  Expect(!std::isfinite((*obj)["timeout"].AsDouble()),
         "over-range float should decode as a non-finite double");
  // Negative overflow preserves sign.
  const auto neg = ParseJson("-1e400");
  Expect(neg.has_value() && neg->IsDouble() && neg->AsDouble() < 0.0,
         "negative over-range float preserves its sign");
}

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

// RFC 8259 forbids raw control bytes (< 0x20) inside a string literal; they must
// be escaped. A raw newline/tab/NUL/CR from a malformed peer (or a hand-authored
// --control-spec line) must reject rather than smuggle a control char into a host
// string. The escaped forms stay fully valid.
void TestRawControlCharsInStringRejected() {
  Expect(!ParseJson("\"a\nb\"").has_value(), "raw newline inside a string must reject");
  Expect(!ParseJson("\"a\tb\"").has_value(), "raw tab inside a string must reject");
  Expect(!ParseJson("\"a\rb\"").has_value(), "raw carriage return inside a string must reject");
  Expect(!ParseJson(std::string("\"a\0b\"", 5)).has_value(),
         "raw NUL inside a string must reject");
  // A control byte as the very first content byte must also reject (empty run).
  Expect(!ParseJson("\"\x01\"").has_value(), "leading raw control byte must reject");

  // The escaped forms of the same characters stay valid and decode correctly.
  Expect(ParseJson("\"a\\nb\"")->AsString() == "a\nb", "escaped newline must decode");
  Expect(ParseJson("\"a\\tb\"")->AsString() == "a\tb", "escaped tab must decode");
  Expect(ParseJson("\"a\\rb\"")->AsString() == "a\rb", "escaped carriage return must decode");
}

// RFC 8259 has a strict number grammar: no lone '-', no leading zeros ("01"), a
// '.' needs a trailing digit ("1." is invalid), and an exponent needs digits
// ("1e"/"1e+" are invalid). Valid forms — including "0", "-0", "1.0", "1e-9" and
// the large-integer double fallback — must still parse.
void TestNumberGrammarStrictness() {
  // Rejected malformed numbers. Wrap in an array so a trailing-garbage number is
  // exercised inside a real document context.
  Expect(!ParseJson("-").has_value(), "lone '-' must reject");
  Expect(!ParseJson("01").has_value(), "leading zero must reject");
  Expect(!ParseJson("1.").has_value(), "trailing decimal point must reject");
  Expect(!ParseJson("1e").has_value(), "exponent with no digits must reject");
  Expect(!ParseJson("1e+").has_value(), "exponent sign with no digits must reject");
  Expect(!ParseJson("-e5").has_value(), "sign with no integer digit must reject");
  Expect(!ParseJson("[01]").has_value(), "leading zero inside an array must reject");

  // Accepted forms round-trip to the expected value/kind.
  Expect(ParseJson("0")->AsInt() == 0, "'0' must parse");
  Expect(ParseJson("-0")->AsInt() == 0, "'-0' must parse");
  Expect(ParseJson("123")->AsInt() == 123, "'123' must parse");
  const auto one_point_zero = ParseJson("1.0");
  Expect(one_point_zero.has_value() && one_point_zero->IsDouble() &&
             one_point_zero->AsDouble() == 1.0,
         "'1.0' must parse as a double");
  const auto tiny = ParseJson("1e-9");
  Expect(tiny.has_value() && tiny->IsDouble() && tiny->AsDouble() == 1e-9,
         "'1e-9' must parse as a double");
  const auto neg_half = ParseJson("-0.5");
  Expect(neg_half.has_value() && neg_half->AsDouble() == -0.5, "'-0.5' must parse");

  // The int64-overflow -> double fallback for a syntactically valid literal is
  // preserved (regression guard for the strict-grammar change).
  const auto big = ParseJson("9223372036854775808");  // 2^63
  Expect(big.has_value() && big->IsDouble(),
         "valid over-range integer must still fall back to double");
}

// Structural equality is the correctness basis for the LSP registration fast
// path (comparing init-options/settings without serializing). Objects must
// compare key-order-independently; nested structures compare deeply.
void TestStructuralEqualityIsOrderIndependentAndDeep() {
  const auto a = ParseJson(R"({"a":1,"b":{"x":[1,2,3],"y":"t"}})");
  const auto b = ParseJson(R"({"b":{"y":"t","x":[1,2,3]},"a":1})");  // reordered keys
  const auto c = ParseJson(R"({"a":1,"b":{"x":[1,2,3],"y":"u"}})");  // one leaf differs
  Expect(a.has_value() && b.has_value() && c.has_value(), "fixtures must parse");
  Expect(*a == *b, "objects equal regardless of key order (unordered_map ==)");
  Expect(!(*a == *c), "a differing nested leaf must compare unequal");

  // Scalars and type mismatches.
  Expect(JsonValue(std::int64_t{5}) == JsonValue(std::int64_t{5}), "equal ints compare equal");
  Expect(!(JsonValue(std::int64_t{5}) == JsonValue(std::string("5"))), "int != string");
  Expect(JsonValue() == JsonValue(), "null == null");
  Expect(!(JsonValue(true) == JsonValue(false)), "true != false");
}

}  // namespace

void RegisterJsonValueTests(std::vector<TestCase>& tests) {
  AddTest(tests, "JsonValue/StructuralEqualityIsOrderIndependentAndDeep",
          TestStructuralEqualityIsOrderIndependentAndDeep);
  AddTest(tests, "JsonValue/RawControlCharsInStringRejected",
          TestRawControlCharsInStringRejected);
  AddTest(tests, "JsonValue/NumberGrammarStrictness", TestNumberGrammarStrictness);
  AddTest(tests, "JsonValue/MutableAccessorsMoveOutAndTypeGuard",
          TestMutableAccessorsMoveOutAndTypeGuard);
  AddTest(tests, "JsonValue/DeeplyNestedArrayRejected", TestDeeplyNestedArrayRejected);
  AddTest(tests, "JsonValue/DeeplyNestedObjectRejected", TestDeeplyNestedObjectRejected);
  AddTest(tests, "JsonValue/ModestNestingStillParses", TestModestNestingStillParses);
  AddTest(tests, "JsonValue/SerializeRoundTripsModestNesting",
          TestSerializeRoundTripsModestNesting);
  AddTest(tests, "JsonValue/AsIntClampsOutOfRangeDouble", TestAsIntClampsOutOfRangeDouble);
  AddTest(tests, "JsonValue/OutOfRangeIntegerFallsBackToDouble",
          TestOutOfRangeIntegerFallsBackToDouble);
  AddTest(tests, "JsonValue/LoneLowSurrogateRejected", TestLoneLowSurrogateRejected);
  AddTest(tests, "JsonValue/StringEscapeRunsRoundTrip", TestStringEscapeRunsRoundTrip);
  AddTest(tests, "JsonValue/NonFiniteDoubleSerializesAsNull",
          TestNonFiniteDoubleSerializesAsNull);
  AddTest(tests, "JsonValue/OverRangeFloatDoesNotAbortParse",
          TestOverRangeFloatDoesNotAbortParse);
}

}  // namespace microide::tests
