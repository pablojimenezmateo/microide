#include "TestSupport.h"

#include "editor/TextViewport.h"
#include "workspace/lsp/LspPositionEncoding.h"
#include "workspace/lsp/LspViewportPositions.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

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

// ---- exhaustive round trips -------------------------------------------------
//
// Every LSP request and every diagnostic crosses these two functions, in both
// directions, once per position. They are each other's inverse on UTF-8
// boundaries, and that has to hold for EVERY column of every line -- not just
// the few literals a hand-written case happens to pick.
//
// The corpus deliberately includes ill-formed UTF-8. A source file is arbitrary
// bytes; a server that reports a position inside a truncated sequence must be
// clamped, not walked off the end of the line.

// Well-formed lines. "Never split a codepoint" is only a meaningful assertion
// here, because only here is a codepoint boundary well defined.
const std::vector<std::string>& WellFormedCorpus() {
  static const std::vector<std::string> corpus = {
      "",
      "a",
      "plain ascii line",
      "caf\xC3\xA9 x",                                  // 2-byte
      "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E",              // 3-byte CJK
      "a\xF0\x9F\x98\x80\x62",                          // 4-byte: a surrogate pair in utf-16
      "\xF0\x9F\x98\x80\xF0\x9F\x99\x83",                    // two 4-byte in a row
      "e\xCC\x81 combining",                            // base + combining mark
      "mix \xC3\xA9 \xE6\x97\xA5 \xF0\x9F\x98\x80 end",
      "\t\tindented\t",
      "trailing multibyte \xC3\xA9",
  };
  return corpus;
}

// Ill-formed bytes: a lead byte with its continuations cut off, a stray
// continuation, an overlong form, and bytes that are not UTF-8 at all. A source
// file is arbitrary bytes, so these must still clamp, stay ordered and converge
// -- the lenient decoder's idea of where a "codepoint" starts is its own, so the
// boundary assertion does not apply to them.
const std::vector<std::string>& IllFormedCorpus() {
  static const std::vector<std::string> corpus = {
      "bad \xE6\x97 truncated",
      "bad \x80 stray",
      "bad \xC0\x80 overlong",
      "\xFF\xFE not utf-8 at all",
      "\xF0\x9F truncated emoji",
  };
  return corpus;
}

std::vector<std::string> EncodingCorpus() {
  std::vector<std::string> all = WellFormedCorpus();
  const std::vector<std::string>& bad = IllFormedCorpus();
  all.insert(all.end(), bad.begin(), bad.end());
  return all;
}

bool IsUtf8Boundary(std::string_view line, std::size_t index) {
  return index >= line.size() ||
         (static_cast<unsigned char>(line[index]) & 0xC0) != 0x80;
}

std::string Where(std::string_view line, std::size_t index, PositionEncoding encoding) {
  const char* name = encoding == PositionEncoding::Utf8    ? "utf-8"
                     : encoding == PositionEncoding::Utf16 ? "utf-16"
                                                           : "utf-32";
  std::string bytes;
  for (const char c : line) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%02X ", static_cast<unsigned char>(c));
    bytes += buffer;
  }
  return " [encoding=" + std::string(name) + " index=" + std::to_string(index) +
         " line=" + bytes + "]";
}

// A byte column that IS a codepoint boundary must survive byte -> character ->
// byte unchanged. Anything else silently moves an edit, a rename, or a
// diagnostic squiggle off its target.
void TestByteColumnRoundTripsThroughEveryEncoding() {
  std::size_t checked = 0;
  for (const std::string& line : WellFormedCorpus()) {
    for (const PositionEncoding encoding :
         {PositionEncoding::Utf8, PositionEncoding::Utf16, PositionEncoding::Utf32}) {
      for (std::size_t column = 0; column <= line.size(); ++column) {
        if (!IsUtf8Boundary(line, column)) {
          continue;
        }
        const std::size_t character = ByteColumnToLspCharacter(line, column, encoding);
        const std::size_t back = LspCharacterToByteColumn(line, character, encoding);
        Expect(back == column,
               "byte column " + std::to_string(column) + " round-tripped to " +
                   std::to_string(back) + Where(line, column, encoding));
        ++checked;
      }
    }
  }
  Expect(checked > 250, "the corpus must actually exercise columns, checked " +
                            std::to_string(checked));
}

