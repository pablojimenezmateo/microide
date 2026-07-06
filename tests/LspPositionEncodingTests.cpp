#include "TestSupport.h"

#include "workspace/LspPositionEncoding.h"

namespace microide::tests {
namespace {

using microide::workspace::lsp_encoding::ByteColumnToLspCharacter;
using microide::workspace::lsp_encoding::LspCharacterToByteColumn;
using microide::workspace::lsp_encoding::ParsePositionEncoding;
using microide::workspace::lsp_encoding::PositionEncoding;

// "café x" — 'é' (U+00E9) is 2 UTF-8 bytes, 1 UTF-16 unit, 1 codepoint. So the
// space after "café" is at byte 5, utf-16 unit 4, codepoint 4.
constexpr std::string_view kCafe = "caf\xC3\xA9 x";

// "a😀b" — the emoji U+1F600 is 4 UTF-8 bytes, 2 UTF-16 units (surrogate pair),
// 1 codepoint. 'b' is at byte 5, utf-16 unit 3, codepoint 2.
constexpr std::string_view kEmoji = "a\xF0\x9F\x98\x80\x62";

void TestParsePositionEncoding() {
  Expect(ParsePositionEncoding("utf-8") == PositionEncoding::Utf8, "utf-8 parses");
  Expect(ParsePositionEncoding("utf-16") == PositionEncoding::Utf16, "utf-16 parses");
  Expect(ParsePositionEncoding("utf-32") == PositionEncoding::Utf32, "utf-32 parses");
  Expect(ParsePositionEncoding("") == PositionEncoding::Utf16, "empty defaults to utf-16");
  Expect(ParsePositionEncoding("weird") == PositionEncoding::Utf16, "unknown defaults to utf-16");
}

void TestUtf16RoundTrip() {
  // The space after café: utf-16 unit 4 <-> byte 5.
  Expect(LspCharacterToByteColumn(kCafe, 4, PositionEncoding::Utf16) == 5,
         "utf-16 unit 4 maps past the 2-byte é to byte 5");
  Expect(ByteColumnToLspCharacter(kCafe, 5, PositionEncoding::Utf16) == 4,
         "byte 5 maps back to utf-16 unit 4");
  // 'x' at the end: utf-16 unit 5 <-> byte 6.
  Expect(LspCharacterToByteColumn(kCafe, 5, PositionEncoding::Utf16) == 6, "x at byte 6");
  Expect(ByteColumnToLspCharacter(kCafe, 6, PositionEncoding::Utf16) == 5, "x back to unit 5");
}

void TestUtf16SurrogatePair() {
  // 'b' after the emoji: 2 surrogate units for the emoji + 1 for 'a' = unit 3, byte 5.
  Expect(LspCharacterToByteColumn(kEmoji, 3, PositionEncoding::Utf16) == 5,
         "utf-16 unit 3 clears the surrogate pair to byte 5");
  Expect(ByteColumnToLspCharacter(kEmoji, 5, PositionEncoding::Utf16) == 3, "byte 5 back to unit 3");
  // A character offset landing INSIDE the surrogate pair (unit 2) snaps to the
  // codepoint start (byte 1, just after 'a').
  Expect(LspCharacterToByteColumn(kEmoji, 2, PositionEncoding::Utf16) == 1,
         "an offset inside the surrogate pair snaps to the codepoint start");
}

void TestUtf32AndUtf8() {
  // utf-32 counts codepoints: the space after café is codepoint 4 <-> byte 5.
  Expect(LspCharacterToByteColumn(kCafe, 4, PositionEncoding::Utf32) == 5, "utf-32 cp4 -> byte 5");
  Expect(ByteColumnToLspCharacter(kCafe, 5, PositionEncoding::Utf32) == 4, "byte 5 -> utf-32 cp4");
  // utf-8 is a pass-through (byte == unit), clamped to the line length.
  Expect(LspCharacterToByteColumn(kCafe, 3, PositionEncoding::Utf8) == 3, "utf-8 passes bytes");
  Expect(LspCharacterToByteColumn(kCafe, 999, PositionEncoding::Utf8) == kCafe.size(),
         "utf-8 clamps to line length");
}

void TestClampingBeyondLine() {
  Expect(LspCharacterToByteColumn(kCafe, 999, PositionEncoding::Utf16) == kCafe.size(),
         "utf-16 clamps past end to line length");
  Expect(ByteColumnToLspCharacter(kCafe, 999, PositionEncoding::Utf16) == 6,
         "byte clamp past end counts all 6 utf-16 units");
  Expect(LspCharacterToByteColumn("", 3, PositionEncoding::Utf16) == 0, "empty line -> byte 0");
}

}  // namespace

void RegisterLspPositionEncodingTests(std::vector<TestCase>& tests) {
  AddTest(tests, "LspPositionEncoding/ParsePositionEncoding", TestParsePositionEncoding);
  AddTest(tests, "LspPositionEncoding/Utf16RoundTrip", TestUtf16RoundTrip);
  AddTest(tests, "LspPositionEncoding/Utf16SurrogatePair", TestUtf16SurrogatePair);
  AddTest(tests, "LspPositionEncoding/Utf32AndUtf8", TestUtf32AndUtf8);
  AddTest(tests, "LspPositionEncoding/ClampingBeyondLine", TestClampingBeyondLine);
}

}  // namespace microide::tests
