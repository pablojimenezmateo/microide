#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace microide::util {

struct JsonValue;
struct JsonObjectEntry;

using JsonArray = std::vector<JsonValue>;

class JsonObject {
 public:
  using value_type = JsonObjectEntry;
  using Storage = std::vector<JsonObjectEntry>;
  using iterator = Storage::iterator;
  using const_iterator = Storage::const_iterator;

  JsonObject();
  JsonObject(std::initializer_list<JsonObjectEntry> init);
  JsonObject(const JsonObject&);
  JsonObject(JsonObject&&) noexcept;
  JsonObject& operator=(const JsonObject&);
  JsonObject& operator=(JsonObject&&) noexcept;
  ~JsonObject();

  iterator begin();
  iterator end();
  const_iterator begin() const;
  const_iterator end() const;
  const_iterator cbegin() const;
  const_iterator cend() const;

  bool empty() const;
  std::size_t size() const;
  void clear();
  void reserve(std::size_t capacity);

  iterator find(std::string_view key);
  const_iterator find(std::string_view key) const;
  std::size_t count(std::string_view key) const;
  bool contains(std::string_view key) const;

  JsonValue& operator[](std::string_view key);
  std::size_t erase(std::string_view key);

  bool operator==(const JsonObject& other) const;

  void AppendInDocumentOrder(std::string key, JsonValue value);
  void SortAfterParse();

 private:
  Storage entries_;
};

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
  [[gnu::always_inline]] std::int64_t AsInt(std::int64_t fallback = 0) const {
    if (const auto* n = std::get_if<std::int64_t>(&v)) [[likely]] {
      return *n;
    }
    return AsIntSlow(fallback);
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

  std::int64_t AsIntSlow(std::int64_t fallback) const;

  const JsonValue& operator[](std::string_view key) const;
  const JsonValue& operator[](std::size_t idx) const;

  bool HasKey(std::string_view key) const;

  JsonValue* MutableAt(std::string_view key);
  JsonArray* MutableArray() { return std::get_if<JsonArray>(&v); }
  std::string* MutableString() { return std::get_if<std::string>(&v); }
  JsonObject* MutableObject() { return std::get_if<JsonObject>(&v); }

  bool operator==(const JsonValue&) const = default;
};

struct JsonObjectEntry {
  std::string key;
  JsonValue value;

  bool operator==(const JsonObjectEntry&) const = default;
};

namespace detail {
inline bool JsonKeyOrderLess(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return a.size() < b.size();
  }
  return a.compare(b) < 0;
}
inline bool JsonKeyLess(const JsonObjectEntry& entry, std::string_view key) {
  return JsonKeyOrderLess(entry.key, key);
}
}

inline JsonObject::JsonObject() = default;
inline JsonObject::JsonObject(std::initializer_list<JsonObjectEntry> init) : entries_(init) {
  SortAfterParse();
}
inline JsonObject::JsonObject(const JsonObject&) = default;
inline JsonObject::JsonObject(JsonObject&&) noexcept = default;
inline JsonObject& JsonObject::operator=(const JsonObject&) = default;
inline JsonObject& JsonObject::operator=(JsonObject&&) noexcept = default;
inline JsonObject::~JsonObject() = default;

inline JsonObject::iterator JsonObject::begin() { return entries_.begin(); }
inline JsonObject::iterator JsonObject::end() { return entries_.end(); }
inline JsonObject::const_iterator JsonObject::begin() const { return entries_.begin(); }
inline JsonObject::const_iterator JsonObject::end() const { return entries_.end(); }
inline JsonObject::const_iterator JsonObject::cbegin() const { return entries_.cbegin(); }
inline JsonObject::const_iterator JsonObject::cend() const { return entries_.cend(); }

inline bool JsonObject::empty() const { return entries_.empty(); }
inline std::size_t JsonObject::size() const { return entries_.size(); }
inline void JsonObject::clear() { entries_.clear(); }
inline void JsonObject::reserve(std::size_t capacity) { entries_.reserve(capacity); }

inline JsonObject::iterator JsonObject::find(std::string_view key) {
  const auto it = std::lower_bound(entries_.begin(), entries_.end(), key, detail::JsonKeyLess);
  return (it != entries_.end() && it->key == key) ? it : entries_.end();
}
inline JsonObject::const_iterator JsonObject::find(std::string_view key) const {
  const auto it = std::lower_bound(entries_.begin(), entries_.end(), key, detail::JsonKeyLess);
  return (it != entries_.end() && it->key == key) ? it : entries_.end();
}
inline std::size_t JsonObject::count(std::string_view key) const {
  return find(key) == entries_.end() ? 0u : 1u;
}
inline bool JsonObject::contains(std::string_view key) const { return count(key) != 0; }

inline JsonValue& JsonObject::operator[](std::string_view key) {
  auto it = std::lower_bound(entries_.begin(), entries_.end(), key, detail::JsonKeyLess);
  if (it != entries_.end() && it->key == key) {
    return it->value;
  }
  it = entries_.insert(it, JsonObjectEntry{std::string(key), JsonValue{}});
  return it->value;
}

inline std::size_t JsonObject::erase(std::string_view key) {
  const auto it = find(key);
  if (it == entries_.end()) {
    return 0;
  }
  entries_.erase(it);
  return 1;
}

inline bool JsonObject::operator==(const JsonObject& other) const {
  return entries_ == other.entries_;
}

inline JsonValue* JsonValue::MutableAt(std::string_view key) {
  if (auto* o = std::get_if<JsonObject>(&v)) {
    auto it = o->find(key);
    if (it != o->end()) return &it->value;
  }
  return nullptr;
}

inline void JsonObject::AppendInDocumentOrder(std::string key, JsonValue value) {
  if (entries_.empty()) {
    entries_.reserve(8);
  }
  entries_.push_back(JsonObjectEntry{std::move(key), std::move(value)});
}

std::optional<JsonValue> ParseJson(std::string_view text);

std::string SerializeJson(const JsonValue& value);

}