// The other direction cannot be an exact round trip -- a character offset inside
// a multi-unit codepoint snaps to that codepoint's start -- but it must be
// IDEMPOTENT: converting the snapped result again lands in the same place.
// A conversion that drifts by one on each hop walks a long-lived diagnostic off
// its symbol as the file is edited.
void TestCharacterOffsetConversionIsIdempotent() {
  const std::vector<std::string> well_formed = WellFormedCorpus();
  for (const std::string& line : EncodingCorpus()) {
    const bool check_boundary =
        std::find(well_formed.begin(), well_formed.end(), line) != well_formed.end();
    for (const PositionEncoding encoding :
         {PositionEncoding::Utf8, PositionEncoding::Utf16, PositionEncoding::Utf32}) {
      for (std::size_t character = 0; character <= line.size() + 4; ++character) {
        const std::size_t byte_once = LspCharacterToByteColumn(line, character, encoding);
        Expect(byte_once <= line.size(),
               "a character offset must clamp inside the line, got " +
                   std::to_string(byte_once) + Where(line, character, encoding));
        Expect(!check_boundary || IsUtf8Boundary(line, byte_once),
               "a converted byte column must land on a codepoint boundary" +
                   Where(line, character, encoding));
        const std::size_t character_again = ByteColumnToLspCharacter(line, byte_once, encoding);
        const std::size_t byte_twice =
            LspCharacterToByteColumn(line, character_again, encoding);
        Expect(byte_twice == byte_once,
               "converting twice moved the column from " + std::to_string(byte_once) + " to " +
                   std::to_string(byte_twice) + Where(line, character, encoding));
      }
    }
  }
}

// Both directions are monotonic: a column further along the line never maps to
// an earlier offset. A non-monotonic mapping inverts a range, and an inverted
// range is applied as a deletion of the wrong span.
void TestConversionIsMonotonicInBothDirections() {
  for (const std::string& line : EncodingCorpus()) {
    for (const PositionEncoding encoding :
         {PositionEncoding::Utf8, PositionEncoding::Utf16, PositionEncoding::Utf32}) {
      // Only boundary columns: the editor stores caret and range columns as
      // codepoint boundaries, so a column pointing INTO a sequence is not an
      // input this direction ever receives. Asking anyway measures how a
      // truncated slice happens to decode, which is not a contract.
      std::size_t previous_character = 0;
      for (std::size_t column = 0; column <= line.size(); ++column) {
        if (!IsUtf8Boundary(line, column)) {
          continue;
        }
        const std::size_t character = ByteColumnToLspCharacter(line, column, encoding);
        Expect(character >= previous_character,
               "byte -> character went backwards at column " + std::to_string(column) +
                   Where(line, column, encoding));
        previous_character = character;
      }
      std::size_t previous_byte = 0;
      for (std::size_t character = 0; character <= line.size() + 4; ++character) {
        const std::size_t byte_column = LspCharacterToByteColumn(line, character, encoding);
        Expect(byte_column >= previous_byte,
               "character -> byte went backwards at offset " + std::to_string(character) +
                   Where(line, character, encoding));
        previous_byte = byte_column;
      }
    }
  }
}

// utf-8 is the encoding this client advertises first, so it is the one clangd
// and rust-analyzer actually use. It must be a pure pass-through -- any work at
// all here is work on every position of every request.
void TestUtf8EncodingIsAnExactPassThrough() {
  for (const std::string& line : EncodingCorpus()) {
    for (std::size_t column = 0; column <= line.size() + 4; ++column) {
      const std::size_t clamped = std::min(column, line.size());
      Expect(ByteColumnToLspCharacter(line, column, PositionEncoding::Utf8) == clamped,
             "utf-8 byte -> character is the identity, clamped" +
                 Where(line, column, PositionEncoding::Utf8));
    }
  }
}

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

// TD-2026-07-17-089: an outbound line/column past INT_MAX must saturate to INT_MAX
// rather than wrapping to a negative value (which would ask the server about the
// wrong location). LspLineView tolerates an out-of-range line, so we can pass a
// huge line index directly.
void TestViewportOutboundPositionSaturates() {
  const TextViewport viewport = MakeViewport("abc");
  constexpr std::size_t kHuge = static_cast<std::size_t>(std::numeric_limits<int>::max()) + 100;
  const LspClient::Position pos =
      ByteColumnToLspPosition(viewport, kHuge, kHuge, PositionEncoding::Utf8);
  Expect(pos.line == std::numeric_limits<int>::max(),
         "an out-of-int line saturates to INT_MAX, never wraps negative");
  Expect(pos.character >= 0, "the outbound character is never negative after saturation");
  // The shared helper saturates directly.
  Expect(microide::workspace::SaturateToLspInt(kHuge) == std::numeric_limits<int>::max(),
         "SaturateToLspInt clamps an over-range size_t to INT_MAX");
  Expect(microide::workspace::SaturateToLspInt(42) == 42, "SaturateToLspInt is identity in range");
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
  AddTest(tests, "LspPositionEncoding/ByteColumnRoundTripsThroughEveryEncoding",
          TestByteColumnRoundTripsThroughEveryEncoding);
  AddTest(tests, "LspPositionEncoding/CharacterOffsetConversionIsIdempotent",
          TestCharacterOffsetConversionIsIdempotent);
  AddTest(tests, "LspPositionEncoding/ConversionIsMonotonicInBothDirections",
          TestConversionIsMonotonicInBothDirections);
  AddTest(tests, "LspPositionEncoding/Utf8EncodingIsAnExactPassThrough",
          TestUtf8EncodingIsAnExactPassThrough);
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
  AddTest(tests, "LspViewportPositions/OutboundPositionSaturates",
          TestViewportOutboundPositionSaturates);
}

}  // namespace microide::tests
