#pragma once

#include <string>
#include <vector>

#include "util/JsonValue.h"
#include "workspace/WorkspaceLspClient.h"

// Single home for the JSON <-> LSP wire-type mapping. Every place that builds or
// parses LSP request/response/notification payloads routes through these helpers
// so the encoding lives in exactly one translation unit.
namespace microide::workspace::lsp_protocol {

// ---- Decode (wire JSON -> LSP structs) ------------------------------------
LspClient::Position ParsePosition(const util::JsonValue& value);
LspClient::Range ParseRange(const util::JsonValue& value);
LspClient::Location ParseLocation(const util::JsonValue& value);
// Accepts either a single Location or an array of Location/LocationLink.
std::vector<LspClient::Location> ParseLocations(const util::JsonValue& result);
LspClient::Diagnostic ParseDiagnostic(const util::JsonValue& value);
std::vector<LspClient::Diagnostic> ParseDiagnostics(const util::JsonValue& array);
// Accepts DocumentSymbol (range/selectionRange/children) or SymbolInformation
// (location.range) item shapes.
LspClient::DocumentSymbol ParseDocumentSymbol(const util::JsonValue& value);
std::vector<LspClient::DocumentSymbol> ParseDocumentSymbols(const util::JsonValue& result);

// ---- Encode (LSP structs -> wire JSON) ------------------------------------
util::JsonValue MakePosition(const LspClient::Position& position);
util::JsonValue MakeRange(const LspClient::Range& range);
util::JsonValue MakeTextDocumentIdentifier(const std::string& uri);
util::JsonValue MakeTextDocumentPositionParams(const std::string& uri,
                                               const LspClient::Position& position);

}  // namespace microide::workspace::lsp_protocol
