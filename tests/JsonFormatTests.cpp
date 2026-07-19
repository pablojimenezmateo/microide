#include "TestSupport.h"

#include "util/JsonFormat.h"

#include <string>

namespace microide::tests {
namespace {

using microide::util::FormatJson;
using microide::util::JsonFormatResult;

std::string Format(std::string_view src, std::string_view indent = "  ") {
  const JsonFormatResult r = FormatJson(src, indent);
  Expect(r.ok, "expected valid JSON to format");
  return r.text;
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
}

}  // namespace microide::tests
