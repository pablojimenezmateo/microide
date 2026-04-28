#include "TestSupport.h"

#include <cmath>
#include <limits>

#include "util/Parse.h"

namespace microide::tests {
namespace {

void TestParseIntRejectsEmptyWhitespaceAndTrailingJunk() {
  Expect(!microide::util::ParseInt("").has_value(), "parse int should reject empty input");
  Expect(!microide::util::ParseInt(" 12").has_value(),
         "parse int should reject leading whitespace");
  Expect(!microide::util::ParseInt("12 ").has_value(),
         "parse int should reject trailing whitespace");
  Expect(!microide::util::ParseInt("12x").has_value(),
         "parse int should reject trailing junk");
}

void TestParseIntRejectsOverflow() {
  Expect(!microide::util::ParseInt("999999999999999999999").has_value(),
         "parse int should reject overflow");
}

void TestParseInt64ParsesSignedValues() {
  const auto parsed = microide::util::ParseInt64("-42");
  Expect(parsed.has_value() && *parsed == -42, "parse int64 should preserve signed values");
}

void TestParseSizeRejectsNegativeOverflowAndWhitespace() {
  Expect(!microide::util::ParseSize("-1").has_value(),
         "parse size should reject negative values");
  Expect(!microide::util::ParseSize("184467440737095516160").has_value(),
         "parse size should reject overflow");
  Expect(!microide::util::ParseSize("7 ").has_value(),
         "parse size should reject trailing whitespace");
}

void TestParseFloatParsesFiniteValuesAndRejectsInvalidInput() {
  const auto parsed = microide::util::ParseFloat("3.5");
  Expect(parsed.has_value() && std::fabs(*parsed - 3.5f) < 0.0001f,
         "parse float should decode finite decimal values");
  Expect(!microide::util::ParseFloat("").has_value(), "parse float should reject empty input");
  Expect(!microide::util::ParseFloat(" nan").has_value(),
         "parse float should reject leading whitespace");
  Expect(!microide::util::ParseFloat("nan").has_value(),
         "parse float should reject non-finite values");
}

}  // namespace

void RegisterParseTests(std::vector<TestCase>& tests) {
  AddTest(tests, "Parse/IntRejectsEmptyWhitespaceAndTrailingJunk",
          TestParseIntRejectsEmptyWhitespaceAndTrailingJunk);
  AddTest(tests, "Parse/IntRejectsOverflow", TestParseIntRejectsOverflow);
  AddTest(tests, "Parse/Int64ParsesSignedValues", TestParseInt64ParsesSignedValues);
  AddTest(tests, "Parse/SizeRejectsNegativeOverflowAndWhitespace",
          TestParseSizeRejectsNegativeOverflowAndWhitespace);
  AddTest(tests, "Parse/FloatParsesFiniteValuesAndRejectsInvalidInput",
          TestParseFloatParsesFiniteValuesAndRejectsInvalidInput);
}

}  // namespace microide::tests
