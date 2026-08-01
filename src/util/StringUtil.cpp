#include "util/StringUtil.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>

#include "util/SaturatingMath.h"

namespace microide::util {

namespace {

std::size_t ClampUtf8Offset(std::string_view text, std::size_t offset) {
  return std::min(offset, text.size());
}

}  // namespace

std::size_t FirstNonAsciiOrByte(std::string_view text, char also_match) {
  constexpr std::uint64_t kHighBits = 0x8080808080808080ULL;
  constexpr std::uint64_t kLowOnes = 0x0101010101010101ULL;
  const std::uint64_t match_word = kLowOnes * static_cast<unsigned char>(also_match);
  std::size_t index = 0;
  // `word & kHighBits` flags any non-ASCII byte; the classic has-zero-byte trick
  // over `word ^ match_word` flags any byte equal to `also_match`. A byte >= 0x80
  // can produce a false positive in the second term, but it is already flagged by
  // the first, so the union is exact and the scalar tail resolves the offset.
  for (; index + sizeof(std::uint64_t) <= text.size(); index += sizeof(std::uint64_t)) {
    std::uint64_t word = 0;
    std::memcpy(&word, text.data() + index, sizeof(word));
    const std::uint64_t marks = word ^ match_word;
    if (((word & kHighBits) | ((marks - kLowOnes) & ~marks & kHighBits)) != 0) {
      break;
    }
  }
  for (; index < text.size(); ++index) {
    const auto byte = static_cast<unsigned char>(text[index]);
    if (byte >= 0x80 || text[index] == also_match) {
      return index;
    }
  }
  return text.size();
}

std::size_t Utf8SequenceLength(unsigned char lead_byte) {
  if (lead_byte <= 0x7F) {
    return 1;
  }
  if (lead_byte >= 0xC2 && lead_byte <= 0xDF) {
    return 2;
  }
  if (lead_byte >= 0xE0 && lead_byte <= 0xEF) {
    return 3;
  }
  if (lead_byte >= 0xF0 && lead_byte <= 0xF4) {
    return 4;
  }
  return 0;
}

bool IsUtf8ContinuationByte(unsigned char byte) {
  return (byte & 0xC0u) == 0x80u;
}

char32_t DecodeUtf8Codepoint(std::string_view glyph) {
  if (glyph.empty()) {
    return 0xFFFD;
  }
  const unsigned char lead = static_cast<unsigned char>(glyph[0]);
  const std::size_t length = Utf8SequenceLength(lead);
  if (length == 0 || length > glyph.size()) {
    return 0xFFFD;
  }
  if (length == 1) {
    return lead;
  }
  static constexpr unsigned char kLeadMask[] = {0, 0, 0x1F, 0x0F, 0x07};
  char32_t codepoint = lead & kLeadMask[length];
  for (std::size_t i = 1; i < length; ++i) {
    const unsigned char byte = static_cast<unsigned char>(glyph[i]);
    if (!IsUtf8ContinuationByte(byte)) {
      return 0xFFFD;
    }
    codepoint = (codepoint << 6) | (byte & 0x3Fu);
  }
  // Reject non-shortest-form (overlong) encodings, UTF-16 surrogates, and
  // out-of-range scalars — the lead-byte-only length classification above does
  // not enforce the second-byte range checks that IsValidUtf8 applies, so these
  // must be caught here to avoid decoding an invalid scalar (e.g. ED A0 80 ->
  // U+D800, E0 80 80 -> overlong U+0000, F4 BF BF BF -> U+13FFFF).
  static constexpr char32_t kMinForLength[] = {0, 0, 0x80, 0x800, 0x10000};
  if (codepoint < kMinForLength[length] || codepoint > 0x10FFFF ||
      (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
    return 0xFFFD;
  }
  return codepoint;
}

void AppendUtf8(std::string& out, char32_t codepoint) {
  // Never emit invalid UTF-8: surrogate scalars and values above U+10FFFF are
  // not encodable and would produce a byte sequence no decoder accepts. Fold
  // them to U+FFFD so callers (JSON \u escapes, terminal input) can never push
  // malformed bytes downstream.
  if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
    codepoint = 0xFFFD;
  }
  if (codepoint <= 0x7F) {
    out += static_cast<char>(codepoint);
  } else if (codepoint <= 0x7FF) {
    out += static_cast<char>(0xC0 | (codepoint >> 6));
    out += static_cast<char>(0x80 | (codepoint & 0x3F));
  } else if (codepoint <= 0xFFFF) {
    out += static_cast<char>(0xE0 | (codepoint >> 12));
    out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (codepoint & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (codepoint >> 18));
    out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (codepoint & 0x3F));
  }
}

