#pragma once

#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "util/SaturatingMath.h"

namespace microide::util {

// UTF-8 encoding of U+FFFD REPLACEMENT CHARACTER, emitted for malformed input.
inline constexpr std::string_view kUtf8ReplacementChar = "\xEF\xBF\xBD";

enum class LineEnding {
  LF,
  CRLF,
  CR,
};

struct DecodedText {
  std::vector<std::string> lines;
  LineEnding line_ending = LineEnding::LF;
  bool mixed_line_endings = false;
};

std::size_t Utf8SequenceLength(unsigned char lead_byte);
std::size_t Utf8SequenceLength(std::string_view text, std::size_t offset);
// Decode the first UTF-8 codepoint of `glyph`. Returns U+FFFD on malformed or
// empty input. Intended for already-grouped single-glyph slices.
char32_t DecodeUtf8Codepoint(std::string_view glyph);
// Append the UTF-8 encoding of `codepoint` to `out`. Codepoints above U+10FFFF
// are encoded as written (callers are expected to pass valid scalar values).
void AppendUtf8(std::string& out, char32_t codepoint);
// Terminal display column width of a codepoint: 0 for zero-width / combining
// marks, 2 for East Asian wide / fullwidth / emoji-presentation codepoints, and
// 1 otherwise. Matches the layout assumptions of common `wcwidth`/unicode-width
// implementations so grid alignment agrees with TUI applications.
int CodepointDisplayWidth(char32_t codepoint);
bool IsUtf8ContinuationByte(unsigned char byte);
std::size_t PreviousUtf8Boundary(std::string_view text, std::size_t offset);
std::size_t NextUtf8Boundary(std::string_view text, std::size_t offset);
// Longest UTF-8-code-point-aligned prefix of `text` that fits within `max_bytes`:
// the cut backs off any trailing continuation byte so a multi-byte sequence is never
// chopped mid-character. Returns `text.size()` when the whole string already fits.
// This is the shared primitive under every "truncate this string to a byte budget"
// call site (notifications, terminal selection/input, clipboard export, control
// channel, LSP hover/label harvest, plugin provider fields).
std::size_t Utf8ByteBudgetPrefixLength(std::string_view text, std::size_t max_bytes);
// In-place counterpart: resize `text` down to `Utf8ByteBudgetPrefixLength(text,
// max_bytes)`. Returns true when it actually truncated (i.e. the input exceeded the
// budget), so callers can flag/annotate the truncation.
bool TruncateUtf8ToByteBudget(std::string& text, std::size_t max_bytes);
bool RemoveLastUtf8Codepoint(std::string* text);
std::size_t Utf8ByteOffsetForCodepointCount(std::string_view text, std::size_t codepoint_count);
std::size_t Utf8CodepointCount(std::string_view text);
bool IsValidUtf8(std::string_view content);
std::string NormalizeLineEndings(std::string_view text);
void TrimTrailingLineEndings(std::string* text);
// Trim leading and trailing ASCII whitespace (`std::isspace` semantics on the
// unsigned char value: space, tab, CR, LF, VT, FF).
std::string TrimAsciiWhitespace(std::string_view text);
// Lowercase the ASCII A-Z range, leaving every other byte untouched. Safe for
// UTF-8 input because the multi-byte sequences never include 'A'..'Z'.
std::string ToLowerAscii(std::string_view text);
// Lowercase `text` (ASCII A-Z) into `out`, reusing `out`'s existing capacity so
// hot per-line loops avoid allocating a fresh string each call.
void ToLowerAsciiInto(std::string_view text, std::string& out);
// True if `text` contains any ASCII uppercase letter. Used by smart-case search
// to decide whether a query should match case-sensitively.
bool QueryHasUppercaseAscii(std::string_view text);

// Simple 1:1 Unicode case fold for a single scalar. Covers ASCII, Latin-1
// Supplement, Latin Extended-A, Greek, and Cyrillic uppercase letters; every
// other scalar is returned unchanged. Turkish dotted/dotless I is intentionally
// left unfolded (locale-sensitive). Not a full Unicode case-folding table — a
// deterministic best-effort for case-insensitive matching of common scripts.
char32_t SimpleFoldCodepoint(char32_t cp);
// Case-fold `text` into `out` using SimpleFoldCodepoint per scalar. The ASCII
// range takes a no-decode fast path. Malformed byte sequences are copied
// verbatim so a caller mapping matches back to the source keeps byte alignment.
// Reuses `out`'s capacity for hot per-line loops.
void Utf8CaseFoldInto(std::string_view text, std::string& out);
std::string Utf8CaseFold(std::string_view text);
// True if `text` has any scalar whose simple fold differs from itself (i.e. an
// uppercase letter in a covered script). The Unicode-aware analogue of
// QueryHasUppercaseAscii for smart-case search.
bool Utf8QueryHasCaseVariation(std::string_view text);
// True if `cp` is identifier content: ASCII `[A-Za-z0-9_]` plus a permissive
// superset of non-ASCII letters (excludes common punctuation/symbol ranges).
// Used by word motion and identifier-range extraction so multi-byte identifiers
// are not split mid-scalar.
bool Utf8IsIdentifierCodepoint(char32_t cp);
// True when `haystack` contains `needle` as a case-insensitive (ASCII) substring.
// An empty needle matches. Allocation-free.
bool ContainsCaseInsensitiveAscii(std::string_view haystack, std::string_view needle);
// True if `text` is non-empty and every byte is an ASCII digit ('0'..'9').
bool IsAllAsciiDigits(std::string_view text);
// Split `text` on runs of ASCII whitespace, returning views into `text`.
// Leading/trailing whitespace produces no empty tokens. The returned views are
// valid only for the lifetime of `text`.
std::vector<std::string_view> SplitAsciiWhitespace(std::string_view text);
// Split `text` on NUL bytes into records, returning views into `text`. Empty
// records (adjacent NULs) ARE preserved so positional `-z` git output stays
// aligned; a single terminating NUL yields no trailing empty record. Views are
// valid only for the lifetime of `text`.
std::vector<std::string_view> SplitNulDelimited(std::string_view text);
// Bounded variant: stop after collecting at most `max_records` records. Git parsers
// that cap the entries they RETAIN still paid an O(record count) materialization when
// they split the whole (byte-bounded but possibly millions-of-tiny-records) command
// output first. Passing `retained_cap + lookahead` here bounds the transient token
// vector so hostile NUL-heavy output cannot amplify memory before the entry cap.
std::vector<std::string_view> SplitNulDelimited(std::string_view text, std::size_t max_records);
// Replace each run of ASCII whitespace with a single space. Leading whitespace
// is dropped; trailing whitespace, if any, is also dropped.
std::string CollapseAsciiWhitespace(std::string_view text);
// Like CollapseAsciiWhitespace, but also maps the source byte range
// [match_start, match_end) into the collapsed output, writing the mapped range to
// *out_match_start / *out_match_length (clamped to the collapsed text). Lets a
// search preview highlight the matched span. Degrades gracefully if a boundary
// falls inside collapsed whitespace.
std::string CollapseAsciiWhitespaceTrackingMatch(std::string_view text,
                                                 std::size_t match_start,
                                                 std::size_t match_end,
                                                 std::size_t* out_match_start,
                                                 std::size_t* out_match_length);
LineEnding DetectLineEnding(std::string_view text);
DecodedText DecodeLines(std::string_view content);
std::string_view LineEndingSeparator(LineEnding line_ending);
std::string LineEndingLabel(LineEnding line_ending);
LineEnding ParseLineEndingLabel(std::string_view text);
std::vector<std::string> SplitLines(std::string_view content);
// Like SplitLines but returns views into `content` instead of owning copies.
// The returned views are valid only for the lifetime of `content`; callers that
// outlive the source buffer must copy. Used by allocation-sensitive diff paths.
std::vector<std::string_view> SplitLineViews(std::string_view content);
// Bounded variant of SplitLineViews: stop after `max_lines` lines so a parser with a
// retained-entry cap does not first materialize a line view for every line of a huge
// ref/branch listing. Unlike the unbounded form it does NOT synthesize a single empty
// line for empty input (callers here iterate and cap, and an empty listing is empty).
std::vector<std::string_view> SplitLineViews(std::string_view content, std::size_t max_lines);
std::string JoinLines(std::span<const std::string> lines, std::string_view separator);
std::string SerializeLines(std::span<const std::string> lines, LineEnding line_ending);

// TD-2026-07-17-095: streaming line serializer that works over ANY lines-like source
// exposing `size()` and `operator[]` returning something string_view-convertible —
// in particular `editor::LineSpan` over a `TextBuffer` (zero-copy via LineView). Use
// this instead of `SerializeLines(buffer.Snapshot(), ...)` so an LSP didOpen /
// full-sync fallback never materializes a whole-document vector-of-strings before
// building the payload. Kept in `util` (no editor dependency) via the template.
template <typename LinesLike>
std::string SerializeLinesStreaming(const LinesLike& lines, LineEnding line_ending) {
  const std::string_view separator = LineEndingSeparator(line_ending);
  const std::size_t count = lines.size();
  if (count == 0) {
    return {};
  }
  // Exact reserve with saturating adds (mirrors JoinLines) so the final string is
  // built in a single allocation without an intermediate vector.
  std::size_t total = 0;
  for (std::size_t i = 0; i < count; ++i) {
    total = SaturatingAdd(total, std::string_view(lines[i]).size());
  }
  for (std::size_t i = 1; i < count; ++i) {
    total = SaturatingAdd(total, separator.size());
  }
  std::string out;
  if (total != std::numeric_limits<std::size_t>::max()) {
    out.reserve(total);
  }
  for (std::size_t i = 0; i < count; ++i) {
    if (i != 0) {
      out += separator;
    }
    out += std::string_view(lines[i]);
  }
  return out;
}

}  // namespace microide::util
