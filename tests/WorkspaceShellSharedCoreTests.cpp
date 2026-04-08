#include "TestSupport.h"

#include "workspace/WorkspaceShellShared.h"

#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::CollapseWhitespace;
using microide::workspace::CommandCompletionCandidate;
using microide::workspace::CommonPrefix;
using microide::workspace::DecodeSessionNodePath;
using microide::workspace::DetectLineEnding;
using microide::workspace::EncodeSessionNodePath;
using microide::workspace::FormatCommandCompletionToken;
using microide::workspace::JoinCommandArguments;
using microide::workspace::ParseCommandLine;
using microide::workspace::ParseUiScaleValue;
using microide::workspace::PathEqualsOrWithin;
using microide::workspace::QuoteCommandArg;
using microide::workspace::ReadFileText;
using microide::workspace::RelativePathLabel;
using microide::workspace::RemoveLastUtf8Codepoint;
using microide::workspace::ReplacePathPrefix;
using microide::workspace::SplitSyntaxLines;
using microide::workspace::ToLower;
using microide::workspace::Utf8ByteOffsetForCodepointCount;
using microide::workspace::Utf8CodepointCount;

void TestWorkspaceSharedParseCommandLine() {
  const auto parsed = ParseCommandLine(R"(open "two words" path\ with\ spaces 'tail value')");
  Expect(parsed.tokens.size() == 4, "command line should produce four tokens");
  Expect(parsed.tokens[0].text == "open", "first token should be command name");
  Expect(parsed.tokens[1].text == "two words", "double-quoted token should preserve spaces");
  Expect(parsed.tokens[2].text == "path with spaces", "escaped spaces should be unescaped");
  Expect(parsed.tokens[3].text == "tail value", "single-quoted token should preserve spaces");
  Expect(!parsed.trailing_space, "command line should not end with trailing space");
  Expect(!parsed.dangling_escape, "command line should not end with dangling escape");
  Expect(parsed.open_quote == '\0', "command line should not leave an open quote");
}

void TestWorkspaceSharedParseCommandLineIncompleteState() {
  const auto escaped = ParseCommandLine(R"(open trailing\)");
  Expect(escaped.tokens.size() == 2, "dangling escape command should keep prior tokens");
  Expect(escaped.dangling_escape, "dangling escape should be reported");

  const auto quoted = ParseCommandLine(R"(open "unterminated)");
  Expect(quoted.tokens.size() == 2, "unterminated quote command should keep current token");
  Expect(quoted.open_quote == '"', "unterminated quote should be reported");
}

void TestWorkspaceSharedUiScaleParsing() {
  const auto percent = ParseUiScaleValue("125%");
  Expect(percent.has_value() && *percent == 1.25f, "percent ui scale should parse to ratio");

  const auto whole_number_percent = ParseUiScaleValue("150");
  Expect(whole_number_percent.has_value() && *whole_number_percent == 1.5f,
         "whole-number percent ui scale should parse to ratio");

  const auto invalid = ParseUiScaleValue("abc");
  Expect(!invalid.has_value(), "invalid ui scale should fail");
}

void TestWorkspaceSharedSessionEncoding() {
  const std::vector<std::size_t> path = {0, 3, 12};
  const std::string encoded = EncodeSessionNodePath(path);
  Expect(encoded == "0/3/12", "session path encoding mismatch");

  const auto decoded = DecodeSessionNodePath(encoded);
  Expect(decoded.has_value(), "session path decoding should succeed");
  Expect(*decoded == path, "decoded session path should match original");

  const auto root = DecodeSessionNodePath(".");
  Expect(root.has_value() && root->empty(), "root session path should decode to empty path");
}

void TestWorkspaceSharedQuoteAndLineEndings() {
  Expect(QuoteCommandArg("plain-token") == "plain-token", "plain token should not be quoted");
  Expect(QuoteCommandArg("two words") == "'two words'", "spaced token should be quoted");
  Expect(QuoteCommandArg("can't") == "'can'\\''t'", "single quote should be shell escaped");

  Expect(DetectLineEnding("a\r\nb\r\n") == microide::editor::TextViewport::LineEnding::CRLF,
         "crlf ending should be detected");
  Expect(DetectLineEnding("a\nb\n") == microide::editor::TextViewport::LineEnding::LF,
         "lf ending should be detected");
  Expect(DetectLineEnding("a\rb\r") == microide::editor::TextViewport::LineEnding::CR,
         "cr ending should be detected");
}

void TestWorkspaceSharedSplitSyntaxLines() {
  const auto lines = SplitSyntaxLines("alpha\nbeta\ngamma");
  Expect(lines.size() == 3, "split syntax lines should produce three rows");
  Expect(lines[0] == "alpha", "first syntax line mismatch");
  Expect(lines[2] == "gamma", "last syntax line mismatch");
}

void TestWorkspaceSharedReadFileText() {
  const std::filesystem::path temp_root =
      std::filesystem::temp_directory_path() / "microide-workspace-shared-test";
  std::filesystem::remove_all(temp_root);
  std::filesystem::create_directories(temp_root);
  const std::filesystem::path sample = temp_root / "sample.txt";
  WriteFile(sample, "sample text\n");

  const auto content = ReadFileText(sample);
  Expect(content.has_value(), "read file text should load existing file");
  Expect(*content == "sample text\n", "read file text content mismatch");

  const auto missing = ReadFileText(temp_root / "missing.txt");
  Expect(!missing.has_value(), "read file text should fail for missing file");

  std::filesystem::remove_all(temp_root);
}