namespace {

struct CodepointRange {
  char32_t low;
  char32_t high;
};

bool InSortedRanges(char32_t cp, const CodepointRange* ranges, std::size_t count) {
  std::size_t lo = 0;
  std::size_t hi = count;
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (cp < ranges[mid].low) {
      hi = mid;
    } else if (cp > ranges[mid].high) {
      lo = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

}  // namespace

int CodepointDisplayWidth(char32_t codepoint) {
  // Fast path: printable ASCII is always one column. This is the hot case for
  // typical terminal output, so it skips the range searches below.
  if (codepoint >= 0x20 && codepoint < 0x7F) {
    return 1;
  }
  // NUL and C0/C1 control codes contribute no advance (they are normally
  // handled before reaching the grid, but stay defensive here).
  if (codepoint == 0) {
    return 0;
  }
  if (codepoint < 0x20 || (codepoint >= 0x7F && codepoint < 0xA0)) {
    return 0;
  }

  // Zero-width: combining marks, joiners, variation selectors. Sorted by `low`.
  static constexpr CodepointRange kZeroWidth[] = {
      {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF},
      {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x0610, 0x061A}, {0x064B, 0x065F},
      {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4}, {0x06E7, 0x06E8},
      {0x06EA, 0x06ED}, {0x0711, 0x0711}, {0x0730, 0x074A}, {0x07A6, 0x07B0},
      {0x07EB, 0x07F3}, {0x0816, 0x0819}, {0x081B, 0x0823}, {0x0825, 0x0827},
      {0x0829, 0x082D}, {0x0859, 0x085B}, {0x08E3, 0x0903}, {0x093A, 0x093C},
      {0x093E, 0x094F}, {0x0951, 0x0957}, {0x0962, 0x0963}, {0x0E31, 0x0E31},
      {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E}, {0x200B, 0x200F}, {0x202A, 0x202E},
      {0x2060, 0x2064}, {0x20D0, 0x20F0}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F},
  };
  if (InSortedRanges(codepoint, kZeroWidth, std::size(kZeroWidth))) {
    return 0;
  }

  // East Asian Wide / Fullwidth + emoji presentation. Sorted by `low`.
  static constexpr CodepointRange kWide[] = {
      {0x1100, 0x115F},   {0x2329, 0x232A},   {0x2E80, 0x303E},   {0x3041, 0x33FF},
      {0x3400, 0x4DBF},   {0x4E00, 0x9FFF},   {0xA000, 0xA4CF},   {0xAC00, 0xD7A3},
      {0xF900, 0xFAFF},   {0xFE10, 0xFE19},   {0xFE30, 0xFE6F},   {0xFF00, 0xFF60},
      {0xFFE0, 0xFFE6},   {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E},
      {0x1F191, 0x1F19A}, {0x1F200, 0x1F2FF}, {0x1F300, 0x1F64F}, {0x1F680, 0x1F6FF},
      {0x1F900, 0x1F9FF}, {0x1FA70, 0x1FAFF}, {0x20000, 0x3FFFD},
  };
  if (InSortedRanges(codepoint, kWide, std::size(kWide))) {
    return 2;
  }

  return 1;
}

std::size_t Utf8SequenceLength(std::string_view text, std::size_t offset) {
  if (offset >= text.size()) {
    return 0;
  }

  const unsigned char lead = static_cast<unsigned char>(text[offset]);
  if (lead <= 0x7F) {
    return 1;
  }

  auto continuation = [&](std::size_t count) {
    if (offset + count >= text.size()) {
      return false;
    }
    for (std::size_t i = 1; i <= count; ++i) {
      const unsigned char byte = static_cast<unsigned char>(text[offset + i]);
      if (!IsUtf8ContinuationByte(byte)) {
        return false;
      }
    }
    return true;
  };

  if (lead >= 0xC2 && lead <= 0xDF && continuation(1)) {
    return 2;
  }
  if (lead == 0xE0 && continuation(2)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0xA0 && second <= 0xBF) {
      return 3;
    }
  }
  if (((lead >= 0xE1 && lead <= 0xEC) || (lead >= 0xEE && lead <= 0xEF)) &&
      continuation(2)) {
    return 3;
  }
  if (lead == 0xED && continuation(2)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0x80 && second <= 0x9F) {
      return 3;
    }
  }
  if (lead == 0xF0 && continuation(3)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0x90 && second <= 0xBF) {
      return 4;
    }
  }
  if (lead >= 0xF1 && lead <= 0xF3 && continuation(3)) {
    return 4;
  }
  if (lead == 0xF4 && continuation(3)) {
    const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
    if (second >= 0x80 && second <= 0x8F) {
      return 4;
    }
  }

  return 1;
}

