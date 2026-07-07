#include "workspace/LspProtocol.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace microide::workspace::lsp_protocol {

using util::JsonObject;
using util::JsonValue;

LspClient::Position ParsePosition(const JsonValue& value) {
  LspClient::Position position;
  position.line = static_cast<int>(value["line"].AsInt());
  position.character = static_cast<int>(value["character"].AsInt());
  return position;
}

LspClient::Range ParseRange(const JsonValue& value) {
  LspClient::Range range;
  if (!value.HasKey("start") || !value.HasKey("end")) {
    return range;
  }
  range.start = ParsePosition(value["start"]);
  range.end = ParsePosition(value["end"]);
  return range;
}

LspClient::Location ParseLocation(const JsonValue& value) {
  LspClient::Location location;
  // LocationLink uses targetUri/targetRange; Location uses uri/range.
  if (value.HasKey("targetUri")) {
    location.uri = value["targetUri"].AsString();
    location.range = ParseRange(value.HasKey("targetSelectionRange")
                                    ? value["targetSelectionRange"]
                                    : value["targetRange"]);
    return location;
  }
  location.uri = value["uri"].AsString();
  location.range = ParseRange(value["range"]);
  return location;
}

// Cap the number of entries materialized from a server array. LSP messages are
// bounded to 64 MiB (kMaxLspMessageBytes), but a minimal location/diagnostic/
// symbol object is only a few dozen bytes of JSON, so a hostile/buggy server can
// still pack ~1M entries into one message — each materialized into strings +
// ranges and built into a picker/outline on the UI thread. These caps sit far
// above any usable list; past them the extra entries are dropped, not parsed.
constexpr std::size_t kMaxLspLocations = 50000;
constexpr std::size_t kMaxLspDiagnostics = 10000;  // matches DiagnosticsStore cap
constexpr std::size_t kMaxLspSymbolNodes = 100000;  // total across the symbol tree

std::vector<LspClient::Location> ParseLocations(const JsonValue& result) {
  std::vector<LspClient::Location> locations;
  if (result.IsArray()) {
    const auto& array = result.AsArray();
    const std::size_t count = std::min(array.size(), kMaxLspLocations);
    locations.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      locations.push_back(ParseLocation(array[i]));
    }
  } else if (result.HasKey("uri") || result.HasKey("targetUri")) {
    locations.push_back(ParseLocation(result));
  }
  return locations;
}

LspClient::Diagnostic ParseDiagnostic(const JsonValue& value) {
  LspClient::Diagnostic diagnostic;
  diagnostic.range = ParseRange(value["range"]);
  diagnostic.message = value["message"].AsString();
  diagnostic.severity = static_cast<int>(value["severity"].AsInt(1));
  diagnostic.code = value["code"].AsString();
  return diagnostic;
}

std::vector<LspClient::Diagnostic> ParseDiagnostics(const JsonValue& array) {
  std::vector<LspClient::Diagnostic> diagnostics;
  if (!array.IsArray()) {
    return diagnostics;
  }
  const auto& items = array.AsArray();
  const std::size_t count = std::min(items.size(), kMaxLspDiagnostics);
  diagnostics.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    diagnostics.push_back(ParseDiagnostic(items[i]));
  }
  return diagnostics;
}

// Bound recursion over the server-supplied `children` tree: a hostile/buggy
// server could otherwise nest it deeply enough to overflow the stack. Past the
// cap, keep the symbol but drop its deeper descendants (an outline that deep is
// unusable anyway).
constexpr int kMaxDocumentSymbolDepth = 64;

// `remaining` is a shared total-node budget: a server can stay under the depth
// cap yet pack millions of siblings/children into the tree, so bound the total
// count of materialized nodes across the whole recursion, not just the depth.
LspClient::DocumentSymbol ParseDocumentSymbolAtDepth(const JsonValue& value, int depth,
                                                     std::size_t& remaining) {
  LspClient::DocumentSymbol symbol;
  symbol.name = value["name"].AsString();
  symbol.detail = value["detail"].AsString();
  symbol.kind = static_cast<int>(value["kind"].AsInt(1));
  if (value.HasKey("location")) {
    // SymbolInformation shape.
    symbol.range = ParseRange(value["location"]["range"]);
    symbol.selection_range = symbol.range;
  } else {
    symbol.range = ParseRange(value["range"]);
    symbol.selection_range =
        value.HasKey("selectionRange") ? ParseRange(value["selectionRange"]) : symbol.range;
  }
  if (depth >= kMaxDocumentSymbolDepth) {
    return symbol;
  }
  for (const auto& child : value["children"].AsArray()) {
    if (remaining == 0) {
      break;
    }
    --remaining;
    symbol.children.push_back(ParseDocumentSymbolAtDepth(child, depth + 1, remaining));
  }
  return symbol;
}

LspClient::DocumentSymbol ParseDocumentSymbol(const JsonValue& value) {
  std::size_t remaining = kMaxLspSymbolNodes;
  return ParseDocumentSymbolAtDepth(value, 0, remaining);
}

