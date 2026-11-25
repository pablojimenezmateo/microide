#include "TestSupport.h"

#include "util/JsonValue.h"
#include "util/Parse.h"
#include "util/StringUtil.h"

#include <clocale>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

namespace microide::tests {
namespace {

using microide::util::JsonValue;
using microide::util::ParseJson;
using microide::util::SerializeJson;

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

void TestModestNestingStillParses() {
  const int kDepth = 50;
  std::string payload;
  payload.append(static_cast<std::size_t>(kDepth), '[');
  payload += "42";
  payload.append(static_cast<std::size_t>(kDepth), ']');

  const auto parsed = ParseJson(payload);
  Expect(parsed.has_value(), "modest nesting within the limit should parse");

  const JsonValue* cursor = &*parsed;
  for (int i = 0; i < kDepth; ++i) {
    Expect(cursor->IsArray() && cursor->AsArray().size() == 1,
           "each level should be a single-element array");
    cursor = &cursor->AsArray()[0];
  }
  Expect(cursor->IsInt() && cursor->AsInt() == 42, "innermost value should be 42");
}

void TestSerializeRoundTripsModestNesting() {
  const std::string payload = "[[[[[1]]]]]";
  const auto parsed = ParseJson(payload);
  Expect(parsed.has_value(), "small nested array should parse");
  Expect(SerializeJson(*parsed) == payload, "round-trip should preserve structure");
}

void TestOutOfRangeIntegerFallsBackToDouble() {
  const auto parsed = ParseJson("{\"id\":9223372036854775808}");
  Expect(parsed.has_value(), "uint64-range integer must not reject the whole document");
  const JsonValue& id = (*parsed)["id"];
  Expect(id.IsDouble(), "over-range integer should decode as a double");
  Expect(id.AsDouble() > 9.2e18, "double fallback should preserve rough magnitude");
}

void TestAsIntClampsOutOfRangeDouble() {
  const auto parsed = ParseJson("{\"ref\":18446744073709551615}");
  Expect(parsed.has_value(), "over-range integer document must parse");
  const JsonValue& ref = (*parsed)["ref"];
  Expect(ref.IsDouble(), "the over-range value should be stored as a double");
  Expect(ref.AsInt() == std::numeric_limits<std::int64_t>::max(),
         "AsInt() must clamp a too-large double to int64 max (no UB)");

  const auto negative = ParseJson("{\"n\":-99999999999999999999}");
  Expect(negative.has_value(), "over-range negative integer document must parse");
  Expect((*negative)["n"].AsInt() == std::numeric_limits<std::int64_t>::min(),
         "AsInt() must clamp a too-negative double to int64 min");

  JsonValue nan_value(std::nan(""));
  Expect(nan_value.AsInt(-7) == -7, "AsInt() on NaN must return the fallback, not trap");
}

void TestLoneSurrogateDecodesToReplacement() {
  for (const char* text : {"\"\\udc00\"", "\"\\ud800\"", "[\"\\uD800\\n\"]"}) {
    const auto lone = ParseJson(text);
    Expect(lone.has_value(), std::string("a lone surrogate does not fail the document: ") + text);
  }
  Expect(ParseJson("\"\\udc00\"")->AsString() == "\xEF\xBF\xBD", "a lone low surrogate is U+FFFD");
  Expect(ParseJson("[\"\\uD800\\n\"]").value()[0].AsString() == "\xEF\xBF\xBD\n",
         "a lone high surrogate is U+FFFD and the escape after it is read on its own");
  const auto pair = ParseJson("\"\\ud83d\\ude00\"");
  Expect(pair.has_value() && pair->IsString(), "valid surrogate pair should parse");
  Expect(microide::util::IsValidUtf8(pair->AsString()),
         "decoded surrogate pair must be valid UTF-8");
}

void TestStringEscapeRunsRoundTrip() {
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

  const auto direct = ParseJson("\"\\t\\\\\\\"mid\\n\\/end\"");
  Expect(direct.has_value() && direct->IsString(), "adjacent escapes should parse");
  Expect(direct->AsString() == std::string("\t\\\"mid\n/end"),
         "adjacent-escape decoding must be exact");

  Expect(ParseJson("\"\"")->AsString().empty(), "empty string should decode to empty");
  Expect(ParseJson("\"\\n\"")->AsString() == "\n", "single-escape string should decode");
}

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
  const auto neg = ParseJson("-1e400");
  Expect(neg.has_value() && neg->IsDouble() && neg->AsDouble() < 0.0,
         "negative over-range float preserves its sign");
}

void TestNonFiniteDoubleSerializesAsNull() {
  const JsonValue nan_value{std::numeric_limits<double>::quiet_NaN()};
  const JsonValue inf_value{std::numeric_limits<double>::infinity()};
  Expect(SerializeJson(nan_value) == "null", "NaN should serialize as null");
  Expect(SerializeJson(inf_value) == "null", "infinity should serialize as null");
  Expect(ParseJson(SerializeJson(nan_value)).has_value(),
         "serialized non-finite output must be parseable");
}

void TestMutableAccessorsMoveOutAndTypeGuard() {
  JsonValue root = ParseJson(R"({"items":[{"label":"alpha","n":3}],"name":"root"})").value();

  Expect(root.MutableAt("missing") == nullptr, "MutableAt returns nullptr for an absent key");
  JsonValue* name = root.MutableAt("name");
  Expect(name != nullptr && name->IsString(), "MutableAt resolves a present key");

  Expect(root.MutableAt("items")->MutableString() == nullptr,
         "MutableString returns nullptr for a non-string");
  std::string moved = std::move(*name->MutableString());
  Expect(moved == "root", "MutableString exposes the stored string for moving");
  Expect(root["name"].AsString().empty(), "the moved-from string is left empty in place");

  JsonValue* items = root.MutableAt("items");
  Expect(items != nullptr && items->MutableArray() != nullptr, "MutableArray resolves an array");
  Expect(root.MutableAt("name")->MutableArray() == nullptr,
         "MutableArray returns nullptr for a non-array");
  JsonValue& first = (*items->MutableArray())[0];
  std::string label = std::move(*first.MutableAt("label")->MutableString());
  Expect(label == "alpha", "an element's string field moves out via the mutable chain");
  Expect(first["n"].AsInt() == 3, "unrelated fields are untouched by the move-out");
}

void TestRawControlCharsInStringRejected() {
  Expect(!ParseJson("\"a\nb\"").has_value(), "raw newline inside a string must reject");
  Expect(!ParseJson("\"a\tb\"").has_value(), "raw tab inside a string must reject");
  Expect(!ParseJson("\"a\rb\"").has_value(), "raw carriage return inside a string must reject");
  Expect(!ParseJson(std::string("\"a\0b\"", 5)).has_value(),
         "raw NUL inside a string must reject");
  Expect(!ParseJson("\"\x01\"").has_value(), "leading raw control byte must reject");

  Expect(ParseJson("\"a\\nb\"")->AsString() == "a\nb", "escaped newline must decode");
  Expect(ParseJson("\"a\\tb\"")->AsString() == "a\tb", "escaped tab must decode");
  Expect(ParseJson("\"a\\rb\"")->AsString() == "a\rb", "escaped carriage return must decode");
}

void TestNumberGrammarStrictness() {
  Expect(!ParseJson("-").has_value(), "lone '-' must reject");
  Expect(!ParseJson("01").has_value(), "leading zero must reject");
  Expect(!ParseJson("1.").has_value(), "trailing decimal point must reject");
  Expect(!ParseJson("1e").has_value(), "exponent with no digits must reject");
  Expect(!ParseJson("1e+").has_value(), "exponent sign with no digits must reject");
  Expect(!ParseJson("-e5").has_value(), "sign with no integer digit must reject");
  Expect(!ParseJson("[01]").has_value(), "leading zero inside an array must reject");

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

  const auto big = ParseJson("9223372036854775808");
  Expect(big.has_value() && big->IsDouble(),
         "valid over-range integer must still fall back to double");
}

void TestObjectMemberSemantics() {
  const auto dup = ParseJson(R"({"k":1,"other":9,"k":2,"k":3})");
  Expect(dup.has_value(), "an object with duplicate keys should still parse");
  Expect(dup->AsObject().size() == 2, "duplicate keys collapse to one member");
  Expect((*dup)["k"].AsInt() == 3, "the LAST occurrence of a duplicate key wins");
  Expect((*dup)["other"].AsInt() == 9, "collapsing duplicates leaves other members intact");

  const auto mixed = ParseJson(R"({"zzz":1,"a":2,"mm":3,"bbbb":4,"n":5})");
  Expect(mixed.has_value(), "fixture should parse");
  Expect((*mixed)["zzz"].AsInt() == 1 && (*mixed)["a"].AsInt() == 2 &&
             (*mixed)["mm"].AsInt() == 3 && (*mixed)["bbbb"].AsInt() == 4 &&
             (*mixed)["n"].AsInt() == 5,
         "every member resolves regardless of document order");
  Expect((*mixed)["missing"].IsNull() && !mixed->HasKey("missing"),
         "an absent key reports missing");

  const auto a = ParseJson(R"({"one":1,"two":[2,{"deep":true}],"three":"x"})");
  const auto b = ParseJson(R"({"three":"x","one":1,"two":[2,{"deep":true}]})");
  Expect(a.has_value() && b.has_value(), "reorder fixtures should parse");
  Expect(SerializeJson(*a) == SerializeJson(*b),
         "key order in the source must not change the serialized bytes");
  Expect(*a == *b, "structural equality stays key-order-independent");
}

void TestWideObjectSortsAndDedupes() {
  std::string text = "{";
  for (int i = 79; i >= 0; --i) {
    text += "\"k" + std::to_string(i) + "\":-1,";
  }
  for (int i = 0; i < 80; ++i) {
    text += "\"k" + std::to_string(i) + "\":" + std::to_string(i);
    if (i != 79) text += ",";
  }
  text += "}";

  const auto parsed = ParseJson(text);
  Expect(parsed.has_value(), "a wide object should parse");
  Expect(parsed->AsObject().size() == 80, "duplicates collapse across the wide-object path too");
  for (int i = 0; i < 80; ++i) {
    Expect((*parsed)["k" + std::to_string(i)].AsInt() == i,
           "every wide-object member resolves to its last value");
  }
}

void TestStructuralEqualityIsOrderIndependentAndDeep() {
  const auto a = ParseJson(R"({"a":1,"b":{"x":[1,2,3],"y":"t"}})");
  const auto b = ParseJson(R"({"b":{"y":"t","x":[1,2,3]},"a":1})");
  const auto c = ParseJson(R"({"a":1,"b":{"x":[1,2,3],"y":"u"}})");
  Expect(a.has_value() && b.has_value() && c.has_value(), "fixtures must parse");
  Expect(*a == *b, "objects equal regardless of key order (unordered_map ==)");
  Expect(!(*a == *c), "a differing nested leaf must compare unequal");

  Expect(JsonValue(std::int64_t{5}) == JsonValue(std::int64_t{5}), "equal ints compare equal");
  Expect(!(JsonValue(std::int64_t{5}) == JsonValue(std::string("5"))), "int != string");
  Expect(JsonValue() == JsonValue(), "null == null");
  Expect(!(JsonValue(true) == JsonValue(false)), "true != false");
}

}

