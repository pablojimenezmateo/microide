#include "TestSupport.h"

#include "util/JsonFormat.h"
#include "util/JsonValue.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::util::FormatJson;
using microide::util::JsonFormatResult;

std::string Format(std::string_view src, std::string_view indent = "  ") {
  const JsonFormatResult r = FormatJson(src, indent);
  Expect(r.ok, "expected valid JSON to format");
  return r.text;
}

// Property: reformatting is semantics-preserving.
//
// The tests above pin the intended OUTPUT for hand-written inputs, which is the
// right shape for "keys sort naturally" but cannot say that reformatting never
// changes what a document MEANS. Everything the formatter does by hand — copying
// scalar tokens byte-for-byte, re-quoting nothing, sorting members, rewriting
// whitespace — is a chance to drop a member, mangle an escape, or alter a number,
// and any of those still produces well-formed JSON that the existing assertions
// would not notice.
//
// So generate documents that exercise the parts most likely to break (escapes,
// non-ASCII, duplicate and empty keys, numeric forms, deep nesting, whitespace
// noise) and require three things of every one:
//   * it formats,
//   * the formatted text parses to an EQUIVALENT value (members compared as a
//     multiset, since sorting them is the whole point), and
//   * formatting is idempotent.

// Order-insensitive structural equality. `JsonObject::operator==` compares its
// entry vector in order, which a key-sorting formatter is expected to change, so
// the comparison has to be by content: same multiset of (key, value) pairs.
bool JsonEquivalent(const microide::util::JsonValue& a, const microide::util::JsonValue& b);

