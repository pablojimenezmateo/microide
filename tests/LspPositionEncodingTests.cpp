#include "TestSupport.h"

#include "editor/TextViewport.h"
#include "workspace/LspPositionEncoding.h"
#include "workspace/LspViewportPositions.h"

namespace microide::tests {
namespace {

using microide::editor::TextViewport;
using microide::workspace::ByteColumnToLspPosition;
using microide::workspace::LspInboundColumn;
using microide::workspace::LspLineView;
using microide::workspace::LspPositionToByteColumn;
using microide::workspace::LspRangeToEditorRange;
using microide::workspace::lsp_encoding::ByteColumnToLspCharacter;
using microide::workspace::lsp_encoding::LspCharacterToByteColumn;
using microide::workspace::lsp_encoding::ParsePositionEncoding;
using microide::workspace::lsp_encoding::PositionEncoding;
using LspClient = microide::workspace::LspClient;

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

// The viewport-aware helpers (LspViewportPositions.h) resolve the affected line
// in a TextViewport, then map its column through the negotiated encoding. These
// replaced three hand-rolled copies; the tests pin the shared behavior, including
// the non-ASCII skew that a per-copy drift would have introduced.
TextViewport MakeViewport(std::string_view content) {
  TextViewport viewport;
  viewport.LoadContent(std::string(content) + "\n", "/t.cpp");
  return viewport;
}

void TestViewportLineView() {
  const TextViewport viewport = MakeViewport(kCafe);
  Expect(LspLineView(viewport, 0) == kCafe, "line 0 view returns the line text");
  Expect(LspLineView(viewport, 5).empty(), "out-of-range line view is empty");
}

void TestViewportOutboundInboundRoundTrip() {
  const TextViewport viewport = MakeViewport(kCafe);  // "café x": space at byte 5, utf-16 unit 4.
  // Outbound: editor byte column 5 -> utf-16 unit 4.
  const LspClient::Position pos = ByteColumnToLspPosition(viewport, 0, 5, PositionEncoding::Utf16);
  Expect(pos.line == 0 && pos.character == 4, "byte col 5 -> utf-16 position (0,4)");
  // Inbound: utf-16 unit 4 -> editor byte column 5.
  Expect(LspPositionToByteColumn(viewport, 0, 4, PositionEncoding::Utf16) == 5,
         "utf-16 unit 4 -> byte col 5");
}

void TestViewportInboundColumnFastPaths() {
  const TextViewport viewport = MakeViewport(kCafe);
  // Null viewport (file not open) passes the character through as a raw byte.
  Expect(LspInboundColumn(nullptr, 0, 4, PositionEncoding::Utf16) == 4,
         "null viewport passes the offset through");
  // utf-8 short-circuits without touching the line text.
  Expect(LspInboundColumn(&viewport, 0, 4, PositionEncoding::Utf8) == 4, "utf-8 passes through");
  // utf-16 converts through the resolved line.
  Expect(LspInboundColumn(&viewport, 0, 4, PositionEncoding::Utf16) == 5, "utf-16 converts to byte 5");
}

void TestViewportRangeToEditorRange() {
  const TextViewport viewport = MakeViewport(kCafe);  // space at unit 4/byte 5, 'x' at unit 5/byte 6.
  const editor::SelectionRange range = LspRangeToEditorRange(
      viewport, LspClient::Range{{0, 4}, {0, 5}}, PositionEncoding::Utf16);
  Expect(range.start.line == 0 && range.start.column == 5, "range start utf-16 unit 4 -> byte 5");
  Expect(range.end.line == 0 && range.end.column == 6, "range end utf-16 unit 5 -> byte 6");
}

}  // namespace

void RegisterLspPositionEncodingTests(std::vector<TestCase>& tests) {
  AddTest(tests, "LspPositionEncoding/ParsePositionEncoding", TestParsePositionEncoding);
  AddTest(tests, "LspPositionEncoding/Utf16RoundTrip", TestUtf16RoundTrip);
  AddTest(tests, "LspPositionEncoding/Utf16SurrogatePair", TestUtf16SurrogatePair);
  AddTest(tests, "LspPositionEncoding/Utf32AndUtf8", TestUtf32AndUtf8);
  AddTest(tests, "LspPositionEncoding/ClampingBeyondLine", TestClampingBeyondLine);
  AddTest(tests, "LspViewportPositions/LineView", TestViewportLineView);
  AddTest(tests, "LspViewportPositions/OutboundInboundRoundTrip",
          TestViewportOutboundInboundRoundTrip);
  AddTest(tests, "LspViewportPositions/InboundColumnFastPaths", TestViewportInboundColumnFastPaths);
  AddTest(tests, "LspViewportPositions/RangeToEditorRange", TestViewportRangeToEditorRange);
}

}  // namespace microide::tests
