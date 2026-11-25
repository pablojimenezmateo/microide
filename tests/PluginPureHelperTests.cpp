#include "TestSupport.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "plugin/PluginLuaInterop.h"

namespace microide::tests {
namespace {

using microide::plugin::lua_interop::IsValidIdentifier;
using microide::plugin::lua_interop::ParseHexColor;

void TestIdentifierRuleAcceptsExactlyItsAlphabet() {
  const std::vector<std::string> valid = {
      "a", "Z", "0", "plugin", "my-plugin", "my_plugin", "my.plugin",
      "a1.b2-c3_d4", "UPPER", "123", "...", "---", "___",
  };
  for (const std::string& value : valid) {
    Expect(IsValidIdentifier(value), "\"" + value + "\" should be a valid identifier");
  }

  const std::vector<std::string> invalid = {
      "",
      " ",
      "has space",
      "tab\there",
      "new\nline",
      "slash/path",
      "back\\slash",
      "colon:id",
      "star*",
      "quote\"",
      "'single'",
      "semi;colon",
      "dollar$",
      "percent%",
      "paren()",
      "brace{}",
      "bracket[]",
      "at@sign",
      "plus+",
      "equals=",
      "caf\xc3\xa9",
      "\xe6\x97\xa5",
  };
  for (const std::string& value : invalid) {
    Expect(!IsValidIdentifier(value),
           "\"" + value + "\" should NOT be a valid identifier");
  }

  const std::string embedded_nul("ab\0cd", 5);
  Expect(!IsValidIdentifier(embedded_nul),
         "an embedded NUL must be rejected, not treated as a terminator");
  const std::string trailing_nul("ab\0", 3);
  Expect(!IsValidIdentifier(trailing_nul), "a trailing NUL is still a byte in the id");
}

void TestHexColorAcceptsOnlyTheTwoDocumentedShapes() {
  struct Case {
    const char* text;
    std::uint8_t r, g, b, a;
  };
  const std::vector<Case> valid = {
      {"#000000", 0, 0, 0, 255},
      {"#ffffff", 255, 255, 255, 255},
      {"#FFFFFF", 255, 255, 255, 255},
      {"#AbCdEf", 0xAB, 0xCD, 0xEF, 255},
      {"#123456", 0x12, 0x34, 0x56, 255},
      {"#00000000", 0, 0, 0, 0},
      {"#ffffffff", 255, 255, 255, 255},
      {"#12345678", 0x12, 0x34, 0x56, 0x78},
      {"#0000007f", 0, 0, 0, 0x7F},
  };
  for (const Case& test_case : valid) {
    const auto parsed = ParseHexColor(test_case.text);
    Expect(parsed.has_value(), std::string(test_case.text) + " should parse");
    Expect(parsed->r == test_case.r && parsed->g == test_case.g && parsed->b == test_case.b &&
               parsed->a == test_case.a,
           std::string(test_case.text) + " parsed to (" + std::to_string(parsed->r) + "," +
               std::to_string(parsed->g) + "," + std::to_string(parsed->b) + "," +
               std::to_string(parsed->a) + ")");
  }

  const std::vector<std::string> invalid = {
      "",
      "#",
      "#fff",
      "#ffff",
      "#fffff",
      "#fffffff",
      "#fffffffff",
      "ffffff",
      "ffffffff",
      "#gggggg",
      "#12345g",
      "#1234567g",
      "#ffffff ",
      " #ffffff",
      "#ff ffff",
      "0xffffff",
      "#-12345",
      "#+12345",
  };
  for (const std::string& text : invalid) {
    Expect(!ParseHexColor(text).has_value(),
           "\"" + text + "\" must not parse as a colour");
  }

  const std::string with_nul("#ff\0fff", 7);
  Expect(!ParseHexColor(with_nul).has_value(),
         "a NUL byte is not a hex digit, whatever the length");
}

}

void RegisterPluginPureHelperTests(std::vector<TestCase>& tests) {
  AddTest(tests, "PluginPureHelpers/IdentifierRuleAcceptsExactlyItsAlphabet",
          TestIdentifierRuleAcceptsExactlyItsAlphabet);
  AddTest(tests, "PluginPureHelpers/HexColorAcceptsOnlyTheTwoDocumentedShapes",
          TestHexColorAcceptsOnlyTheTwoDocumentedShapes);
}

}
