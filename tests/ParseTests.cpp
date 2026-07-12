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
  // Regression: match the integer parsers' strictness (std::from_chars). strto*
  // previously accepted a leading space / '+' / hex-float on an otherwise FINITE
  // value, making ParseFloat laxer than ParseInt for the same token.
  Expect(!microide::util::ParseFloat(" 3.5").has_value(),
         "parse float should reject leading whitespace before a finite value");
  Expect(!microide::util::ParseFloat("+3.5").has_value(),
         "parse float should reject a leading '+'");
  Expect(!microide::util::ParseFloat("0x1p4").has_value(),
         "parse float should reject hex-float notation");
  Expect(microide::util::ParseFloat("-3.5").has_value(),
         "parse float still accepts a leading '-'");
}

void TestParseRealAcceptsSubnormalsAndRejectsOverflow() {
  // Gradual underflow to a representable subnormal sets errno==ERANGE but yields a
  // finite value that must be accepted, not dropped.
  const auto tiny = microide::util::ParseDouble("1e-310");
  Expect(tiny.has_value() && *tiny > 0.0 && *tiny < 1e-300,
         "parse double should accept subnormal magnitudes");
  const auto tiny_float = microide::util::ParseFloat("1e-40");
  Expect(tiny_float.has_value() && *tiny_float > 0.0f,
         "parse float should accept subnormal magnitudes");
  // Overflow yields ±HUGE_VAL (non-finite) and must still be rejected.
  Expect(!microide::util::ParseDouble("1e400").has_value(),
         "parse double should reject overflow to infinity");
  Expect(!microide::util::ParseFloat("1e400").has_value(),
         "parse float should reject overflow to infinity");
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
  AddTest(tests, "Parse/RealAcceptsSubnormalsAndRejectsOverflow",
          TestParseRealAcceptsSubnormalsAndRejectsOverflow);
}

}  // namespace microide::tests