void TestNumberFormattingIsLocaleIndependent() {
  using microide::util::JsonValue;
  using microide::util::ParseDouble;
  using microide::util::ParseJson;
  using microide::util::SerializeJson;

  const auto check = [](const char* where) {
    for (const double d : {1.5, 0.1, -2.25, 1e-9, 1e20, 3.14159265358979}) {
      const std::string text = SerializeJson(JsonValue(d));
      Expect(text.find(',') == std::string::npos,
             std::string(where) + ": serialized double must not use a comma separator");
      const auto reparsed = ParseJson(text);
      Expect(reparsed.has_value() && reparsed->AsDouble() == d,
             std::string(where) + ": serialized double must round-trip exactly");
    }
    const auto parsed = ParseDouble("1.5");
    Expect(parsed.has_value() && *parsed == 1.5,
           std::string(where) + ": '1.5' must parse under any locale");
    const auto in_json = ParseJson("{\"x\":2.5}");
    Expect(in_json.has_value() && (*in_json)["x"].AsDouble() == 2.5,
           std::string(where) + ": a JSON double must parse under any locale");
  };

  check("C locale");

  const char* const saved = std::setlocale(LC_NUMERIC, nullptr);
  const std::string restore = saved != nullptr ? saved : "C";
  for (const char* candidate :
       {"de_DE.UTF-8", "fr_FR.UTF-8", "es_ES.UTF-8", "it_IT.UTF-8", "de_DE.utf8"}) {
    if (std::setlocale(LC_NUMERIC, candidate) == nullptr) {
      continue;
    }
    check(candidate);
    break;
  }
  std::setlocale(LC_NUMERIC, restore.c_str());
}

