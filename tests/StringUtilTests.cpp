#include "TestSupport.h"

#include "compare/CompareModel.h"
#include "util/StringUtil.h"

#include <vector>

namespace microide::tests {
namespace {

using microide::util::IsValidUtf8;
using microide::util::JoinLines;
using microide::util::SplitLines;
using microide::util::Utf8SequenceLength;

void TestStringUtilUtf8SequenceLengthHandlesAsciiAndEmoji() {
  const std::string text = "a😀z";
  Expect(Utf8SequenceLength(text, 0) == 1,
         "utf8 sequence length should treat ascii as a single byte");
  Expect(Utf8SequenceLength(text, 1) == 4,
         "utf8 sequence length should return four bytes for emoji codepoints");
  Expect(Utf8SequenceLength(text, text.size()) == 0,
         "utf8 sequence length should report zero past the end");
}

void TestStringUtilUtf8ValidationRejectsBrokenSequences() {
  Expect(IsValidUtf8("hello 😀"),
         "utf8 validation should accept valid multibyte sequences");

  const std::string truncated = std::string("x") + static_cast<char>(0xE2) +
                                static_cast<char>(0x82);
  Expect(!IsValidUtf8(truncated),
         "utf8 validation should reject truncated multibyte sequences");

  const std::string surrogate = std::string("x") + static_cast<char>(0xED) +
                                static_cast<char>(0xA0) + static_cast<char>(0x80);
  Expect(!IsValidUtf8(surrogate),
         "utf8 validation should reject surrogate codepoint encodings");
}

void TestStringUtilSplitLinesNormalizesMixedLineEndings() {
  const std::vector<std::string> lines = SplitLines("alpha\r\nbeta\rgamma\n");
  Expect(lines.size() == 4,
         "line splitting should preserve a trailing empty line after the final newline");
  Expect(lines[0] == "alpha" && lines[1] == "beta" && lines[2] == "gamma" &&
             lines[3].empty(),
         "line splitting should normalize CRLF, CR, and LF separators consistently");
}

void TestStringUtilJoinLinesHonorsSeparatorsAndEmptyInput() {
  const std::vector<std::string> lines = {"alpha", "beta", "gamma"};
  Expect(JoinLines(lines, "::") == "alpha::beta::gamma",
         "join lines should place the requested separator between every line");
  Expect(JoinLines({}, "\n").empty(),
         "join lines should keep empty inputs empty");
}

void TestCompareModelHandlesCrLfInputViaSharedSplitter() {
  const auto model =
      microide::compare::BuildCompareModel("alpha\r\nbeta\r\n", "alpha\r\nbeta\r\ngamma\r\n");
  Expect(model.rows.size() == 4,
         "compare model should preserve logical lines plus the shared trailing empty row");
  Expect(model.rows[0].left_text == "alpha" && model.rows[1].left_text == "beta",
         "compare model should split CRLF input into plain logical lines");
  Expect(model.rows[2].kind == microide::compare::CompareRowKind::Added &&
             model.rows[2].right_text == "gamma",
         "compare model should preserve added CRLF lines under the shared splitter");
  Expect(model.rows[3].left_text.empty() && model.rows[3].right_text.empty(),
         "compare model should preserve the trailing empty row from TextViewport splitting");
}

}  // namespace

void RegisterStringUtilTests(std::vector<TestCase>& tests) {
  AddTest(tests, "StringUtil/Utf8SequenceLengthHandlesAsciiAndEmoji",
          TestStringUtilUtf8SequenceLengthHandlesAsciiAndEmoji);
  AddTest(tests, "StringUtil/Utf8ValidationRejectsBrokenSequences",
          TestStringUtilUtf8ValidationRejectsBrokenSequences);
  AddTest(tests, "StringUtil/SplitLinesNormalizesMixedLineEndings",
          TestStringUtilSplitLinesNormalizesMixedLineEndings);
  AddTest(tests, "StringUtil/JoinLinesHonorsSeparatorsAndEmptyInput",
          TestStringUtilJoinLinesHonorsSeparatorsAndEmptyInput);
  AddTest(tests, "StringUtil/CompareModelHandlesCrLfInputViaSharedSplitter",
          TestCompareModelHandlesCrLfInputViaSharedSplitter);
}

}  // namespace microide::tests
