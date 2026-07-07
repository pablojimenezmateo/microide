#include "TestSupport.h"

#include "util/JsonValue.h"
#include "workspace/LspProtocol.h"

#include <optional>

namespace microide::tests {
namespace {

using microide::util::JsonValue;
using microide::util::ParseJson;
using microide::util::SerializeJson;
using microide::workspace::LspClient;
namespace codec = microide::workspace::lsp_protocol;

JsonValue Json(std::string_view text) {
  std::optional<JsonValue> parsed = ParseJson(text);
  Expect(parsed.has_value(), "test JSON literal should parse");
  return std::move(*parsed);
}

void TestLspProtocolParsesPositionAndRange() {
  const JsonValue range_json =
      Json(R"({"start":{"line":3,"character":7},"end":{"line":3,"character":12}})");
  const LspClient::Range range = codec::ParseRange(range_json);
  Expect(range.start.line == 3 && range.start.character == 7, "range start parsed");
  Expect(range.end.line == 3 && range.end.character == 12, "range end parsed");

  // Missing start/end yields a zeroed range rather than garbage.
  const LspClient::Range empty = codec::ParseRange(Json("{}"));
  Expect(empty.start.line == 0 && empty.end.character == 0, "missing range is zeroed");
}

void TestLspProtocolParsesLocationsArrayAndSingleAndLink() {
  const JsonValue single = Json(R"({"uri":"file:///a.cpp","range":
      {"start":{"line":1,"character":0},"end":{"line":1,"character":4}}})");
  const auto from_single = codec::ParseLocations(single);
  Expect(from_single.size() == 1, "single location parses to one entry");
  Expect(from_single[0].uri == "file:///a.cpp", "single location uri");

  const JsonValue array = Json(R"([
      {"uri":"file:///a.cpp","range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}}},
      {"uri":"file:///b.cpp","range":{"start":{"line":2,"character":0},"end":{"line":2,"character":1}}}])");
  const auto from_array = codec::ParseLocations(array);
  Expect(from_array.size() == 2, "array of locations parses both");
  Expect(from_array[1].uri == "file:///b.cpp", "second location uri");

  // LocationLink shape (targetUri / targetSelectionRange) used when linkSupport=true.
  const JsonValue link = Json(R"([{"targetUri":"file:///c.cpp",
      "targetRange":{"start":{"line":5,"character":0},"end":{"line":9,"character":0}},
      "targetSelectionRange":{"start":{"line":5,"character":4},"end":{"line":5,"character":8}}}])");
  const auto from_link = codec::ParseLocations(link);
  Expect(from_link.size() == 1, "location link parses");
  Expect(from_link[0].uri == "file:///c.cpp", "location link target uri");
  Expect(from_link[0].range.start.character == 4, "location link uses selection range");
}

void TestLspProtocolParsesDiagnosticSeverityDefault() {
  const JsonValue with_severity = Json(R"({"range":
      {"start":{"line":0,"character":0},"end":{"line":0,"character":2}},
      "message":"bad","severity":2,"code":"E1"})");
  const LspClient::Diagnostic warn = codec::ParseDiagnostic(with_severity);
  Expect(warn.severity == 2 && warn.message == "bad" && warn.code == "E1", "diagnostic fields");

  // Severity defaults to Error (1) when the server omits it.
  const JsonValue no_severity =
      Json(R"({"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":2}},
               "message":"oops"})");
  Expect(codec::ParseDiagnostic(no_severity).severity == 1, "missing severity defaults to error");
}

