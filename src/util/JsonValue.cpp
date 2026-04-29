#include "util/JsonValue.h"

#include <cassert>
#include <charconv>
#include <cmath>
#include <cstring>
#include <sstream>

#include "util/Parse.h"

namespace microide::util {

const JsonValue& JsonValue::operator[](std::string_view key) const {
  static const JsonValue kNull;
  if (const auto* o = std::get_if<JsonObject>(&v)) {
    auto it = o->find(std::string(key));
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
    return o->count(std::string(key)) > 0;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

namespace {

struct Parser {
  std::string_view src;
  std::size_t pos = 0;

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
    while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
    if (pos < src.size() && src[pos] == '.') {
      is_float = true;
      ++pos;
      while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
    }
    if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
      is_float = true;
      ++pos;
      if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) ++pos;
      while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
    }
    const std::string_view tok = src.substr(start, pos - start);
    if (is_float) {
      const auto parsed = ParseDouble(tok);
      if (!parsed.has_value()) return std::nullopt;
      return JsonValue(*parsed);
    }
    const auto parsed = ParseInt64(tok);
    if (!parsed.has_value()) return std::nullopt;
    return JsonValue(*parsed);
  }

  static unsigned int HexDigit(char c) {
    if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
    return 0xFFFF;
  }

  void AppendUtf8(std::string& out, unsigned int codepoint) {
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

  std::optional<JsonValue> ParseString() {
    if (!Expect('"')) return std::nullopt;
    std::string result;
    while (pos < src.size()) {
      const char c = Advance();
      if (c == '"') return JsonValue(std::move(result));
      if (c != '\\') {
        result += c;
        continue;
      }
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
            const unsigned int d = HexDigit(Advance());
            if (d > 0xF) return std::nullopt;
            cp = (cp << 4) | d;
          }
          // Surrogate pair
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (pos + 6 > src.size() || src[pos] != '\\' || src[pos + 1] != 'u') {
              return std::nullopt;
            }
            pos += 2;
            unsigned int lo = 0;
            for (int i = 0; i < 4; ++i) {
              const unsigned int d = HexDigit(Advance());
              if (d > 0xF) return std::nullopt;
              lo = (lo << 4) | d;
            }
            if (lo < 0xDC00 || lo > 0xDFFF) return std::nullopt;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          AppendUtf8(result, cp);
          break;
        }
        default: return std::nullopt;
      }
    }
    return std::nullopt;  // unterminated
  }

  std::optional<JsonValue> ParseArray() {
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
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[7];
          snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

void AppendValue(std::string& out, const JsonValue& val) {
  if (val.IsNull()) { out += "null"; return; }
  if (val.IsBool()) { out += val.AsBool() ? "true" : "false"; return; }
  if (val.IsInt()) {
    out += std::to_string(val.AsInt());
    return;
  }
  if (val.IsDouble()) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.17g", val.AsDouble());
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
      AppendValue(out, elem);
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
      AppendValue(out, v);
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
