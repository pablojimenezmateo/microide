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

// Decode a `textDocument/semanticTokens/full` result. The `data` array is a flat
// run of 5-int groups (deltaLine, deltaStartChar, length, tokenType,
// tokenModifiers) relative to the previous token; this resolves them to absolute
// (line, start_char, length, token_type) tokens. Malformed input (non-array,
// length not a multiple of 5) yields no tokens; `max_tokens` caps the result so a
// hostile/huge response cannot force an unbounded allocation.
std::vector<LspClient::SemanticToken> ParseSemanticTokensData(const util::JsonValue& result,
                                                              std::size_t max_tokens = 500000);

// ---- Encode (LSP structs -> wire JSON) ------------------------------------
util::JsonValue MakePosition(const LspClient::Position& position);
util::JsonValue MakeRange(const LspClient::Range& range);
// Encode a diagnostic for a request `context.diagnostics` array (range, message,
// severity, and code when non-empty). Servers like clangd match the stored
// quickfixes against these by range + message, so the round-tripped values must
// mirror what the server originally published.
util::JsonValue MakeDiagnostic(const LspClient::Diagnostic& diagnostic);
util::JsonValue MakeTextDocumentIdentifier(const std::string& uri);
util::JsonValue MakeTextDocumentPositionParams(const std::string& uri,
                                               const LspClient::Position& position);

}  // namespace microide::workspace::lsp_protocol