std::size_t PreviousUtf8Boundary(std::string_view text, std::size_t offset) {
  offset = ClampUtf8Offset(text, offset);
  if (offset == 0) {
    return 0;
  }
  std::size_t previous = offset - 1;
  while (previous > 0 &&
         IsUtf8ContinuationByte(static_cast<unsigned char>(text[previous]))) {
    --previous;
  }
  return previous;
}

std::size_t NextUtf8Boundary(std::string_view text, std::size_t offset) {
  offset = ClampUtf8Offset(text, offset);
  if (offset >= text.size()) {
    return text.size();
  }
  return std::min(text.size(), offset + Utf8SequenceLength(text, offset));
}

std::size_t Utf8ByteBudgetPrefixLength(std::string_view text, std::size_t max_bytes) {
  if (text.size() <= max_bytes) {
    return text.size();
  }
  // The byte at `cut` is the first byte that would be dropped; back the cut off any
  // continuation byte (0b10xxxxxx) so the retained prefix ends on a code-point
  // boundary and the straddling multi-byte sequence is dropped whole rather than
  // chopped mid-character.
  std::size_t cut = max_bytes;
  while (cut > 0 && IsUtf8ContinuationByte(static_cast<unsigned char>(text[cut]))) {
    --cut;
  }
  return cut;
}

bool TruncateUtf8ToByteBudget(std::string& text, std::size_t max_bytes) {
  if (text.size() <= max_bytes) {
    return false;
  }
  text.resize(Utf8ByteBudgetPrefixLength(text, max_bytes));
  return true;
}

bool RemoveLastUtf8Codepoint(std::string* text) {
  if (text == nullptr || text->empty()) {
    return false;
  }
  text->erase(PreviousUtf8Boundary(*text, text->size()));
  return true;
}

std::size_t Utf8ByteOffsetForCodepointCount(std::string_view text,
                                            std::size_t codepoint_count) {
  std::size_t offset = 0;
  for (std::size_t count = 0; count < codepoint_count && offset < text.size(); ++count) {
    offset += Utf8SequenceLength(text, offset);
  }
  return offset;
}

std::size_t Utf8CodepointCount(std::string_view text) {
  std::size_t count = 0;
  for (std::size_t offset = 0; offset < text.size();) {
    offset += Utf8SequenceLength(text, offset);
    ++count;
  }
  return count;
}

bool IsValidUtf8(std::string_view content) {
  for (std::size_t offset = 0; offset < content.size();) {
    const std::size_t sequence_length = Utf8SequenceLength(content, offset);
    if (sequence_length == 0) {
      return false;
    }

    const unsigned char lead = static_cast<unsigned char>(content[offset]);
    if (lead > 0x7F && sequence_length == 1) {
      return false;
    }

    offset += sequence_length;
  }
  return true;
}

std::string NormalizeLineEndings(std::string_view text) {
  std::string normalized;
  normalized.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r') {
      normalized.push_back('\n');
      if (i + 1 < text.size() && text[i + 1] == '\n') {
        ++i;
      }
    } else {
      normalized.push_back(text[i]);
    }
  }
  return normalized;
}

void TrimTrailingLineEndings(std::string* text) {
  if (text == nullptr) {
    return;
  }
  while (!text->empty() && (text->back() == '\n' || text->back() == '\r')) {
    text->pop_back();
  }
}

std::string ToLowerAscii(std::string_view text) {
  std::string lowered(text);
  for (char& c : lowered) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + ('a' - 'A'));
    }
  }
  return lowered;
}

