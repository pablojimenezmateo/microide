#include "util/JsonFormat.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "util/Hex.h"

namespace microide::util {

namespace {

// Mirror ParseJson's recursion ceiling so a pathologically nested payload cannot
// overflow the native stack. Legitimate config/JSON nests only a handful of
// levels, so this never trips on real input.
constexpr int kMaxDepth = 200;

constexpr bool IsDigit(char c) { return c >= '0' && c <= '9'; }
constexpr char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// Case-insensitive natural ("human") comparison of two object keys. Letters
// compare case-folded; embedded digit runs compare by numeric value so "item2"
// precedes "item10". Returns <0, 0, or >0. Comparison is over the keys' literal
// text (the bytes between the quotes) — plain for the ASCII identifier keys that
// dominate real JSON; the rare escaped key simply orders by its escaped form.
int NaturalCompare(std::string_view a, std::string_view b) {
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < a.size() && j < b.size()) {
    if (IsDigit(a[i]) && IsDigit(b[j])) {
      // Skip leading zeros, remembering how many, so equal magnitudes tie-break
      // toward fewer leading zeros for a stable, deterministic order.
      const std::size_t zeros_a_start = i;
      const std::size_t zeros_b_start = j;
      while (i < a.size() && a[i] == '0') ++i;
      while (j < b.size() && b[j] == '0') ++j;
      const std::size_t digits_a_start = i;
      const std::size_t digits_b_start = j;
      while (i < a.size() && IsDigit(a[i])) ++i;
      while (j < b.size() && IsDigit(b[j])) ++j;
      const std::size_t len_a = i - digits_a_start;
      const std::size_t len_b = j - digits_b_start;
      if (len_a != len_b) return len_a < len_b ? -1 : 1;  // more significant digits => larger
      const int cmp = a.substr(digits_a_start, len_a).compare(b.substr(digits_b_start, len_b));
      if (cmp != 0) return cmp < 0 ? -1 : 1;
      const std::size_t zeros_a = digits_a_start - zeros_a_start;
      const std::size_t zeros_b = digits_b_start - zeros_b_start;
      if (zeros_a != zeros_b) return zeros_a < zeros_b ? -1 : 1;
      continue;
    }
    const char la = Lower(a[i]);
    const char lb = Lower(b[j]);
    if (la != lb) {
      return static_cast<unsigned char>(la) < static_cast<unsigned char>(lb) ? -1 : 1;
    }
    ++i;
    ++j;
  }
  if (i < a.size()) return 1;
  if (j < b.size()) return -1;
  return 0;
}

struct Formatter {
  std::string_view s;
  std::string_view indent_unit;
  std::size_t pos = 0;
  int depth = 0;

  // Error state. `failed` short-circuits the whole walk once set.
  bool failed = false;
  std::size_t err_off = 0;
  std::string err;

  bool Fail(std::size_t at, std::string message) {
    if (!failed) {
      failed = true;
      err_off = at;
      err = std::move(message);
    }
    return false;
  }

  char Peek() const { return pos < s.size() ? s[pos] : '\0'; }

  void SkipWhitespace() {
    while (pos < s.size() &&
           (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n')) {
      ++pos;
    }
  }

  void WriteIndent(std::string& out, int level) {
    for (int i = 0; i < level; ++i) out.append(indent_unit);
  }

  // Copy a quoted string token verbatim (escapes preserved) into `out`, validating
  // escapes and rejecting raw control bytes exactly as the parser does.
  bool CopyString(std::string& out) {
    const std::size_t start = pos;
    out.push_back('"');
    ++pos;  // opening quote
    while (pos < s.size()) {
      const std::size_t run_start = pos;
      while (pos < s.size() && s[pos] != '"' && s[pos] != '\\' &&
             static_cast<unsigned char>(s[pos]) >= 0x20) {
        ++pos;
      }
      if (pos > run_start) out.append(s.data() + run_start, pos - run_start);
      if (pos >= s.size()) return Fail(start, "unterminated string");
      const unsigned char uc = static_cast<unsigned char>(s[pos]);
      if (uc < 0x20) return Fail(pos, "raw control character in string");
      if (uc == '"') {
        out.push_back('"');
        ++pos;
        return true;
      }
      // Backslash escape: copy it and its payload verbatim after validating.
      out.push_back('\\');
      ++pos;
      if (pos >= s.size()) return Fail(start, "unterminated escape");
      const char esc = s[pos++];
      out.push_back(esc);
      switch (esc) {
        case '"':
        case '\\':
        case '/':
        case 'b':
        case 'f':
        case 'n':
        case 'r':
        case 't':
          break;
        case 'u': {
          if (pos + 4 > s.size()) return Fail(pos, "truncated \\u escape");
          for (int i = 0; i < 4; ++i) {
            if (HexDigitValue(s[pos]) < 0) return Fail(pos, "invalid \\u escape");
            out.push_back(s[pos++]);
          }
          break;
        }
        default:
          return Fail(pos - 1, "invalid escape");
      }
    }
    return Fail(start, "unterminated string");
  }

  // Copy a number token verbatim, validating RFC 8259 grammar.
  bool CopyNumber(std::string& out) {
    const std::size_t start = pos;
    if (Peek() == '-') ++pos;
    if (pos >= s.size() || s[pos] < '0' || s[pos] > '9') {
      return Fail(start, "invalid number");
    }
    if (s[pos] == '0') {
      ++pos;
    } else {
      while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
    }
    if (pos < s.size() && s[pos] == '.') {
      ++pos;
      if (pos >= s.size() || s[pos] < '0' || s[pos] > '9') {
        return Fail(pos, "invalid number");
      }
      while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
    }
    if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
      ++pos;
      if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
      if (pos >= s.size() || s[pos] < '0' || s[pos] > '9') {
        return Fail(pos, "invalid number");
      }
      while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
    }
    out.append(s.data() + start, pos - start);
    return true;
  }