void TestDoubleSerializesInShortestRoundTripForm() {
  using microide::util::JsonValue;
  using microide::util::SerializeJson;
  Expect(SerializeJson(JsonValue(0.1)) == "0.1", "0.1 serializes as `0.1`");
  Expect(SerializeJson(JsonValue(1.5)) == "1.5", "1.5 serializes as `1.5`");
  Expect(SerializeJson(JsonValue(2.0)) == "2", "an integral double drops the fraction");
  Expect(SerializeJson(JsonValue(1.0 / 3.0)).size() <= 20,
         "a repeating fraction stays in shortest form");
}


void TestParseJsonConformanceTable() {
  static constexpr std::string_view kValid[] = {
      "[]", " [] ", "{}", "[1]", "[1,2]", "{\"a\":1}", "{\"a\":\"b\",\"a\":\"c\"}",
      "\"\\u0000\"", "\"\\uFFFF\"", "\"\\ud83d\\ude00\"", "[0e1]", "[0e+1]", "[-0]",
      "[1E22]", "[1e-2]", "[-123]", "[20e1]", "[1.5]", "\"\xe2\x80\xa8\"", "[\"\\/\"]",
      "\"\\\"\"", "[true,false,null]", "[[[[]]]]", "{\"\":0}", "[ 1 , 2 ]", "\"\\t\"",
      "123", "\"x\"", "true", "null", "\t\n\r [1]\n", "[1e-400]", "[-1e-400]", "[1e400]",
      "{\"a\":{\"b\":[{}]}}", "\"\\u00e9\"", "\"caf\xc3\xa9\"",
  };
  static constexpr std::string_view kInvalid[] = {
      "[1,]", "{\"a\":1,}", "[,1]", "[1,,2]", "{1:1}", "{\"a\" 1}", "{\"a\":}", "[\"\\x00\"]",
      "[\"\\a\"]", "[\"\\u12\"]",
      "[\"\"]x", "{\"a\":1} \"x\"", "[1] [2]", "01", "-01", "1.", ".5", "+1", "1e", "1e+",
      "1eE2", "0x42", "Infinity", "NaN", "- 1", "1 000", "[1", "{\"a\":1", "\"abc", "'a'",
      "[\f1]", "[\"a\tb\"]", "[\xe2\x80\xa8]", "tru", "nul", "True", "", "  ", "[1]x",
      "\xe9", "[\"\\", "{\"a\":1}}", "[\"\\u00zz\"]", "{\"a\":1 \"b\":2}", "[1 2]",
  };
  for (std::string_view text : kValid) {
    Expect(ParseJson(text).has_value(), "well-formed JSON must parse: " + std::string(text));
  }
  for (std::string_view text : kInvalid) {
    Expect(!ParseJson(text).has_value(), "malformed JSON must be rejected: " + std::string(text));
  }

  const auto tiny = ParseJson("[1e-400]");
  Expect(tiny.has_value() && (*tiny)[0].AsDouble(-1.0) == 0.0,
         "1e-400 underflows to zero, not to infinity");
  const auto huge = ParseJson("[1e400]");
  Expect(huge.has_value() && std::isinf((*huge)[0].AsDouble()),
         "1e400 overflows to infinity (documented fallback)");
  const auto surrogate = ParseJson("\"\\ud83d\\ude00\"");
  Expect(surrogate.has_value() && surrogate->AsString() == "\xf0\x9f\x98\x80",
         "a surrogate pair decodes to one 4-byte UTF-8 scalar");
  const auto dup = ParseJson("{\"a\":\"b\",\"a\":\"c\"}");
  Expect(dup.has_value() && (*dup)["a"].AsString() == "c", "the last duplicate key wins");
}

void RegisterJsonValueTests(std::vector<TestCase>& tests) {
  AddTest(tests, "JsonValue/ParseConformanceTable", TestParseJsonConformanceTable);
  AddTest(tests, "JsonValue/NumberFormattingIsLocaleIndependent",
          TestNumberFormattingIsLocaleIndependent);
  AddTest(tests, "JsonValue/DoubleSerializesInShortestRoundTripForm",
          TestDoubleSerializesInShortestRoundTripForm);
  AddTest(tests, "JsonValue/ObjectMemberSemantics", TestObjectMemberSemantics);
  AddTest(tests, "JsonValue/WideObjectSortsAndDedupes", TestWideObjectSortsAndDedupes);
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
  AddTest(tests, "JsonValue/LoneSurrogateDecodesToReplacement", TestLoneSurrogateDecodesToReplacement);
  AddTest(tests, "JsonValue/StringEscapeRunsRoundTrip", TestStringEscapeRunsRoundTrip);
  AddTest(tests, "JsonValue/NonFiniteDoubleSerializesAsNull",
          TestNonFiniteDoubleSerializesAsNull);
  AddTest(tests, "JsonValue/OverRangeFloatDoesNotAbortParse",
          TestOverRangeFloatDoesNotAbortParse);
}

}