bool JsonEntriesEquivalent(const microide::util::JsonObject& a,
                           const microide::util::JsonObject& b) {
  if (a.size() != b.size()) {
    return false;
  }
  std::vector<bool> matched(b.size(), false);
  for (const auto& left : a) {
    bool found = false;
    std::size_t index = 0;
    for (const auto& right : b) {
      if (!matched[index] && left.key == right.key &&
          JsonEquivalent(left.value, right.value)) {
        matched[index] = true;
        found = true;
        break;
      }
      ++index;
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

bool JsonEquivalent(const microide::util::JsonValue& a, const microide::util::JsonValue& b) {
  if (a.IsObject() && b.IsObject()) {
    return JsonEntriesEquivalent(a.AsObject(), b.AsObject());
  }
  if (a.IsArray() && b.IsArray()) {
    const auto& left = a.AsArray();
    const auto& right = b.AsArray();
    if (left.size() != right.size()) {
      return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
      if (!JsonEquivalent(left[i], right[i])) {
        return false;
      }
    }
    return true;
  }
  return a == b;
}

struct JsonGenerator {
  std::uint64_t seed = 0x243F6A8885A308D3ULL;

  std::size_t Next() {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<std::size_t>(seed >> 33);
  }

  // Whitespace between tokens is meaningless to JSON and is exactly what the
  // formatter rewrites, so vary it.
  std::string Gap() {
    switch (Next() % 5) {
      case 0: return "";
      case 1: return " ";
      case 2: return "\n";
      case 3: return "\t";
      default: return "\n  ";
    }
  }

  std::string String() {
    static const char* const kBodies[] = {
        "plain", "", "with space", "esc\\\"quote", "back\\\\slash", "tab\\there",
        "newline\\nhere", "unicode\\u00e9", "surrogate\\ud83d\\ude00", "slash\\/solidus",
        "ctrl\\u0000null", "\xc3\xa9 raw utf8", "10", "2",
    };
    return std::string("\"") + kBodies[Next() % (sizeof(kBodies) / sizeof(kBodies[0]))] + "\"";
  }

  std::string Number() {
    static const char* const kNumbers[] = {
        "0", "-0", "1", "-1", "3.14", "-2.5e10", "1E+3", "1e-3", "12345678901234567890",
        "0.000000000000000001", "1.0000000000000002", "9007199254740993",
    };
    return kNumbers[Next() % (sizeof(kNumbers) / sizeof(kNumbers[0]))];
  }

  std::string Value(int depth) {
    if (depth <= 0) {
      switch (Next() % 5) {
        case 0: return "null";
        case 1: return "true";
        case 2: return "false";
        case 3: return Number();
        default: return String();
      }
    }
    switch (Next() % 8) {
      case 0: return "null";
      case 1: return "true";
      case 2: return Number();
      case 3: return String();
      case 4: {
        const std::size_t count = Next() % 4;
        std::string out = "[";
        for (std::size_t i = 0; i < count; ++i) {
          if (i > 0) out += "," + Gap();
          out += Gap() + Value(depth - 1);
        }
        return out + Gap() + "]";
      }
      default: {
        const std::size_t count = Next() % 5;
        std::string out = "{";
        for (std::size_t i = 0; i < count; ++i) {
          if (i > 0) out += "," + Gap();
          // Keys deliberately collide in case, in natural-sort order, and
          // occasionally exactly (a duplicate key is legal JSON).
          out += Gap() + String() + Gap() + ":" + Gap() + Value(depth - 1);
        }
        return out + Gap() + "}";
      }
    }
  }
};

void TestFormattingPreservesSemantics() {
  JsonGenerator generator;
  std::size_t checked = 0;
  for (int iteration = 0; iteration < 400; ++iteration) {
    const std::string source = generator.Gap() + generator.Value(4) + generator.Gap();
    const std::optional<microide::util::JsonValue> parsed = microide::util::ParseJson(source);
    if (!parsed.has_value()) {
      // The generator only emits valid documents; a rejection here would mean the
      // two parsers disagree, which is itself worth failing on.
      Expect(false, "the generator must only produce documents ParseJson accepts");
      continue;
    }
    const JsonFormatResult formatted = FormatJson(source, "  ");
    Expect(formatted.ok, "a document ParseJson accepts must also format");

    const std::optional<microide::util::JsonValue> reparsed =
        microide::util::ParseJson(formatted.text);
    Expect(reparsed.has_value(), "formatted output must parse");
    Expect(JsonEquivalent(*parsed, *reparsed),
           "reformatting must not change what the document means");

    const JsonFormatResult again = FormatJson(formatted.text, "  ");
    Expect(again.ok && again.text == formatted.text, "formatting must be idempotent");
    ++checked;
  }
  Expect(checked == 400, "every generated document must have been checked");
}

void TestNestedObjectReindents() {
  const std::string out = Format(R"({"a":1,"b":{"c":[1,2],"d":null}})");
  const std::string expected =
      "{\n"
      "  \"a\": 1,\n"
      "  \"b\": {\n"
      "    \"c\": [\n"
      "      1,\n"
      "      2\n"
      "    ],\n"
      "    \"d\": null\n"
      "  }\n"
      "}";
  Expect(out == expected, "nested object should reindent with 2-space indent");
}

void TestEmptyContainersStayInline() {
  Expect(Format("{}") == "{}", "empty object stays {}");
  Expect(Format("[]") == "[]", "empty array stays []");
  Expect(Format(R"({"a":{},"b":[]})") ==
             "{\n  \"a\": {},\n  \"b\": []\n}",
         "empty nested containers stay inline");
}

void TestKeysSortedHumanAlphabetical() {
  // Case-insensitive ordering: Banana between apple and cherry, not before both.
  const std::string out = Format(R"({"cherry":1,"Banana":2,"apple":3})");
  Expect(out == "{\n  \"apple\": 3,\n  \"Banana\": 2,\n  \"cherry\": 1\n}",
         "keys sort case-insensitively (apple, Banana, cherry)");
}

void TestKeysSortedNaturally() {
  // Natural numeric ordering: item2 before item10.
  const std::string out = Format(R"({"item10":0,"item2":0,"item1":0})");
  Expect(out == "{\n  \"item1\": 0,\n  \"item2\": 0,\n  \"item10\": 0\n}",
         "digit runs sort by value, not lexically");
}

void TestSubkeysSortedRecursively() {
  const std::string out = Format(R"({"z":{"b":1,"a":2},"a":{"d":1,"c":2}})");
  Expect(out ==
             "{\n"
             "  \"a\": {\n"
             "    \"c\": 2,\n"
             "    \"d\": 1\n"
             "  },\n"
             "  \"z\": {\n"
             "    \"a\": 2,\n"
             "    \"b\": 1\n"
             "  }\n"
             "}",
         "keys sort at every nesting level");
}

void TestArrayOrderPreserved() {
  // Arrays are ordered data — never reordered, only reindented.
  const std::string out = Format(R"([3,1,2])");
  Expect(out == "[\n  3,\n  1,\n  2\n]", "array element order is preserved");
}

void TestScalarTokensCopiedVerbatim() {
  // Numbers keep their exact literal form (no float/int normalization) and
  // strings keep their escapes.
  const std::string out =
      Format(R"({"n":1.0,"big":123456789012345678901234567890,"e":2e10,"s":"a\t\"bé"})");
  Expect(out.find("\"n\": 1.0") != std::string::npos, "1.0 stays 1.0 (not 1)");
  Expect(out.find("123456789012345678901234567890") != std::string::npos,
         "oversized integer literal copied verbatim");
  Expect(out.find("2e10") != std::string::npos, "exponent literal copied verbatim");
  Expect(out.find(R"("a\t\"bé")") != std::string::npos, "string escapes preserved");
}

void TestTopLevelScalar() {
  Expect(Format("  42 ") == "42", "top-level scalar formats to its token");
  Expect(Format(R"(  "hi"  )") == "\"hi\"", "top-level string trims surrounding ws");
}

void TestWhitespaceNormalizedAndIdempotent() {
  const std::string once = Format("  {  \"b\"\t:\n1 , \"a\":2 }  ");
  const std::string twice = Format(once);
  Expect(once == "{\n  \"a\": 2,\n  \"b\": 1\n}", "messy whitespace normalizes");
  Expect(once == twice, "formatting is idempotent");
}

void TestTabIndent() {
  const std::string out = Format(R"({"a":[1]})", "\t");
  Expect(out == "{\n\t\"a\": [\n\t\t1\n\t]\n}", "tab indent unit is honored");
}

void TestInvalidJsonRejectedWithOffset() {
  {
    const JsonFormatResult r = FormatJson(R"({"a":})", "  ");
    Expect(!r.ok, "missing value should reject");
  }
  {
    const JsonFormatResult r = FormatJson(R"({"a":1} garbage)", "  ");
    Expect(!r.ok && r.error_offset >= 8, "trailing garbage rejects at its offset");
  }
  {
    const JsonFormatResult r = FormatJson("[1,2", "  ");
    Expect(!r.ok, "unterminated array rejects");
  }
  {
    const JsonFormatResult r = FormatJson("{\"a\":\"x\ny\"}", "  ");
    Expect(!r.ok, "raw newline inside string rejects");
  }
  {
    const JsonFormatResult r = FormatJson("01", "  ");
    Expect(!r.ok, "leading-zero number rejects");
  }
}

void TestDeeplyNestedRejected() {
  std::string payload;
  payload.append(200000, '[');
  payload.append(200000, ']');
  const JsonFormatResult r = FormatJson(payload, "  ");
  Expect(!r.ok, "pathological nesting rejects rather than overflowing the stack");
}

}  // namespace

void RegisterJsonFormatTests(std::vector<TestCase>& tests) {
  AddTest(tests, "JsonFormat/NestedObjectReindents", TestNestedObjectReindents);
  AddTest(tests, "JsonFormat/EmptyContainersStayInline", TestEmptyContainersStayInline);
  AddTest(tests, "JsonFormat/KeysSortedHumanAlphabetical", TestKeysSortedHumanAlphabetical);
  AddTest(tests, "JsonFormat/KeysSortedNaturally", TestKeysSortedNaturally);
  AddTest(tests, "JsonFormat/SubkeysSortedRecursively", TestSubkeysSortedRecursively);
  AddTest(tests, "JsonFormat/ArrayOrderPreserved", TestArrayOrderPreserved);
  AddTest(tests, "JsonFormat/ScalarTokensCopiedVerbatim", TestScalarTokensCopiedVerbatim);
  AddTest(tests, "JsonFormat/TopLevelScalar", TestTopLevelScalar);
  AddTest(tests, "JsonFormat/WhitespaceNormalizedAndIdempotent",
          TestWhitespaceNormalizedAndIdempotent);
  AddTest(tests, "JsonFormat/TabIndent", TestTabIndent);
  AddTest(tests, "JsonFormat/InvalidJsonRejectedWithOffset", TestInvalidJsonRejectedWithOffset);
  AddTest(tests, "JsonFormat/DeeplyNestedRejected", TestDeeplyNestedRejected);
  AddTest(tests, "JsonFormat/FormattingPreservesSemantics", TestFormattingPreservesSemantics);
}

}  // namespace microide::tests
