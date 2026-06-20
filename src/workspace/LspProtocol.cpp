#include "workspace/LspProtocol.h"

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