std::vector<LspClient::DocumentSymbol> ParseDocumentSymbols(const JsonValue& result) {
  std::vector<LspClient::DocumentSymbol> symbols;
  if (!result.IsArray()) {
    return symbols;
  }
  const auto& items = result.AsArray();
  std::size_t remaining = kMaxLspSymbolNodes;
  const std::size_t count = std::min(items.size(), remaining);
  symbols.reserve(count);
  for (std::size_t i = 0; i < count && remaining > 0; ++i) {
    --remaining;
    symbols.push_back(ParseDocumentSymbolAtDepth(items[i], 0, remaining));
  }
  return symbols;
}

namespace {
// One MarkedString element: either a bare string or {language, value}.
std::string MarkedStringValue(const JsonValue& value) {
  if (value.IsString()) {
    return value.AsString();
  }
  if (value.HasKey("value")) {
    return value["value"].AsString();
  }
  return {};
}
}  // namespace

std::string ParseHoverContents(const JsonValue& hover_result) {
  if (!hover_result.HasKey("contents")) {
    return {};
  }
  const JsonValue& contents = hover_result["contents"];
  // MarkupContent ({kind, value}) and the object form of MarkedString ({language,
  // value}) both carry a "value" — take it directly.
  if (contents.HasKey("value")) {
    return contents["value"].AsString();
  }
  // MarkedString[]: join each element's text with a blank line between blocks.
  if (contents.IsArray()) {
    std::string out;
    for (const auto& element : contents.AsArray()) {
      const std::string piece = MarkedStringValue(element);
      if (piece.empty()) {
        continue;
      }
      if (!out.empty()) {
        out += "\n\n";
      }
      out += piece;
    }
    return out;
  }
  // Bare MarkedString.
  if (contents.IsString()) {
    return contents.AsString();
  }
  return {};
}

std::vector<LspClient::SemanticToken> ParseSemanticTokensData(const JsonValue& result,
                                                              std::size_t max_tokens) {
  std::vector<LspClient::SemanticToken> tokens;
  if (!result.HasKey("data")) {
    return tokens;
  }
  const JsonValue& data = result["data"];
  if (!data.IsArray()) {
    return tokens;
  }
  const auto& ints = data.AsArray();
  const std::size_t groups = ints.size() / 5;  // 5 ints per token; trailing partial ignored
  tokens.reserve(std::min(groups, max_tokens));
  // Accumulate the delta-encoded positions in 64-bit so a server feeding large
  // deltas cannot overflow a signed int (UB) as they sum across up to max_tokens
  // entries; out-of-range results are then dropped as degenerate.
  std::int64_t line = 0;
  std::int64_t start_char = 0;
  for (std::size_t i = 0; i + 5 <= ints.size() && tokens.size() < max_tokens; i += 5) {
    const std::int64_t delta_line = ints[i].AsInt();
    const std::int64_t delta_start = ints[i + 1].AsInt();
    const std::int64_t length = ints[i + 2].AsInt();
    const int token_type = static_cast<int>(ints[i + 3].AsInt());
    if (delta_line > 0) {
      line += delta_line;
      start_char = delta_start;
    } else {
      start_char += delta_start;
    }
    static constexpr std::int64_t kMaxCoord = std::numeric_limits<int>::max();
    if (length <= 0 || length > kMaxCoord || line < 0 || line > kMaxCoord ||
        start_char < 0 || start_char > kMaxCoord) {
      continue;  // skip degenerate/out-of-range tokens but keep decoding the rest
    }
    tokens.push_back(LspClient::SemanticToken{.line = static_cast<int>(line),
                                              .start_char = static_cast<int>(start_char),
                                              .length = static_cast<int>(length),
                                              .token_type = token_type});
  }
  return tokens;
}

JsonValue MakePosition(const LspClient::Position& position) {
  JsonObject object;
  object["line"] = JsonValue(static_cast<std::int64_t>(position.line));
  object["character"] = JsonValue(static_cast<std::int64_t>(position.character));
  return JsonValue(std::move(object));
}

JsonValue MakeRange(const LspClient::Range& range) {
  JsonObject object;
  object["start"] = MakePosition(range.start);
  object["end"] = MakePosition(range.end);
  return JsonValue(std::move(object));
}

JsonValue MakeDiagnostic(const LspClient::Diagnostic& diagnostic) {
  JsonObject object;
  object["range"] = MakeRange(diagnostic.range);
  object["message"] = JsonValue(diagnostic.message);
  object["severity"] = JsonValue(static_cast<std::int64_t>(diagnostic.severity));
  if (!diagnostic.code.empty()) {
    object["code"] = JsonValue(diagnostic.code);
  }
  return JsonValue(std::move(object));
}

JsonValue MakeTextDocumentIdentifier(const std::string& uri) {
  JsonObject object;
  object["uri"] = JsonValue(uri);
  return JsonValue(std::move(object));
}

JsonValue MakeTextDocumentPositionParams(const std::string& uri,
                                         const LspClient::Position& position) {
  JsonObject params;
  params["textDocument"] = MakeTextDocumentIdentifier(uri);
  params["position"] = MakePosition(position);
  return JsonValue(std::move(params));
}

}  // namespace microide::workspace::lsp_protocol
