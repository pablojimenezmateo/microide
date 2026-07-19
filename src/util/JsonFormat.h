#pragma once

#include <cstddef>
#include <string>
#include <string_view>  // IWYU pragma: keep

namespace microide::util {

// Result of FormatJson. On success `ok` is true and `text` holds the reformatted
// document; on failure `ok` is false and `error` / `error_offset` describe the
// first problem (byte offset into the input).
struct JsonFormatResult {
  bool ok = false;
  std::string text;
  std::size_t error_offset = 0;
  std::string error;
};

// Pretty-print a JSON document: rewrite its whitespace and sort object keys.
//
// Object members are ordered case-insensitively and naturally ("human"
// alphabetical: "item2" before "item10"), recursively at every nesting level.
// Array element order is semantic and is preserved. Every scalar token (strings,
// numbers, literals) is copied byte-for-byte, so numeric literals keep their exact
// form. This is a single-pass *structural* reformatter that never builds a DOM —
// which is both why key text stays lossless (a JsonValue round trip rewrites
// numbers) and why it is the fastest, lowest-allocation option.
//
// `indent_unit` is one level of indentation (e.g. two spaces or a tab). Output
// uses '\n' newlines and carries no trailing newline. Empty objects/arrays render
// as `{}` / `[]`. Validation matches RFC 8259 with the same rigor as ParseJson
// (raw control bytes in strings, malformed escapes, bad numbers, trailing garbage
// and over-deep nesting are all rejected).
JsonFormatResult FormatJson(std::string_view src, std::string_view indent_unit);

}  // namespace microide::util
