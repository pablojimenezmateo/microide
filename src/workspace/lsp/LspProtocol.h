#pragma once

#include <string>
#include <vector>

#include "util/JsonValue.h"
#include "workspace/lsp/WorkspaceLspClient.h"

namespace microide::workspace::lsp_protocol {

LspClient::Position ParsePosition(const util::JsonValue& value);
LspClient::Range ParseRange(const util::JsonValue& value);
LspClient::Location ParseLocation(const util::JsonValue& value);
std::vector<LspClient::Location> ParseLocations(const util::JsonValue& result);
LspClient::Diagnostic ParseDiagnostic(const util::JsonValue& value);
std::vector<LspClient::Diagnostic> ParseDiagnostics(const util::JsonValue& array);
std::string StripMarkdownFences(std::string text);
std::string HoverMarkdownToText(std::string_view markdown);

std::vector<LspClient::TextEdit> ParseTextEdits(const util::JsonValue& result,
                                                std::size_t max_edits = 200000);
LspClient::DocumentSymbol ParseDocumentSymbol(const util::JsonValue& value);
std::vector<LspClient::DocumentSymbol> ParseDocumentSymbols(const util::JsonValue& result);

std::vector<LspClient::WorkspaceSymbol> ParseWorkspaceSymbols(const util::JsonValue& result);

LspClient::PrepareRename ParsePrepareRename(const util::JsonValue& result);

LspClient::SignatureHelp ParseSignatureHelp(const util::JsonValue& result);

LspClient::WorkspaceEdit ParseWorkspaceEdit(const util::JsonValue& edit,
                                            std::size_t max_files = 10000,
                                            std::size_t max_edits_total = 200000);

std::string ParseHoverContents(const util::JsonValue& hover_result);

std::vector<LspClient::SemanticToken> ParseSemanticTokensData(const util::JsonValue& result,
                                                              std::size_t max_tokens = 500000);

std::vector<LspClient::InlayHint> ParseInlayHints(const util::JsonValue& result,
                                                  std::size_t max_hints = 20000);

std::vector<LspClient::DocumentHighlight> ParseDocumentHighlights(
    const util::JsonValue& result, std::size_t max_highlights = 4096);

LspClient::CodeLens ParseCodeLens(const util::JsonValue& value);
std::vector<LspClient::CodeLens> ParseCodeLenses(const util::JsonValue& result,
                                                 std::size_t max_lenses = 4096);

std::vector<LspClient::CallHierarchyItem> ParseCallHierarchyItems(const util::JsonValue& result,
                                                                  std::size_t max_items = 256);
std::vector<LspClient::CallHierarchyCall> ParseCallHierarchyCalls(const util::JsonValue& result,
                                                                  bool incoming,
                                                                  std::size_t max_calls = 4096);

util::JsonValue MakePosition(const LspClient::Position& position);
util::JsonValue MakeRange(const LspClient::Range& range);
util::JsonValue MakeDiagnostic(const LspClient::Diagnostic& diagnostic);
util::JsonValue MakeTextDocumentIdentifier(const std::string& uri);
util::JsonValue MakeTextDocumentPositionParams(const std::string& uri,
                                               const LspClient::Position& position);

}