void ToLowerAsciiInto(std::string_view text, std::string& out) {
  out.resize(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    out[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
  }
}

bool QueryHasUppercaseAscii(std::string_view text) {
  return std::any_of(text.begin(), text.end(),
                     [](unsigned char c) { return c >= 'A' && c <= 'Z'; });
}

char32_t SimpleFoldCodepoint(char32_t cp) {
  // ASCII fast path (the overwhelmingly common case for source code / paths).
  if (cp < 0x80) {
    return (cp >= 'A' && cp <= 'Z') ? cp + 0x20 : cp;
  }
  // Latin-1 Supplement uppercase letters (À..Ö, Ø..Þ). × (0xD7) is not a letter.
  if ((cp >= 0xC0 && cp <= 0xD6) || (cp >= 0xD8 && cp <= 0xDE)) {
    return cp + 0x20;
  }
  // Latin Extended-A. The block splits into two regular sub-patterns plus a
  // handful of irregular codepoints. Turkish dotted/dotless I (0x130/0x131) is
  // intentionally left unfolded — its correct fold is locale-sensitive.
  if (cp >= 0x100 && cp <= 0x17F) {
    if (cp == 0x130 || cp == 0x131 || cp == 0x138 || cp == 0x149 || cp == 0x178) {
      return cp;  // ĸ, ŉ, İ, ı, Ÿ: irregular / handled elsewhere.
    }
    // Even-upper sub-blocks: 0x100..0x137, 0x14A..0x177 (Ā/ā style pairs).
    if ((cp >= 0x100 && cp <= 0x137) || (cp >= 0x14A && cp <= 0x177)) {
      return (cp % 2 == 0) ? cp + 1 : cp;
    }
    // Odd-upper sub-blocks: 0x139..0x148, 0x179..0x17E (Ĺ/ĺ style pairs).
    if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E)) {
      return (cp % 2 == 1) ? cp + 1 : cp;
    }
    return cp;
  }
  // Greek uppercase Α..Ω (0x391..0x3A9); 0x3A2 is an unassigned hole.
  if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) {
    return cp + 0x20;
  }
  // Cyrillic: А..Я (0x410..0x42F) and the preceding accented block Ѐ..Џ
  // (0x400..0x40F) fold to their lowercase forms.
  if (cp >= 0x410 && cp <= 0x42F) {
    return cp + 0x20;
  }
  if (cp >= 0x400 && cp <= 0x40F) {
    return cp + 0x50;
  }
  return cp;
}

void Utf8CaseFoldInto(std::string_view text, std::string& out) {
  out.clear();
  // A folded string is never longer than a byte-wise ASCII-lowered one for the
  // ranges we cover (each folded scalar occupies the same UTF-8 length), so a
  // size hint avoids reallocations on the common path.
  out.reserve(text.size());
  std::size_t offset = 0;
  while (offset < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[offset]);
    if (lead < 0x80) {  // ASCII fast path, no decode.
      out += (lead >= 'A' && lead <= 'Z') ? static_cast<char>(lead + 0x20)
                                          : static_cast<char>(lead);
      ++offset;
      continue;
    }
    const std::size_t length = Utf8SequenceLength(text, offset);
    if (length == 0) {  // Invalid lead — copy the single byte and advance.
      out += static_cast<char>(lead);
      ++offset;
      continue;
    }
    const std::string_view scalar = text.substr(offset, length);
    const char32_t cp = DecodeUtf8Codepoint(scalar);
    if (cp == 0xFFFD && scalar != kUtf8ReplacementChar) {
      // Malformed sequence — preserve the original bytes verbatim so byte
      // offsets stay meaningful for callers that map matches back to the source.
      out.append(scalar);
    } else {
      AppendUtf8(out, SimpleFoldCodepoint(cp));
    }
    offset += length;
  }
}

std::string Utf8CaseFold(std::string_view text) {
  std::string out;
  Utf8CaseFoldInto(text, out);
  return out;
}

bool Utf8QueryHasCaseVariation(std::string_view text) {
  std::size_t offset = 0;
  while (offset < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[offset]);
    if (lead < 0x80) {
      if (lead >= 'A' && lead <= 'Z') {
        return true;
      }
      ++offset;
      continue;
    }
    const std::size_t length = Utf8SequenceLength(text, offset);
    if (length == 0) {
      ++offset;
      continue;
    }
    const char32_t cp = DecodeUtf8Codepoint(text.substr(offset, length));
    if (cp != 0xFFFD && SimpleFoldCodepoint(cp) != cp) {
      return true;
    }
    offset += length;
  }
  return false;
}