void TestLspProtocolParsesDocumentSymbolShapes() {
  // DocumentSymbol with nested children.
  const JsonValue doc_symbol = Json(R"({"name":"Outer","kind":5,
      "range":{"start":{"line":0,"character":0},"end":{"line":9,"character":0}},
      "selectionRange":{"start":{"line":0,"character":6},"end":{"line":0,"character":11}},
      "children":[{"name":"inner","kind":6,
        "range":{"start":{"line":1,"character":0},"end":{"line":2,"character":0}}}]})");
  const LspClient::DocumentSymbol parsed = codec::ParseDocumentSymbol(doc_symbol);
  Expect(parsed.name == "Outer" && parsed.kind == 5, "document symbol top fields");
  Expect(parsed.selection_range.start.character == 6, "document symbol selection range");
  Expect(parsed.children.size() == 1 && parsed.children[0].name == "inner", "nested child parsed");

  // SymbolInformation shape (location.range, no selectionRange).
  const JsonValue symbol_info = Json(R"({"name":"freefn","kind":12,
      "location":{"uri":"file:///d.cpp",
        "range":{"start":{"line":4,"character":0},"end":{"line":4,"character":6}}}})");
  const LspClient::DocumentSymbol info = codec::ParseDocumentSymbol(symbol_info);
  Expect(info.name == "freefn", "symbol information name");
  Expect(info.range.start.line == 4 && info.selection_range.start.line == 4,
         "symbol information range mirrors selection range");
}

void TestLspProtocolEncodersRoundTrip() {
  const LspClient::Range range{{2, 5}, {2, 9}};
  // Make -> Parse should reproduce the range.
  const LspClient::Range round = codec::ParseRange(codec::MakeRange(range));
  Expect(round.start.line == 2 && round.start.character == 5, "encoded range start round-trips");
  Expect(round.end.line == 2 && round.end.character == 9, "encoded range end round-trips");

  const JsonValue params =
      codec::MakeTextDocumentPositionParams("file:///e.cpp", LspClient::Position{7, 3});
  Expect(params["textDocument"]["uri"].AsString() == "file:///e.cpp", "params carry uri");
  Expect(params["position"]["line"].AsInt() == 7 && params["position"]["character"].AsInt() == 3,
         "params carry position");
}

void TestLspProtocolDecodesSemanticTokens() {
  // Two tokens on line 0 (cols 0..3 type 1, cols 5..9 type 2) and one on line 2
  // (cols 4..6 type 0), delta-encoded as the LSP wire format prescribes.
  const JsonValue result = Json(R"({"data":[
      0,0,3,1,0,
      0,5,4,2,0,
      2,4,2,0,0]})");
  const std::vector<LspClient::SemanticToken> tokens = codec::ParseSemanticTokensData(result);
  Expect(tokens.size() == 3, "three tokens decode from the delta stream");
  Expect(tokens[0].line == 0 && tokens[0].start_char == 0 && tokens[0].length == 3 &&
             tokens[0].token_type == 1,
         "first token resolves to absolute (0,0,3,type1)");
  Expect(tokens[1].line == 0 && tokens[1].start_char == 5 && tokens[1].length == 4 &&
             tokens[1].token_type == 2,
         "second token shares the line and advances the column (0,5,4,type2)");
  Expect(tokens[2].line == 2 && tokens[2].start_char == 4 && tokens[2].length == 2,
         "third token jumps two lines and resets the column (2,4,2)");

  // Malformed inputs decode to nothing rather than throwing or over-reading.
  Expect(codec::ParseSemanticTokensData(Json(R"({})")).empty(), "missing data => no tokens");
  Expect(codec::ParseSemanticTokensData(Json(R"({"data":[0,0,3]})")).empty(),
         "a partial 3-int group yields no tokens");

  // Delta positions that would overflow a signed int when summed must be handled
  // as saturating 64-bit arithmetic (no UB) and dropped as out-of-range, while
  // earlier in-range tokens still decode. First token line=2e9 (in range);
  // second adds 2e9 -> 4e9 > INT_MAX -> dropped.
  const auto overflow = codec::ParseSemanticTokensData(
      Json(R"({"data":[2000000000,0,3,0,0, 2000000000,0,3,0,0]})"));
  Expect(overflow.size() == 1,
         "an out-of-range accumulated line is dropped without overflowing int");
  Expect(overflow[0].line == 2000000000,
         "the in-range token decodes to its accumulated line");
}

