#include "util/JsonValue.h"

#include <cassert>
#include <charconv>
#include <cmath>
#include <cstring>
#include <sstream>

#include "util/Hex.h"
#include "util/Parse.h"
#include "util/StringUtil.h"

namespace microide::util {

const JsonValue& JsonValue::operator[](std::string_view key) const {
  static const JsonValue kNull;
  if (const auto* o = std::get_if<JsonObject>(&v)) {
    auto it = o->find(key);  // transparent lookup: no temporary std::string
    if (it != o->end()) return it->second;
  }
  return kNull;
}

const JsonValue& JsonValue::operator[](std::size_t idx) const {
  static const JsonValue kNull;
  if (const auto* a = std::get_if<JsonArray>(&v)) {
    if (idx < a->size()) return (*a)[idx];
  }
  return kNull;
}

bool JsonValue::HasKey(std::string_view key) const {
  if (const auto* o = std::get_if<JsonObject>(&v)) {
    return o->find(key) != o->end();  // transparent lookup: no temporary std::string
  }
  return false;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

namespace {

// Bounds recursion so a hostile deeply-nested payload (e.g. `[[[[...]]]]` from a
// control-spec file, a socket request line, or an instance descriptor) cannot
// overflow the native stack. Control/DAP/LSP messages nest only a handful of
// levels, so this ceiling never trips on legitimate input.
constexpr int kMaxParseDepth = 200;

struct Parser {
  std::string_view src;
  std::size_t pos = 0;
  int depth = 0;

  // RAII depth counter: incremented on entry to each array/object, decremented
  // on scope exit regardless of which early return fires.
  struct DepthGuard {
    Parser& parser;
    bool within_limit;
    explicit DepthGuard(Parser& p) : parser(p), within_limit(++p.depth <= kMaxParseDepth) {}
    ~DepthGuard() { --parser.depth; }
    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;
  };

  char Peek() const { return pos < src.size() ? src[pos] : '\0'; }
  char Advance() { return pos < src.size() ? src[pos++] : '\0'; }

  void SkipWhitespace() {
    while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\r' ||
                                src[pos] == '\n')) {
      ++pos;
    }
  }

  bool Expect(char c) {
    if (Peek() == c) {
      ++pos;
      return true;
    }
    return false;
  }

  // Returns nullopt on error.
  std::optional<JsonValue> ParseValue() {
    SkipWhitespace();
    const char c = Peek();
    if (c == '"') return ParseString();
    if (c == '{') return ParseObject();
    if (c == '[') return ParseArray();
    if (c == 't') return ParseLiteral("true", JsonValue(true));
    if (c == 'f') return ParseLiteral("false", JsonValue(false));
    if (c == 'n') return ParseLiteral("null", JsonValue(nullptr));
    if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber();
    return std::nullopt;
  }

  std::optional<JsonValue> ParseLiteral(const char* token, JsonValue value) {
    const std::size_t len = std::strlen(token);
    if (pos + len > src.size()) return std::nullopt;
    if (src.substr(pos, len) != token) return std::nullopt;
    pos += len;
    return value;
  }

  std::optional<JsonValue> ParseNumber() {
    const std::size_t start = pos;
    bool is_float = false;
    if (Peek() == '-') ++pos;
    // Integer part: RFC 8259 requires at least one digit after the optional
    // sign, and a leading '0' must stand alone (reject a lone '-' and "01").
    if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') return std::nullopt;
    if (src[pos] == '0') {
      ++pos;
    } else {
      while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
    }
    if (pos < src.size() && src[pos] == '.') {
      is_float = true;
      ++pos;
      // A decimal point must be followed by at least one digit (reject "1.").
      if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') return std::nullopt;
      while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
    }
    if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
      is_float = true;
      ++pos;
      if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) ++pos;
      // An exponent must carry at least one digit (reject "1e" and "1e+").
      if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') return std::nullopt;
      while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
    }
    const std::string_view tok = src.substr(start, pos - start);
    if (is_float) {
      const auto parsed = ParseDouble(tok);
      if (!parsed.has_value()) return std::nullopt;
      return JsonValue(*parsed);
    }
    const auto parsed = ParseInt64(tok);
    if (parsed.has_value()) return JsonValue(*parsed);
    // A syntactically valid JSON integer literal that overflows int64 (e.g. a
    // uint64-range handle/id/address emitted by a DAP adapter or LSP server) must
    // not abort the entire message parse — that would silently drop the whole
    // response and stall the outstanding request. Fall back to a lossy double so
    // the surrounding value still resolves.
    const auto as_double = ParseDouble(tok);
    if (!as_double.has_value()) return std::nullopt;
    return JsonValue(*as_double);
  }

  std::optional<JsonValue> ParseString() {
    if (!Expect('"')) return std::nullopt;
    std::string result;
    while (pos < src.size()) {
      // Bulk-copy the run of ordinary bytes up to the next quote or backslash in a
      // single append instead of byte-by-byte; escape-free strings (nearly all of
      // them on the LSP/DAP inbound hot path) then cost one memcpy rather than N
      // single-byte operator+= calls.
      const std::size_t run_start = pos;
      while (pos < src.size() && src[pos] != '"' && src[pos] != '\\' &&
             static_cast<unsigned char>(src[pos]) >= 0x20) {
        ++pos;
      }
      if (pos > run_start) {
        result.append(src.data() + run_start, pos - run_start);
      }
      if (pos >= src.size()) break;  // no closing quote before end of input
      // RFC 8259 forbids raw control bytes (< 0x20) inside a string; they must be
      // escaped. The run above stops on such a byte (it is not '"' or '\\'), so a
      // control byte here is a hard reject. Escaped \n \t \r etc. stay valid: they
      // arrive as a '\\' handled by the escape switch below, never as a raw byte.
      if (static_cast<unsigned char>(src[pos]) < 0x20) return std::nullopt;
      const char c = src[pos++];
      if (c == '"') return JsonValue(std::move(result));
      // c is a backslash: decode the escape sequence.
      if (pos >= src.size()) return std::nullopt;
      const char esc = Advance();
      switch (esc) {
        case '"': result += '"'; break;
        case '\\': result += '\\'; break;
        case '/': result += '/'; break;
        case 'b': result += '\b'; break;
        case 'f': result += '\f'; break;
        case 'n': result += '\n'; break;
        case 'r': result += '\r'; break;
        case 't': result += '\t'; break;
        case 'u': {
          if (pos + 4 > src.size()) return std::nullopt;
          unsigned int cp = 0;
          for (int i = 0; i < 4; ++i) {
            const int d = HexDigitValue(Advance());
            if (d < 0) return std::nullopt;
            cp = (cp << 4) | static_cast<unsigned int>(d);
          }
          // Surrogate pair
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (pos + 6 > src.size() || src[pos] != '\\' || src[pos + 1] != 'u') {
              return std::nullopt;
            }
            pos += 2;
            unsigned int lo = 0;
            for (int i = 0; i < 4; ++i) {
              const int d = HexDigitValue(Advance());
              if (d < 0) return std::nullopt;
              lo = (lo << 4) | static_cast<unsigned int>(d);
            }
            if (lo < 0xDC00 || lo > 0xDFFF) return std::nullopt;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          // A lone low surrogate (0xDC00–0xDFFF) never followed a high surrogate
          // above, so it is not a valid scalar value. Emitting it would produce
          // invalid UTF-8 (a CESU-8 ED B0 80 sequence) and poison every consumer
          // that assumes host strings satisfy util::IsValidUtf8. Reject it, matching
          // the lone-high-surrogate rejection above.
          if (cp >= 0xDC00 && cp <= 0xDFFF) return std::nullopt;
          AppendUtf8(result, static_cast<char32_t>(cp));
          break;
        }
        default: return std::nullopt;
      }
    }
    return std::nullopt;  // unterminated
  }

  std::optional<JsonValue> ParseArray() {
    DepthGuard guard(*this);
    if (!guard.within_limit) return std::nullopt;
    if (!Expect('[')) return std::nullopt;
    JsonArray arr;
    SkipWhitespace();
    if (Expect(']')) return JsonValue(std::move(arr));
    while (true) {
      auto elem = ParseValue();
      if (!elem) return std::nullopt;
      arr.push_back(std::move(*elem));
      SkipWhitespace();
      if (Expect(']')) return JsonValue(std::move(arr));
      if (!Expect(',')) return std::nullopt;
    }
  }

  std::optional<JsonValue> ParseObject() {
    DepthGuard guard(*this);
    if (!guard.within_limit) return std::nullopt;
    if (!Expect('{')) return std::nullopt;
    JsonObject obj;
    SkipWhitespace();
    if (Expect('}')) return JsonValue(std::move(obj));
    while (true) {
      SkipWhitespace();
      auto key = ParseString();
      if (!key || !key->IsString()) return std::nullopt;
      SkipWhitespace();
      if (!Expect(':')) return std::nullopt;
      auto val = ParseValue();
      if (!val) return std::nullopt;
      obj[key->AsString()] = std::move(*val);
      SkipWhitespace();
      if (Expect('}')) return JsonValue(std::move(obj));
      if (!Expect(',')) return std::nullopt;
    }
  }
};

}  // namespace