bool IsSearchWordByte(char c) {
  const auto byte = static_cast<unsigned char>(c);
  return byte >= 0x80 || (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') ||
         (byte >= 'A' && byte <= 'Z') || byte == '_';
}

bool SearchMatchStandsAlone(std::string_view text, std::size_t start, std::size_t end) {
  if (start > 0 && start <= text.size() && IsSearchWordByte(text[start - 1])) {
    return false;
  }
  return end >= text.size() || !IsSearchWordByte(text[end]);
}

bool Utf8IsIdentifierCodepoint(char32_t cp) {
  if (cp < 0x80) {
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
           (cp >= '0' && cp <= '9') || cp == '_';
  }
  // Treat any non-ASCII letter-ish scalar as identifier content. This is a
  // deliberately permissive superset (it includes marks/joiners) so word motion
  // and identifier extraction do not split multi-byte identifiers mid-scalar;
  // punctuation and symbol ranges below stay excluded.
  if (cp >= 0x2000 && cp <= 0x206F) return false;   // general punctuation
  if (cp >= 0x2190 && cp <= 0x2BFF) return false;   // arrows / symbols / dingbats
  if (cp >= 0x3000 && cp <= 0x303F) return false;   // CJK symbols & punctuation
  if (cp == 0xA0 || cp == 0x3000) return false;     // no-break / ideographic space
  return true;
}

bool IsAllAsciiDigits(std::string_view text) {
  return !text.empty() &&
         std::all_of(text.begin(), text.end(),
                     [](unsigned char c) { return c >= '0' && c <= '9'; });
}

bool ContainsCaseInsensitiveAscii(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) {
    return true;
  }
  if (needle.size() > haystack.size()) {
    return false;
  }
  // Scan for a first-byte hit before entering the O(needle) compare. The naive
  // nested loop re-entered the inner loop (and re-folded needle[0]) at every
  // haystack position; the settings/font filters run this over every row on each
  // keystroke, so the first-byte reject is the case that has to be tight.
  const char first_lower = ToLowerAsciiChar(needle[0]);
  const char first_upper = ToUpperAsciiChar(needle[0]);
  const std::size_t last = haystack.size() - needle.size();
  for (std::size_t i = 0; i <= last; ++i) {
    const char c = haystack[i];
    if (c != first_lower && c != first_upper) {
      continue;
    }
    std::size_t j = 1;
    for (; j < needle.size(); ++j) {
      if (ToLowerAsciiChar(haystack[i + j]) != ToLowerAsciiChar(needle[j])) {
        break;
      }
    }
    if (j == needle.size()) {
      return true;
    }
  }
  return false;
}

std::vector<std::string_view> SplitAsciiWhitespace(std::string_view text) {
  std::vector<std::string_view> parts;
  std::size_t index = 0;
  while (index < text.size()) {
    while (index < text.size() && IsAsciiSpace(static_cast<unsigned char>(text[index]))) {
      ++index;
    }
    if (index >= text.size()) {
      break;
    }
    const std::size_t start = index;
    while (index < text.size() && !IsAsciiSpace(static_cast<unsigned char>(text[index]))) {
      ++index;
    }
    parts.push_back(text.substr(start, index - start));
  }
  return parts;
}

std::vector<std::string_view> SplitNulDelimited(std::string_view text) {
  std::vector<std::string_view> records;
  std::size_t offset = 0;
  while (offset < text.size()) {
    const std::size_t nul = text.find('\0', offset);
    if (nul == std::string_view::npos) {
      records.push_back(text.substr(offset));
      break;
    }
    records.push_back(text.substr(offset, nul - offset));
    offset = nul + 1;
  }
  return records;
}

std::vector<std::string_view> SplitNulDelimited(std::string_view text, std::size_t max_records) {
  std::vector<std::string_view> records;
  if (max_records == 0) {
    return records;
  }
  records.reserve(std::min<std::size_t>(max_records, 1024));
  std::size_t offset = 0;
  while (offset < text.size() && records.size() < max_records) {
    const std::size_t nul = text.find('\0', offset);
    if (nul == std::string_view::npos) {
      records.push_back(text.substr(offset));
      break;
    }
    records.push_back(text.substr(offset, nul - offset));
    offset = nul + 1;
  }
  return records;
}