// A hostile/buggy language server can pack ~1M minimal entries into one 64 MiB
// message; each parse path materializes strings + ranges and builds a picker /
// outline / diagnostics list on the UI thread. The parsers must cap the count.
void TestLspProtocolParseCapsBoundHostileArrays() {
  // Locations (references/definition picker).
  {
    std::string body = "[";
    for (int i = 0; i < 60000; ++i) {
      if (i != 0) body += ',';
      body += R"({"uri":"file:///a","range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}}})";
    }
    body += "]";
    Expect(codec::ParseLocations(Json(body)).size() <= 50000,
           "ParseLocations must cap a hostile locations array");
  }
  // Diagnostics.
  {
    std::string body = "[";
    for (int i = 0; i < 20000; ++i) {
      if (i != 0) body += ',';
      body += R"({"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}},"message":"x"})";
    }
    body += "]";
    Expect(codec::ParseDiagnostics(Json(body)).size() <= 10000,
           "ParseDiagnostics must cap a hostile diagnostics array");
  }
  // Document symbols: a flat sibling flood stays under the total-node budget.
  {
    std::string body = "[";
    for (int i = 0; i < 120000; ++i) {
      if (i != 0) body += ',';
      body += R"({"name":"s","kind":1,"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}}})";
    }
    body += "]";
    Expect(codec::ParseDocumentSymbols(Json(body)).size() <= 100000,
           "ParseDocumentSymbols must cap a hostile symbol sibling flood");
  }
}

void TestLspProtocolParsesHoverContents() {
  // MarkupContent: {kind, value}.
  Expect(codec::ParseHoverContents(
             Json(R"({"contents":{"kind":"markdown","value":"### foo\ndocs"}})")) ==
             "### foo\ndocs",
         "MarkupContent value is extracted");
  // MarkedString object: {language, value}.
  Expect(codec::ParseHoverContents(
             Json(R"({"contents":{"language":"cpp","value":"int foo"}})")) == "int foo",
         "MarkedString object value is extracted");
  // Bare MarkedString string.
  Expect(codec::ParseHoverContents(Json(R"({"contents":"just text"})")) == "just text",
         "bare MarkedString string is returned verbatim");
  // MarkedString[] joins blocks with a blank line, skipping empty pieces.
  Expect(codec::ParseHoverContents(
             Json(R"({"contents":["one",{"language":"cpp","value":"two"},""]})")) ==
             "one\n\ntwo",
         "MarkedString array joins non-empty blocks");
  // No contents / empty.
  Expect(codec::ParseHoverContents(Json("{}")).empty(), "missing contents yields empty");
  Expect(codec::ParseHoverContents(Json(R"({"contents":[]})")).empty(),
         "empty contents array yields empty");
}

void TestLspProtocolParsesWorkspaceEdit() {
  // `changes` object shape (uri -> TextEdit[]).
  const JsonValue changes = Json(R"({"changes":{
      "file:///a.cpp":[{"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":3}},"newText":"sum"}],
      "file:///b.cpp":[
        {"range":{"start":{"line":2,"character":4},"end":{"line":2,"character":7}},"newText":"sum"},
        {"range":{"start":{"line":5,"character":0},"end":{"line":5,"character":3}},"newText":"sum"}]}})");
  const LspClient::WorkspaceEdit edit = codec::ParseWorkspaceEdit(changes);
  Expect(edit.changes.size() == 2, "both files parse from the changes object");
  Expect(edit.changes.at("file:///a.cpp").size() == 1, "a.cpp has one edit");
  Expect(edit.changes.at("file:///b.cpp").size() == 2, "b.cpp has two edits");
  Expect(edit.changes.at("file:///a.cpp")[0].second == "sum", "newText parsed");

  // `documentChanges` array shape (TextDocumentEdit[]); resource ops are skipped.
  const JsonValue doc_changes = Json(R"({"documentChanges":[
      {"textDocument":{"uri":"file:///c.cpp","version":1},
       "edits":[{"range":{"start":{"line":1,"character":0},"end":{"line":1,"character":2}},"newText":"x"}]},
      {"kind":"rename","oldUri":"file:///old.cpp","newUri":"file:///new.cpp"}]})");
  const LspClient::WorkspaceEdit from_doc = codec::ParseWorkspaceEdit(doc_changes);
  Expect(from_doc.changes.size() == 1, "the resource-rename op is skipped, leaving one file");
  Expect(from_doc.changes.count("file:///c.cpp") == 1, "the TextDocumentEdit file is parsed");

  // Caps bound a hostile edit count.
  const JsonValue hostile = Json(R"({"changes":{"file:///h.cpp":[
      {"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}},"newText":"a"},
      {"range":{"start":{"line":1,"character":0},"end":{"line":1,"character":1}},"newText":"b"},
      {"range":{"start":{"line":2,"character":0},"end":{"line":2,"character":1}},"newText":"c"}]}})");
  const LspClient::WorkspaceEdit capped =
      codec::ParseWorkspaceEdit(hostile, /*max_files=*/10, /*max_edits_total=*/2);
  Expect(capped.changes.at("file:///h.cpp").size() == 2, "the total-edit cap truncates the list");
}

