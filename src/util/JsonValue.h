#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "util/TransparentStringHash.h"

namespace microide::util {

struct JsonValue;

// Transparent hash so object lookups accept a string_view key without allocating
// a temporary std::string per access (the DAP/LSP/control parse paths reach into
// objects via operator[] thousands of times per message).
using JsonObject =
    std::unordered_map<std::string, JsonValue, TransparentStringHash, std::equal_to<>>;
using JsonArray = std::vector<JsonValue>;

struct JsonValue {
  using Null = std::monostate;
  std::variant<Null, bool, std::int64_t, double, std::string, JsonArray, JsonObject> v;

  JsonValue() = default;
  JsonValue(std::nullptr_t) : v(Null{}) {}  // NOLINT(google-explicit-constructor)
  JsonValue(bool b) : v(b) {}               // NOLINT(google-explicit-constructor)
  JsonValue(std::int64_t n) : v(n) {}       // NOLINT
  JsonValue(double d) : v(d) {}             // NOLINT
  JsonValue(std::string s) : v(std::move(s)) {}           // NOLINT
  JsonValue(std::string_view s) : v(std::string(s)) {}    // NOLINT
  JsonValue(const char* s) : v(std::string(s)) {}         // NOLINT
  JsonValue(JsonArray a) : v(std::move(a)) {}             // NOLINT
  JsonValue(JsonObject o) : v(std::move(o)) {}            // NOLINT

  bool IsNull() const { return std::holds_alternative<Null>(v); }
  bool IsBool() const { return std::holds_alternative<bool>(v); }
  bool IsInt() const { return std::holds_alternative<std::int64_t>(v); }
  bool IsDouble() const { return std::holds_alternative<double>(v); }
  bool IsString() const { return std::holds_alternative<std::string>(v); }
  bool IsArray() const { return std::holds_alternative<JsonArray>(v); }
  bool IsObject() const { return std::holds_alternative<JsonObject>(v); }

  bool AsBool(bool fallback = false) const {
    if (const auto* b = std::get_if<bool>(&v)) return *b;
    return fallback;
  }
  std::int64_t AsInt(std::int64_t fallback = 0) const {
    if (const auto* n = std::get_if<std::int64_t>(&v)) return *n;
    if (const auto* d = std::get_if<double>(&v)) {
      // Converting a double whose truncated magnitude does not fit int64 is UB
      // ([conv.fpint]); this is reachable because ParseNumber stores an integer
      // literal that overflows int64 (e.g. a uint64-range DAP variablesReference)
      // as a double. Clamp to the representable range instead of trapping. NaN
      // (which fails both comparisons) falls through to the fallback.
      const double d_value = *d;
      if (d_value >= 9223372036854775808.0) return std::numeric_limits<std::int64_t>::max();
      if (d_value < -9223372036854775808.0) return std::numeric_limits<std::int64_t>::min();
      if (d_value >= -9223372036854775808.0) return static_cast<std::int64_t>(d_value);
    }
    return fallback;
  }
  double AsDouble(double fallback = 0.0) const {
    if (const auto* d = std::get_if<double>(&v)) return *d;
    if (const auto* n = std::get_if<std::int64_t>(&v)) return static_cast<double>(*n);
    return fallback;
  }
  const std::string& AsString() const {
    static const std::string kEmpty;
    if (const auto* s = std::get_if<std::string>(&v)) return *s;
    return kEmpty;
  }
  const JsonArray& AsArray() const {
    static const JsonArray kEmpty;
    if (const auto* a = std::get_if<JsonArray>(&v)) return *a;
    return kEmpty;
  }
  const JsonObject& AsObject() const {
    static const JsonObject kEmpty;
    if (const auto* o = std::get_if<JsonObject>(&v)) return *o;
    return kEmpty;
  }

  // Lookup in object; returns null JsonValue if missing or not an object.
  const JsonValue& operator[](std::string_view key) const;
  // Lookup by index in array.
  const JsonValue& operator[](std::size_t idx) const;

  bool HasKey(std::string_view key) const;

  // Mutable accessors (additive; the const API above is unchanged). Opt-in for
  // consumers that own the JsonValue and want to move data out instead of copying —
  // e.g. an LSP response parser moving result strings out on the main thread. Each
  // returns nullptr when the stored alternative does not match.
  JsonValue* MutableAt(std::string_view key) {
    if (auto* o = std::get_if<JsonObject>(&v)) {
      auto it = o->find(key);  // transparent lookup: no temporary std::string
      if (it != o->end()) return &it->second;
    }
    return nullptr;
  }
  JsonArray* MutableArray() { return std::get_if<JsonArray>(&v); }
  std::string* MutableString() { return std::get_if<std::string>(&v); }
};

// Parse JSON text; returns nullopt on parse error.
std::optional<JsonValue> ParseJson(std::string_view text);

// Serialize to compact JSON string.
std::string SerializeJson(const JsonValue& value);

}  // namespace microide::util