std::string CollapseAsciiWhitespace(std::string_view text) {
  std::string collapsed;
  collapsed.reserve(text.size());
  bool saw_whitespace = false;
  for (unsigned char c : text) {
    if (IsAsciiSpace(c)) {
      saw_whitespace = !collapsed.empty();
      continue;
    }
    if (saw_whitespace) {
      collapsed.push_back(' ');
      saw_whitespace = false;
    }
    collapsed.push_back(static_cast<char>(c));
  }
  return collapsed;
}

std::string CollapseAsciiWhitespaceTrackingMatch(std::string_view text,
                                                 std::size_t match_start,
                                                 std::size_t match_end,
                                                 std::size_t* out_match_start,
                                                 std::size_t* out_match_length) {
  std::string collapsed;
  collapsed.reserve(text.size());
  bool saw_whitespace = false;
  std::size_t mapped_start = std::string::npos;
  std::size_t mapped_end = std::string::npos;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (IsAsciiSpace(c)) {
      saw_whitespace = !collapsed.empty();
      // A boundary landing on whitespace maps to where the next emitted byte
      // (or pending space) will sit.
      if (i == match_start && mapped_start == std::string::npos) {
        mapped_start = collapsed.size();
      }
      if (i == match_end && mapped_end == std::string::npos) {
        mapped_end = collapsed.size();
      }
      continue;
    }
    if (saw_whitespace) {
      collapsed.push_back(' ');
      saw_whitespace = false;
    }
    // The match can end within a whitespace run that collapses to this single
    // space: match_end then points at THIS non-whitespace byte while the last
    // matched byte was whitespace. Map the exclusive end to just after the flushed
    // space so the collapsed space stays inside the match (otherwise mapped_end was
    // missed entirely and the highlight collapsed to zero length).
    if (i == match_end && mapped_end == std::string::npos) {
      mapped_end = collapsed.size();
    }
    if (i == match_start && mapped_start == std::string::npos) {
      mapped_start = collapsed.size();
    }
    collapsed.push_back(static_cast<char>(c));
    if (i + 1 == match_end) {
      mapped_end = collapsed.size();
    }
  }
  if (mapped_start == std::string::npos) {
    mapped_start = collapsed.size();
  }
  if (mapped_end == std::string::npos || mapped_end < mapped_start) {
    mapped_end = mapped_start;
  }
  mapped_start = std::min(mapped_start, collapsed.size());
  mapped_end = std::min(mapped_end, collapsed.size());
  if (out_match_start != nullptr) {
    *out_match_start = mapped_start;
  }
  if (out_match_length != nullptr) {
    *out_match_length = mapped_end - mapped_start;
  }
  return collapsed;
}

std::string TrimAsciiWhitespace(std::string_view text) {
  std::size_t start = 0;
  while (start < text.size() && IsAsciiSpace(static_cast<unsigned char>(text[start]))) {
    ++start;
  }
  std::size_t end = text.size();
  while (end > start && IsAsciiSpace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return std::string(text.substr(start, end - start));
}

namespace {

struct LineEndingCounts {
  std::size_t crlf = 0;
  std::size_t lf = 0;
  std::size_t cr = 0;
};

LineEndingCounts CountLineEndings(std::string_view text) {
  LineEndingCounts counts;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r') {
      if (i + 1 < text.size() && text[i + 1] == '\n') {
        ++counts.crlf;
        ++i;
      } else {
        ++counts.cr;
      }
    } else if (text[i] == '\n') {
      ++counts.lf;
    }
  }
  return counts;
}

LineEnding DominantLineEnding(const LineEndingCounts& counts) {
  if (counts.crlf >= counts.lf && counts.crlf >= counts.cr && counts.crlf > 0) {
    return LineEnding::CRLF;
  }
  if (counts.lf >= counts.cr && counts.lf > 0) {
    return LineEnding::LF;
  }
  if (counts.cr > 0) {
    return LineEnding::CR;
  }
  return LineEnding::LF;
}

}  // namespace

LineEnding DetectLineEnding(std::string_view text) {
  return DominantLineEnding(CountLineEndings(text));
}

