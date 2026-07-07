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
// Parse a TextEdit[] (the shape textDocument/formatting and rangeFormatting both
// return). Non-array input yields no edits; `max_edits` caps the count as a
// hostile-server backstop (a whole-document reformat legitimately returns many).
std::vector<LspClient::TextEdit> ParseTextEdits(const util::JsonValue& result,
                                                std::size_t max_edits = 200000);
// Accepts DocumentSymbol (range/selectionRange/children) or SymbolInformation
// (location.range) item shapes.
LspClient::DocumentSymbol ParseDocumentSymbol(const util::JsonValue& value);
std::vector<LspClient::DocumentSymbol> ParseDocumentSymbols(const util::JsonValue& result);

// Parse a `workspace/symbol` result (SymbolInformation[] / WorkspaceSymbol[]): each
// carries name, kind, optional containerName, and a location (uri + optional range).
// The count is bounded so a hostile server cannot force an unbounded allocation.
std::vector<LspClient::WorkspaceSymbol> ParseWorkspaceSymbols(const util::JsonValue& result);

// Parse a `textDocument/prepareRename` result. Handles all wire shapes: a bare
// Range, `{range, placeholder}`, `{defaultBehavior}`, and JSON null (position not
// renameable → can_rename=false). Callers distinguish "no provider" (nullopt at
// the request layer) from "server says not here" (can_rename=false).
LspClient::PrepareRename ParsePrepareRename(const util::JsonValue& result);

// Parse a `textDocument/signatureHelp` result. Handles a parameter `label` given
// either as a string or as `[start, end]` offsets into the signature label, and
// `documentation` as a bare string or MarkupContent ({value}). The signature and
// parameter counts are bounded so a hostile server cannot force an unbounded
// main-thread allocation.
LspClient::SignatureHelp ParseSignatureHelp(const util::JsonValue& result);

// Parse a WorkspaceEdit into the URI-keyed edit map. Handles both the `changes`
// object shape (uri -> TextEdit[]) and the `documentChanges` array shape
// (TextDocumentEdit[]; resource create/rename/delete ops are skipped). The total
// files and edits are bounded so a hostile server cannot force an unbounded
// main-thread allocation (a rename/apply-edit result is re-materialized here).
LspClient::WorkspaceEdit ParseWorkspaceEdit(const util::JsonValue& edit,
                                            std::size_t max_files = 10000,
                                            std::size_t max_edits_total = 200000);

// Decode a `textDocument/hover` result's `contents` into a single plain/markdown
// string. Handles all three wire shapes: MarkupContent ({kind, value}), a bare
// MarkedString, and a MarkedString[] (joined with blank lines). Returns empty when
// there is no usable content.
std::string ParseHoverContents(const util::JsonValue& hover_result);

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