void TestWorkspaceSharedCommandCompletionHelpers() {
  const std::vector<CommandCompletionCandidate> candidates = {
      {"compare", true},
      {"compact", true},
      {"company", false},
  };
  Expect(CommonPrefix(candidates) == "compa", "common prefix should keep the shared stem");
  Expect(FormatCommandCompletionToken({"two words", true}) == "'two words' ",
         "formatted completion should quote spaced tokens and append a space");
  Expect(JoinCommandArguments({"cmd", "left", "right value"}, 1) == "left right value",
         "joined command arguments should preserve argument order");
}

void TestWorkspaceSharedPathAndCaseHelpers() {
  const auto root = std::filesystem::path("/tmp/project");
  Expect(RelativePathLabel(root, root / "src/main.cpp") == "src/main.cpp",
         "relative path label should shorten paths inside the project");
  Expect(RelativePathLabel(root, std::filesystem::path("/tmp/other/file.txt")) ==
             "/tmp/other/file.txt",
         "relative path label should preserve external paths");
  Expect(ToLower("MiXeD 123") == "mixed 123", "to-lower helper should normalize ASCII letters");
}

void TestWorkspaceSharedPathMutationHelpers() {
  const auto root = std::filesystem::path("/tmp/project");
  const auto nested = root / "src/main.cpp";
  Expect(PathEqualsOrWithin(root, root), "path helper should match identical roots");
  Expect(PathEqualsOrWithin(nested, root), "path helper should detect nested paths");
  Expect(!PathEqualsOrWithin(std::filesystem::path("/tmp/other/file.cpp"), root),
         "path helper should reject unrelated paths");

  Expect(ReplacePathPrefix(nested, root / "src", root / "lib") ==
             std::filesystem::path("/tmp/project/lib/main.cpp"),
         "path prefix replacement should preserve the relative suffix");
  Expect(ReplacePathPrefix(root / "docs", root / "docs", root / "manual") ==
             std::filesystem::path("/tmp/project/manual"),
         "path prefix replacement should replace exact-prefix paths");
}

void TestWorkspaceSharedUtf8Editing() {
  std::string text = "a😀b";
  Expect(RemoveLastUtf8Codepoint(&text), "utf8 deletion should succeed for non-empty text");
  Expect(text == "a😀", "utf8 deletion should remove the last full codepoint");
  Expect(RemoveLastUtf8Codepoint(&text), "utf8 deletion should remove a multibyte codepoint");
  Expect(text == "a", "utf8 deletion should preserve earlier codepoints");
  Expect(RemoveLastUtf8Codepoint(&text), "utf8 deletion should remove the last ascii codepoint");
  Expect(text.empty(), "utf8 deletion should empty the string");
  Expect(!RemoveLastUtf8Codepoint(&text), "utf8 deletion should fail for empty strings");
}

void TestWorkspaceSharedUtf8Metrics() {
  const std::string text = "a😀β";
  Expect(Utf8CodepointCount(text) == 3,
         "utf8 codepoint count should treat multibyte sequences as one codepoint");
  Expect(Utf8ByteOffsetForCodepointCount(text, 0) == 0,
         "utf8 byte offset should return zero for zero codepoints");
  Expect(Utf8ByteOffsetForCodepointCount(text, 1) == 1,
         "utf8 byte offset should advance by one for ASCII codepoints");
  Expect(Utf8ByteOffsetForCodepointCount(text, 2) == 5,
         "utf8 byte offset should skip the full emoji sequence");
  Expect(Utf8ByteOffsetForCodepointCount(text, 3) == text.size(),
         "utf8 byte offset should clamp at the end of the string");
}

void TestWorkspaceSharedCollapseWhitespace() {
  Expect(CollapseWhitespace("  alpha\t beta\n\ngamma  ") == "alpha beta gamma",
         "collapse-whitespace should normalize runs of whitespace to single spaces");
  Expect(CollapseWhitespace("single") == "single",
         "collapse-whitespace should preserve strings without whitespace runs");
  Expect(CollapseWhitespace(" \t\r\n ") == "",
         "collapse-whitespace should discard all-whitespace input");
}

}  // namespace

void RegisterWorkspaceShellSharedCoreTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceShared/ParseCommandLine", TestWorkspaceSharedParseCommandLine);
  AddTest(tests, "WorkspaceShared/ParseCommandLineIncompleteState",
          TestWorkspaceSharedParseCommandLineIncompleteState);
  AddTest(tests, "WorkspaceShared/UiScaleParsing", TestWorkspaceSharedUiScaleParsing);
  AddTest(tests, "WorkspaceShared/SessionEncoding", TestWorkspaceSharedSessionEncoding);
  AddTest(tests, "WorkspaceShared/QuoteAndLineEndings", TestWorkspaceSharedQuoteAndLineEndings);
  AddTest(tests, "WorkspaceShared/SplitSyntaxLines", TestWorkspaceSharedSplitSyntaxLines);
  AddTest(tests, "WorkspaceShared/ReadFileText", TestWorkspaceSharedReadFileText);
  AddTest(tests, "WorkspaceShared/CommandCompletionHelpers",
          TestWorkspaceSharedCommandCompletionHelpers);
  AddTest(tests, "WorkspaceShared/PathAndCaseHelpers", TestWorkspaceSharedPathAndCaseHelpers);
  AddTest(tests, "WorkspaceShared/PathMutationHelpers", TestWorkspaceSharedPathMutationHelpers);
  AddTest(tests, "WorkspaceShared/Utf8Editing", TestWorkspaceSharedUtf8Editing);
  AddTest(tests, "WorkspaceShared/Utf8Metrics", TestWorkspaceSharedUtf8Metrics);
  AddTest(tests, "WorkspaceShared/CollapseWhitespace", TestWorkspaceSharedCollapseWhitespace);
}

}  // namespace microide::tests