DecodedText DecodeLines(std::string_view content) {
  DecodedText decoded;

  const LineEndingCounts counts = CountLineEndings(content);
  const std::size_t present_styles =
      (counts.crlf > 0 ? 1 : 0) + (counts.lf > 0 ? 1 : 0) + (counts.cr > 0 ? 1 : 0);
  decoded.mixed_line_endings = present_styles > 1;
  decoded.line_ending = DominantLineEnding(counts);

  std::size_t line_start = 0;
  for (std::size_t i = 0; i < content.size(); ++i) {
    if (content[i] != '\r' && content[i] != '\n') {
      continue;
    }

    decoded.lines.emplace_back(content.substr(line_start, i - line_start));
    if (content[i] == '\r' && i + 1 < content.size() && content[i + 1] == '\n') {
      ++i;
    }
    line_start = i + 1;
  }

  if (line_start <= content.size()) {
    decoded.lines.emplace_back(content.substr(line_start));
  }
  if (decoded.lines.empty()) {
    decoded.lines.push_back("");
  }
  return decoded;
}

std::string_view LineEndingSeparator(LineEnding line_ending) {
  switch (line_ending) {
    case LineEnding::CRLF:
      return "\r\n";
    case LineEnding::CR:
      return "\r";
    case LineEnding::LF:
    default:
      return "\n";
  }
}

std::string LineEndingLabel(LineEnding line_ending) {
  switch (line_ending) {
    case LineEnding::CRLF:
      return "crlf";
    case LineEnding::CR:
      return "cr";
    case LineEnding::LF:
    default:
      return "lf";
  }
}

LineEnding ParseLineEndingLabel(std::string_view text) {
  if (text == "crlf") {
    return LineEnding::CRLF;
  }
  if (text == "cr") {
    return LineEnding::CR;
  }
  return LineEnding::LF;
}

std::vector<std::string> SplitLines(std::string_view content) {
  return DecodeLines(content).lines;
}

std::vector<std::string_view> SplitLineViews(std::string_view content) {
  std::vector<std::string_view> lines;
  std::size_t line_start = 0;
  for (std::size_t i = 0; i < content.size(); ++i) {
    if (content[i] != '\r' && content[i] != '\n') {
      continue;
    }
    lines.push_back(content.substr(line_start, i - line_start));
    if (content[i] == '\r' && i + 1 < content.size() && content[i + 1] == '\n') {
      ++i;
    }
    line_start = i + 1;
  }
  if (line_start <= content.size()) {
    lines.push_back(content.substr(line_start));
  }
  if (lines.empty()) {
    lines.emplace_back();
  }
  return lines;
}

std::vector<std::string_view> SplitLineViews(std::string_view content, std::size_t max_lines) {
  std::vector<std::string_view> lines;
  if (max_lines == 0) {
    return lines;
  }
  lines.reserve(std::min<std::size_t>(max_lines, 1024));
  std::size_t line_start = 0;
  for (std::size_t i = 0; i < content.size() && lines.size() < max_lines; ++i) {
    if (content[i] != '\r' && content[i] != '\n') {
      continue;
    }
    lines.push_back(content.substr(line_start, i - line_start));
    if (content[i] == '\r' && i + 1 < content.size() && content[i + 1] == '\n') {
      ++i;
    }
    line_start = i + 1;
  }
  // Trailing partial line (no terminator) if we still have budget and content left.
  if (lines.size() < max_lines && line_start < content.size()) {
    lines.push_back(content.substr(line_start));
  }
  return lines;
}

std::string JoinLines(std::span<const std::string> lines, std::string_view separator) {
  if (lines.empty()) {
    return {};
  }

  // Accumulate the exact byte total with saturating adds so a pathological set of
  // lines cannot wrap std::size_t and yield a too-small reserve (forcing repeated
  // reallocation) or a wrapped-huge one. Separators are added per-line to sidestep
  // a multiply overflow. A saturated (== max) total is treated as "do not reserve"
  // — such a string could never be held in memory anyway, so we let it grow lazily.
  std::size_t size = 0;
  for (const std::string& line : lines) {
    size = SaturatingAdd(size, line.size());
  }
  for (std::size_t i = 1; i < lines.size(); ++i) {
    size = SaturatingAdd(size, separator.size());
  }

  std::string joined;
  if (size != std::numeric_limits<std::size_t>::max()) {
    joined.reserve(size);
  }
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) {
      joined += separator;
    }
    joined += lines[i];
  }
  return joined;
}

std::string SerializeLines(std::span<const std::string> lines, LineEnding line_ending) {
  return JoinLines(lines, LineEndingSeparator(line_ending));
}

}  // namespace microide::util
