#include "workspace/LspProtocol.h"

#include <algorithm>
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

std::vector<LspClient::Location> ParseLocations(const JsonValue& result) {
  std::vector<LspClient::Location> locations;
  if (result.IsArray()) {
    const auto& array = result.AsArray();
    locations.reserve(array.size());
    for (const auto& item : array) {
      locations.push_back(ParseLocation(item));
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
  diagnostics.reserve(items.size());
  for (const auto& item : items) {
    diagnostics.push_back(ParseDiagnostic(item));
  }
  return diagnostics;
}

LspClient::DocumentSymbol ParseDocumentSymbol(const JsonValue& value) {
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
  for (const auto& child : value["children"].AsArray()) {
    symbol.children.push_back(ParseDocumentSymbol(child));
  }
  return symbol;
}

std::vector<LspClient::DocumentSymbol> ParseDocumentSymbols(const JsonValue& result) {
  std::vector<LspClient::DocumentSymbol> symbols;
  if (!result.IsArray()) {
    return symbols;
  }
  const auto& items = result.AsArray();
  symbols.reserve(items.size());
  for (const auto& item : items) {
    symbols.push_back(ParseDocumentSymbol(item));
  }
  return symbols;
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
  int line = 0;
  int start_char = 0;
  for (std::size_t i = 0; i + 5 <= ints.size() && tokens.size() < max_tokens; i += 5) {
    const int delta_line = static_cast<int>(ints[i].AsInt());
    const int delta_start = static_cast<int>(ints[i + 1].AsInt());
    const int length = static_cast<int>(ints[i + 2].AsInt());
    const int token_type = static_cast<int>(ints[i + 3].AsInt());
    if (delta_line > 0) {
      line += delta_line;
      start_char = delta_start;
    } else {
      start_char += delta_start;
    }
    if (length <= 0 || line < 0 || start_char < 0) {
      continue;  // skip degenerate tokens but keep decoding the rest
    }
    tokens.push_back(LspClient::SemanticToken{
        .line = line, .start_char = start_char, .length = length, .token_type = token_type});
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
