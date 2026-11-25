#include "util/JsonValue.h"

#include <cassert>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

#include "util/Hex.h"
#include "util/Parse.h"
#include "util/StringUtil.h"

namespace microide::util {

std::int64_t JsonValue::AsIntSlow(std::int64_t fallback) const {
  if (const auto* d = std::get_if<double>(&v)) {
    const double d_value = *d;
    if (d_value >= 9223372036854775808.0) return std::numeric_limits<std::int64_t>::max();
    if (d_value < -9223372036854775808.0) return std::numeric_limits<std::int64_t>::min();
    if (d_value >= -9223372036854775808.0) return static_cast<std::int64_t>(d_value);
  }
  return fallback;
}

const JsonValue& JsonValue::operator[](std::string_view key) const {
  static const JsonValue kNull;
  if (const auto* o = std::get_if<JsonObject>(&v)) {
    auto it = o->find(key);
    if (it != o->end()) return it->value;
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
    return o->find(key) != o->end();
  }
  return false;
}

namespace {

constexpr int kMaxParseDepth = 200;

struct Parser {
  std::string_view src;
  std::size_t pos = 0;
  int depth = 0;

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
    if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') return std::nullopt;
    if (src[pos] == '0') {
      ++pos;
    } else {
      while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
    }
    if (pos < src.size() && src[pos] == '.') {
      is_float = true;
      ++pos;
      if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') return std::nullopt;
      while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
    }
    if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
      is_float = true;
      ++pos;
      if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) ++pos;
      if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') return std::nullopt;
      while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') ++pos;
    }
    const std::string_view tok = src.substr(start, pos - start);
    if (is_float) {
      const auto parsed = ParseDouble(tok);
      if (parsed.has_value()) return JsonValue(*parsed);
      const double overflow = (!tok.empty() && tok.front() == '-')
                                  ? -std::numeric_limits<double>::infinity()
                                  : std::numeric_limits<double>::infinity();
      return JsonValue(overflow);
    }
    const auto parsed = ParseInt64(tok);
    if (parsed.has_value()) return JsonValue(*parsed);
    const auto as_double = ParseDouble(tok);
    if (!as_double.has_value()) return std::nullopt;
    return JsonValue(*as_double);
  }

  std::optional<JsonValue> ParseString() {
    if (!Expect('"')) return std::nullopt;
    std::string result;
    while (pos < src.size()) {
      const std::size_t run_start = pos;
      while (pos < src.size() && src[pos] != '"' && src[pos] != '\\' &&
             static_cast<unsigned char>(src[pos]) >= 0x20) {
        ++pos;
      }
      if (pos > run_start) {
        result.append(src.data() + run_start, pos - run_start);
      }
      if (pos >= src.size()) break;
      if (static_cast<unsigned char>(src[pos]) < 0x20) return std::nullopt;
      const char c = src[pos++];
      if (c == '"') return JsonValue(std::move(result));
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
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            unsigned int lo = 0;
            bool paired = pos + 6 <= src.size() && src[pos] == '\\' && src[pos + 1] == 'u';
            for (std::size_t i = 0; paired && i < 4; ++i) {
              const int d = HexDigitValue(src[pos + 2 + i]);
              paired = d >= 0;
              lo = (lo << 4) | static_cast<unsigned int>(d < 0 ? 0 : d);
            }
            paired = paired && lo >= 0xDC00 && lo <= 0xDFFF;
            if (paired) {
              pos += 6;
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else {
              cp = 0xFFFD;
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;
          }
          AppendUtf8(result, static_cast<char32_t>(cp));
          break;
        }
        default: return std::nullopt;
      }
    }
    return std::nullopt;
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
      obj.AppendInDocumentOrder(std::move(*key->MutableString()), std::move(*val));
      SkipWhitespace();
      if (Expect('}')) {
        obj.SortAfterParse();
        return JsonValue(std::move(obj));
      }
      if (!Expect(',')) return std::nullopt;
    }
  }
};

}

std::optional<JsonValue> ParseJson(std::string_view text) {
  Parser p{text};
  auto result = p.ParseValue();
  if (!result) return std::nullopt;
  p.SkipWhitespace();
  if (p.pos != text.size()) return std::nullopt;
  return result;
}

namespace {

void AppendEscaped(std::string& out, const std::string& s) {
  out += '"';
  const std::size_t n = s.size();
  std::size_t i = 0;
  while (i < n) {
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
  if (depth > kMaxParseDepth) { out += "null"; return; }
  if (val.IsNull()) { out += "null"; return; }
  if (val.IsBool()) { out += val.AsBool() ? "true" : "false"; return; }
  if (val.IsInt()) {
    char buf[24];
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), val.AsInt());
    (void)ec;
    out.append(buf, static_cast<std::size_t>(ptr - buf));
    return;
  }
  if (val.IsDouble()) {
    const double d = val.AsDouble();
    if (!std::isfinite(d)) { out += "null"; return; }
    char buf[40];
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), d);
    (void)ec;
    out.append(buf, static_cast<std::size_t>(ptr - buf));
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
    for (const auto& entry : val.AsObject()) {
      if (!first) out += ',';
      first = false;
      AppendEscaped(out, entry.key);
      out += ':';
      AppendValue(out, entry.value, depth + 1);
    }
    out += '}';
  }
}

}

std::string SerializeJson(const JsonValue& value) {
  std::string out;
  AppendValue(out, value);
  return out;
}

void JsonObject::SortAfterParse() {
  if (entries_.size() < 2) {
    return;
  }
  constexpr std::size_t kInsertionSortLimit = 32;
  if (entries_.size() <= kInsertionSortLimit) {
    for (std::size_t i = 1; i < entries_.size(); ++i) {
      JsonObjectEntry pivot = std::move(entries_[i]);
      std::size_t j = i;
      while (j > 0 && detail::JsonKeyOrderLess(pivot.key, entries_[j - 1].key)) {
        entries_[j] = std::move(entries_[j - 1]);
        --j;
      }
      entries_[j] = std::move(pivot);
    }
  } else {
    std::stable_sort(entries_.begin(), entries_.end(),
                     [](const JsonObjectEntry& a, const JsonObjectEntry& b) {
                       return detail::JsonKeyOrderLess(a.key, b.key);
                     });
  }
  auto write = entries_.begin();
  for (auto read = entries_.begin(); read != entries_.end();) {
    auto next = read + 1;
    while (next != entries_.end() && next->key == read->key) {
      ++next;
    }
    if (write != next - 1) {
      *write = std::move(*(next - 1));
    }
    ++write;
    read = next;
  }
  entries_.erase(write, entries_.end());
}

}