void TestLspProtocolParsesSignatureHelp() {
  // Two overloads; parameter labels as strings, documentation as MarkupContent and
  // as a bare string; top-level active signature/parameter.
  const JsonValue help = Json(R"({
    "activeSignature":1,
    "activeParameter":0,
    "signatures":[
      {"label":"foo() -> int","documentation":"first overload"},
      {"label":"foo(int a, int b) -> int",
       "documentation":{"kind":"markdown","value":"second overload"},
       "activeParameter":1,
       "parameters":[
         {"label":"int a","documentation":"the first arg"},
         {"label":"int b","documentation":{"kind":"plaintext","value":"the second arg"}}]}
    ]})");
  const LspClient::SignatureHelp parsed = codec::ParseSignatureHelp(help);
  Expect(parsed.signatures.size() == 2, "both overloads parse");
  Expect(parsed.active_signature == 1, "top-level activeSignature parsed");
  Expect(parsed.active_parameter == 0, "top-level activeParameter parsed");
  Expect(parsed.signatures[1].label == "foo(int a, int b) -> int", "signature label parsed");
  Expect(parsed.signatures[1].documentation == "second overload",
         "MarkupContent documentation unwrapped to its value");
  Expect(parsed.signatures[1].active_parameter == 1,
         "per-signature activeParameter overrides the top-level one");
  Expect(parsed.signatures[1].parameters.size() == 2, "both parameters parse");
  Expect(parsed.signatures[1].parameters[0].label == "int a", "string parameter label parsed");
  Expect(parsed.signatures[1].parameters[1].documentation == "the second arg",
         "MarkupContent parameter documentation unwrapped");
  // A signature that omits activeParameter reports -1 (falls back to the top level).
  Expect(parsed.signatures[0].active_parameter == -1, "absent per-signature activeParameter is -1");

  // Parameter label given as `[start, end]` offsets into the signature label.
  const JsonValue offsets = Json(R"json({
    "signatures":[{"label":"bar(x, y)","parameters":[{"label":[4,5]},{"label":[7,8]}]}]})json");
  const LspClient::SignatureHelp offset_parsed = codec::ParseSignatureHelp(offsets);
  Expect(offset_parsed.signatures.size() == 1, "offset-label signature parses");
  Expect(offset_parsed.signatures[0].parameters[0].label == "x",
         "first parameter label resolved from offsets");
  Expect(offset_parsed.signatures[0].parameters[1].label == "y",
         "second parameter label resolved from offsets");

  // Out-of-range offsets are ignored rather than reading past the label.
  const JsonValue bad = Json(R"({"signatures":[{"label":"z","parameters":[{"label":[3,9]}]}]})");
  const LspClient::SignatureHelp bad_parsed = codec::ParseSignatureHelp(bad);
  Expect(bad_parsed.signatures[0].parameters[0].label.empty(),
         "out-of-range offset label yields empty, not a read past the string");

  // A result with no signatures is empty (nothing to show).
  const LspClient::SignatureHelp none = codec::ParseSignatureHelp(Json("{}"));
  Expect(none.signatures.empty(), "missing signatures array yields no signatures");
}