std::optional<JsonValue> ParseJson(std::string_view text) {
  Parser p{text};
  auto result = p.ParseValue();
  if (!result) return std::nullopt;
  p.SkipWhitespace();
  if (p.pos != text.size()) return std::nullopt;  // trailing garbage
  return result;
}

// ---------------------------------------------------------------------------
// Serializer
// ---------------------------------------------------------------------------

namespace {

void AppendEscaped(std::string& out, const std::string& s) {
  out += '"';
  const std::size_t n = s.size();
  std::size_t i = 0;
  while (i < n) {
    // Bulk-append the run of bytes that need no escaping (>= 0x20, not '"' or '\')
    // in one shot instead of byte-by-byte; the overwhelming majority of every
    // outgoing string copies verbatim.
    const std::size_t run_start = i;
    while (i < n) {
      const unsigned char uc = static_cast<unsigned char>(s[i]);
      if (uc == '"' || uc == '\\' || uc < 0x20) break;
      ++i;
    }
    if (i > run_start) {
      out.append(s.data() + run_start, i - run_start);
    }
    if (i >= n) break;
    const char c = s[i++];
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: {
        char buf[7];
        snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
        out += buf;
      }
    }
  }
  out += '"';
}

void AppendValue(std::string& out, const JsonValue& val, int depth = 0) {
  // Mirror the parser's recursion ceiling so serializing a pathologically deep
  // value (however it was constructed) cannot overflow the stack. Past the limit
  // we emit null rather than recurse — serialization has no error channel.
  if (depth > kMaxParseDepth) { out += "null"; return; }
  if (val.IsNull()) { out += "null"; return; }
  if (val.IsBool()) { out += val.AsBool() ? "true" : "false"; return; }
  if (val.IsInt()) {
    // to_chars into a stack buffer avoids the throwaway heap std::string that
    // std::to_string allocates for every integer serialized.
    char buf[24];
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), val.AsInt());
    (void)ec;  // int64 always fits in 24 bytes
    out.append(buf, static_cast<std::size_t>(ptr - buf));
    return;
  }
  if (val.IsDouble()) {
    const double d = val.AsDouble();
    // A non-finite double has no JSON representation; emitting the bare token
    // `nan`/`inf` would produce output this very parser (and any strict peer)
    // rejects, and serialization has no error channel to report it. Emit `null`,
    // matching JSON.stringify convention, so a stray NaN cannot corrupt a message.
    if (!std::isfinite(d)) { out += "null"; return; }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.17g", d);
    out += buf;
    return;
  }
  if (val.IsString()) {
    AppendEscaped(out, val.AsString());
    return;
  }
  if (val.IsArray()) {
    out += '[';
    bool first = true;
    for (const auto& elem : val.AsArray()) {
      if (!first) out += ',';
      first = false;
      AppendValue(out, elem, depth + 1);
    }
    out += ']';
    return;
  }
  if (val.IsObject()) {
    out += '{';
    bool first = true;
    for (const auto& [k, v] : val.AsObject()) {
      if (!first) out += ',';
      first = false;
      AppendEscaped(out, k);
      out += ':';
      AppendValue(out, v, depth + 1);
    }
    out += '}';
  }
}

}  // namespace

std::string SerializeJson(const JsonValue& value) {
  std::string out;
  AppendValue(out, value);
  return out;
}

}  // namespace microide::util