  bool CopyLiteral(std::string& out, std::string_view token) {
    if (pos + token.size() > s.size() || s.substr(pos, token.size()) != token) {
      return Fail(pos, "invalid literal");
    }
    out.append(token);
    pos += token.size();
    return true;
  }

  bool Value(std::string& out, int level) {
    if (failed) return false;
    SkipWhitespace();
    if (pos >= s.size()) return Fail(pos, "unexpected end of input");
    const char c = s[pos];
    switch (c) {
      case '"':
        return CopyString(out);
      case '{':
        return Object(out, level);
      case '[':
        return Array(out, level);
      case 't':
        return CopyLiteral(out, "true");
      case 'f':
        return CopyLiteral(out, "false");
      case 'n':
        return CopyLiteral(out, "null");
      default:
        if (c == '-' || (c >= '0' && c <= '9')) return CopyNumber(out);
        return Fail(pos, "unexpected character");
    }
  }

  // One object member: its sort key (literal text between the quotes) and the
  // fully-formatted `"key": value` text to emit.
  struct Member {
    std::string_view key;
    std::string text;
  };

  bool Object(std::string& out, int level) {
    if (++depth > kMaxDepth) return Fail(pos, "nesting too deep");
    ++pos;  // '{'
    SkipWhitespace();
    if (Peek() == '}') {
      ++pos;
      out.append("{}");
      --depth;
      return true;
    }
    std::vector<Member> members;
    while (true) {
      SkipWhitespace();
      if (Peek() != '"') return Fail(pos, "expected object key");
      Member member;
      const std::size_t key_start = pos;
      if (!CopyString(member.text)) return false;
      // Content between the surrounding quotes drives the ordering.
      member.key = s.substr(key_start + 1, (pos - key_start) - 2);
      SkipWhitespace();
      if (Peek() != ':') return Fail(pos, "expected ':' after key");
      ++pos;
      member.text.push_back(':');
      member.text.push_back(' ');
      if (!Value(member.text, level + 1)) return false;
      members.push_back(std::move(member));
      SkipWhitespace();
      const char c = Peek();
      if (c == ',') {
        ++pos;
        continue;
      }
      if (c == '}') {
        ++pos;
        break;
      }
      return Fail(pos, "expected ',' or '}'");
    }
    // Human-alphabetical ordering of keys, stable so any duplicate keys keep
    // their source order. Subkeys are already sorted by the recursive Value().
    std::stable_sort(members.begin(), members.end(), [](const Member& a, const Member& b) {
      return NaturalCompare(a.key, b.key) < 0;
    });
    out.push_back('{');
    for (std::size_t m = 0; m < members.size(); ++m) {
      if (m != 0) out.push_back(',');
      out.push_back('\n');
      WriteIndent(out, level + 1);
      out.append(members[m].text);
    }
    out.push_back('\n');
    WriteIndent(out, level);
    out.push_back('}');
    --depth;
    return true;
  }

  bool Array(std::string& out, int level) {
    if (++depth > kMaxDepth) return Fail(pos, "nesting too deep");
    ++pos;  // '['
    SkipWhitespace();
    if (Peek() == ']') {
      ++pos;
      out.append("[]");
      --depth;
      return true;
    }
    out.push_back('[');
    bool first = true;
    while (true) {
      if (!first) out.push_back(',');
      first = false;
      out.push_back('\n');
      WriteIndent(out, level + 1);
      // Array element order is semantic — never reordered, only reindented.
      if (!Value(out, level + 1)) return false;
      SkipWhitespace();
      const char c = Peek();
      if (c == ',') {
        ++pos;
        continue;
      }
      if (c == ']') {
        ++pos;
        out.push_back('\n');
        WriteIndent(out, level);
        out.push_back(']');
        --depth;
        return true;
      }
      return Fail(pos, "expected ',' or ']'");
    }
  }
};

}  // namespace

JsonFormatResult FormatJson(std::string_view src, std::string_view indent_unit) {
  Formatter f;
  f.s = src;
  f.indent_unit = indent_unit;
  std::string out;
  out.reserve(src.size() + src.size() / 2 + 16);

  if (!f.Value(out, 0)) {
    return JsonFormatResult{false, {}, f.err_off, std::move(f.err)};
  }
  f.SkipWhitespace();
  if (f.pos != src.size()) {
    return JsonFormatResult{false, {}, f.pos, "trailing characters after JSON value"};
  }
  return JsonFormatResult{true, std::move(out), 0, {}};
}

}  // namespace microide::util