void TestLspProtocolParsesPrepareRename() {
  // `{ range, placeholder }` shape.
  const LspClient::PrepareRename with_placeholder = codec::ParsePrepareRename(Json(
      R"({"range":{"start":{"line":2,"character":4},"end":{"line":2,"character":9}},
          "placeholder":"widget"})"));
  Expect(with_placeholder.can_rename, "range+placeholder shape is renameable");
  Expect(with_placeholder.placeholder == "widget", "placeholder parsed");
  Expect(with_placeholder.range.start.character == 4, "range start parsed");

  // A bare Range (no placeholder).
  const LspClient::PrepareRename bare_range = codec::ParsePrepareRename(
      Json(R"({"start":{"line":1,"character":0},"end":{"line":1,"character":3}})"));
  Expect(bare_range.can_rename, "bare range is renameable");
  Expect(bare_range.placeholder.empty(), "bare range carries no placeholder");
  Expect(bare_range.range.end.character == 3, "bare range end parsed");

  // `{ defaultBehavior: true/false }`.
  Expect(codec::ParsePrepareRename(Json(R"({"defaultBehavior":true})")).can_rename,
         "defaultBehavior true is renameable");
  Expect(!codec::ParsePrepareRename(Json(R"({"defaultBehavior":false})")).can_rename,
         "defaultBehavior false is not renameable");

  // JSON null => the server says this position is not renameable.
  Expect(!codec::ParsePrepareRename(Json("null")).can_rename,
         "null result is not renameable");
}

void TestLspProtocolParsesTextEdits() {
  const auto edits = codec::ParseTextEdits(Json(R"([
      {"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":2}},"newText":"AB"},
      {"range":{"start":{"line":3,"character":1},"end":{"line":3,"character":1}},"newText":"x"}])"));
  Expect(edits.size() == 2, "both edits parse");
  Expect(edits[0].second == "AB" && edits[1].second == "x", "newText parsed for each edit");
  Expect(edits[0].first.end.character == 2, "range parsed for each edit");

  // Non-array yields no edits; the cap truncates.
  Expect(codec::ParseTextEdits(Json("{}")).empty(), "non-array yields no edits");
  const auto capped = codec::ParseTextEdits(Json(R"([
      {"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}},"newText":"a"},
      {"range":{"start":{"line":1,"character":0},"end":{"line":1,"character":1}},"newText":"b"}])"),
      /*max_edits=*/1);
  Expect(capped.size() == 1, "the edit cap truncates");
}

}  // namespace

void RegisterLspProtocolTests(std::vector<TestCase>& tests) {
  AddTest(tests, "LspProtocol/ParsesWorkspaceEdit", TestLspProtocolParsesWorkspaceEdit);
  AddTest(tests, "LspProtocol/ParsesSignatureHelp", TestLspProtocolParsesSignatureHelp);
  AddTest(tests, "LspProtocol/ParsesPrepareRename", TestLspProtocolParsesPrepareRename);
  AddTest(tests, "LspProtocol/ParsesTextEdits", TestLspProtocolParsesTextEdits);
  AddTest(tests, "LspProtocol/ParsesHoverContents", TestLspProtocolParsesHoverContents);
  AddTest(tests, "LspProtocol/ParseCapsBoundHostileArrays",
          TestLspProtocolParseCapsBoundHostileArrays);
  AddTest(tests, "LspProtocol/DecodesSemanticTokens", TestLspProtocolDecodesSemanticTokens);
  AddTest(tests, "LspProtocol/ParsesPositionAndRange", TestLspProtocolParsesPositionAndRange);
  AddTest(tests, "LspProtocol/ParsesLocations", TestLspProtocolParsesLocationsArrayAndSingleAndLink);
  AddTest(tests, "LspProtocol/ParsesDiagnosticSeverityDefault",
          TestLspProtocolParsesDiagnosticSeverityDefault);
  AddTest(tests, "LspProtocol/ParsesDocumentSymbolShapes",
          TestLspProtocolParsesDocumentSymbolShapes);
  AddTest(tests, "LspProtocol/EncodersRoundTrip", TestLspProtocolEncodersRoundTrip);
}

}  // namespace microide::tests
