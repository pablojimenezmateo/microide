#include "TestSupport.h"

#include "util/JsonValue.h"
#include "workspace/lsp/LspProtocol.h"

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

  // LSP Diagnostic.code is `integer | string`. A numeric code (e.g. TypeScript's
  // 2304) must be captured as text, not silently dropped by AsString().
  const JsonValue int_code =
      Json(R"({"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":2}},
               "message":"cannot find name","severity":1,"code":2304})");
  Expect(codec::ParseDiagnostic(int_code).code == "2304",
         "integer diagnostic code must be preserved as its decimal string");

  // Out-of-spec severity values (0, 99, INT_MAX) must be clamped to the LSP
  // domain (1..4) at parse time so they cannot misclassify severity filters or
  // "most severe" status logic; anything invalid defaults to Error (1).
  const auto severity_of = [](const char* value) {
    std::string body = R"({"range":{"start":{"line":0,"character":0},)"
                       R"("end":{"line":0,"character":2}},"message":"m","severity":)";
    body += value;
    body += "}";
    return codec::ParseDiagnostic(Json(body)).severity;
  };
  Expect(severity_of("0") == 1, "severity 0 clamps to Error");
  Expect(severity_of("5") == 1, "severity 5 is out of range and clamps to Error");
  Expect(severity_of("99") == 1, "severity 99 clamps to Error");
  Expect(severity_of("2147483647") == 1, "INT_MAX severity clamps to Error");
  Expect(severity_of("4") == 4, "in-range severity 4 (Hint) is preserved");
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

  // TD-2026-07-16-46: real INT64 overflow. Two groups whose deltaLine is INT64_MAX
  // each would UB-overflow a plain `line += delta` on the second add (the first token
  // is already dropped but leaves `line` at INT64_MAX). Saturating arithmetic drops
  // both and never overflows. Also exercise a same-line huge deltaStart run.
  const auto line_overflow = codec::ParseSemanticTokensData(
      Json(R"({"data":[9223372036854775807,0,3,0,0, 9223372036854775807,0,3,0,0]})"));
  Expect(line_overflow.empty(),
         "two INT64_MAX line deltas are both dropped without signed-overflow UB");
  const auto col_overflow = codec::ParseSemanticTokensData(
      Json(R"({"data":[0,9223372036854775807,3,0,0, 0,9223372036854775807,3,0,0]})"));
  Expect(col_overflow.empty(),
         "two INT64_MAX same-line column deltas are both dropped without overflow UB");
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
  // Fenced code blocks lose their fence lines (with the language tag) and keep
  // their content; the popup has no markdown renderer, so "```cpp" was painted.
  Expect(codec::ParseHoverContents(Json(
             R"({"contents":{"kind":"markdown","value":"```cpp\nint foo(int x)\n```\nReturns x."}})")) ==
             "int foo(int x)\nReturns x.",
         "markdown code fences are stripped, their content kept: [" +
             codec::ParseHoverContents(Json(
                 R"({"contents":{"kind":"markdown","value":"```cpp\nint foo(int x)\n```\nReturns x."}})")) +
             "]");
  Expect(codec::ParseHoverContents(Json(R"({"contents":["```\na\n```",{"language":"cpp","value":"b"}]})")) ==
             "a\n\nb",
         "fences are stripped inside a MarkedString array too");
  // No contents / empty.
  Expect(codec::ParseHoverContents(Json("{}")).empty(), "missing contents yields empty");
  Expect(codec::ParseHoverContents(Json(R"({"contents":[]})")).empty(),
         "empty contents array yields empty");
}

// J27: ParseHoverContents caps a hostile MarkedString[] by element count AND total
// bytes, truncating on a UTF-8 boundary with an explicit marker, so a near-64 MiB
// hover cannot force an unbounded concatenation on the callback path.
void TestLspProtocolHoverContentsAreBounded() {
  const std::string marker = "\n\n\xE2\x80\xA6 (truncated)";  // "\n\n… (truncated)"

  // A single 200 KiB element is truncated well below its input size and marked.
  {
    const std::string big(200 * 1024, 'x');
    const std::string body = std::string(R"({"contents":[")") + big + R"("]})";
    const std::string out = codec::ParseHoverContents(Json(body));
    Expect(out.size() < 64 * 1024, "a huge hover element is truncated below its input size");
    Expect(out.rfind(marker) != std::string::npos, "byte-cap truncation is explicitly marked");
  }

  // Hundreds of tiny elements are capped by element count and marked.
  {
    std::string many = R"({"contents":[)";
    for (int i = 0; i < 500; ++i) {
      if (i != 0) many += ',';
      many += "\"e\"";
    }
    many += "]}";
    const std::string out = codec::ParseHoverContents(Json(many));
    Expect(out.rfind(marker) != std::string::npos,
           "an over-long hover array is capped by element count and marked");
  }

  // A 3-byte code point ("→") payload is never split mid-character at the byte cap:
  // the join before the marker ends on a code-point boundary (a multiple of 3).
  {
    std::string arrows;
    while (arrows.size() < 200 * 1024) {
      arrows += "\xE2\x86\x92";
    }
    const std::string body = std::string(R"({"contents":[")") + arrows + R"("]})";
    const std::string out = codec::ParseHoverContents(Json(body));
    const auto mpos = out.rfind(marker);
    Expect(mpos != std::string::npos, "utf8 hover is truncated + marked");
    Expect(mpos % 3 == 0, "truncation lands on a UTF-8 boundary (no split multibyte char)");
  }

  // Regression: the single-string shapes were previously UNCAPPED. A bare
  // MarkedString and a MarkupContent {value} must be capped + marked too, so a
  // server cannot push a tens-of-MiB hover payload onto the callback/render path.
  {
    const std::string big(200 * 1024, 'y');
    const std::string bare = codec::ParseHoverContents(Json(std::string(R"({"contents":")") + big + R"("})"));
    Expect(bare.size() < 64 * 1024 && bare.rfind(marker) != std::string::npos,
           "a bare-string hover is capped and marked");
    const std::string value_body = std::string(R"({"contents":{"kind":"markdown","value":")") + big + R"("}})";
    const std::string value = codec::ParseHoverContents(Json(value_body));
    Expect(value.size() < 64 * 1024 && value.rfind(marker) != std::string::npos,
           "a MarkupContent {value} hover is capped and marked");
  }
}

// J26: LSP positions narrow to `int` deterministically. Out-of-int-range
// coordinates clamp to the nearest int bound rather than wrapping to an unrelated
// small/negative value.
void TestLspProtocolClampsOutOfRangePositions() {
  const LspClient::Position over =
      codec::ParsePosition(Json(R"({"line":2147483648,"character":9223372036854775807})"));
  Expect(over.line == 2147483647, "line INT_MAX+1 clamps to INT_MAX (no wrap to negative)");
  Expect(over.character == 2147483647, "character INT64_MAX clamps to INT_MAX");

  const LspClient::Position under =
      codec::ParsePosition(Json(R"({"line":-9223372036854775808,"character":-1})"));
  Expect(under.line == (-2147483647 - 1), "a huge negative line clamps to INT_MIN");
  Expect(under.character == -1, "an in-range negative character is preserved");
}

// TD-2026-07-17A-122: a fractional JSON number is not a valid protocol integer.
// JsonIntInRange (exercised here via ParsePosition/severity) must reject it and use
// the field's fallback rather than truncating 12.9 -> 12 and steering an edit or
// diagnostic to an adjacent-but-wrong location. Exact-integral doubles still narrow.
void TestLspProtocolRejectsFractionalIntegers() {
  const LspClient::Position fractional =
      codec::ParsePosition(Json(R"({"line":12.9,"character":4.5})"));
  Expect(fractional.line == 0, "a fractional line is rejected to the 0 fallback, not truncated to 12");
  Expect(fractional.character == 0, "a fractional character is rejected to the 0 fallback");

  const LspClient::Position integral_double =
      codec::ParsePosition(Json(R"({"line":12.0,"character":4.0})"));
  Expect(integral_double.line == 12, "an exact-integral double (12.0) is still accepted");
  Expect(integral_double.character == 4, "an exact-integral double (4.0) is still accepted");

  const LspClient::Position integer =
      codec::ParsePosition(Json(R"({"line":7,"character":3})"));
  Expect(integer.line == 7 && integer.character == 3, "a plain integer is accepted");
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

  // `documentChanges` array shape (TextDocumentEdit[] + resource ops).
  const JsonValue doc_changes = Json(R"({"documentChanges":[
      {"textDocument":{"uri":"file:///c.cpp","version":1},
       "edits":[{"range":{"start":{"line":1,"character":0},"end":{"line":1,"character":2}},"newText":"x"}]},
      {"kind":"rename","oldUri":"file:///old.cpp","newUri":"file:///new.cpp"}]})");
  const LspClient::WorkspaceEdit from_doc = codec::ParseWorkspaceEdit(doc_changes);
  Expect(from_doc.changes.size() == 1, "the resource-rename op adds no text-edit bucket");
  Expect(from_doc.changes.count("file:///c.cpp") == 1, "the TextDocumentEdit file is parsed");
  Expect(from_doc.resource_ops.size() == 1, "the resource-rename op is parsed");
  Expect(from_doc.expected_versions.at("file:///c.cpp") == 1,
         "the versioned TextDocumentEdit records its expected version");

  // Caps bound a hostile edit count.
  const JsonValue hostile = Json(R"({"changes":{"file:///h.cpp":[
      {"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}},"newText":"a"},
      {"range":{"start":{"line":1,"character":0},"end":{"line":1,"character":1}},"newText":"b"},
      {"range":{"start":{"line":2,"character":0},"end":{"line":2,"character":1}},"newText":"c"}]}})");
  const LspClient::WorkspaceEdit capped =
      codec::ParseWorkspaceEdit(hostile, /*max_files=*/10, /*max_edits_total=*/2);
  Expect(capped.changes.at("file:///h.cpp").size() == 2, "the total-edit cap truncates the list");
}

// TD-2026-07-17-011: WorkspaceEdit resource ops (create/rename/delete), their
// options, versioned TextDocumentEdits, and the pre-rename edit re-keying.
void TestLspProtocolParsesWorkspaceEditResourceOps() {
  using Op = LspClient::WorkspaceEdit::ResourceOp;
  const JsonValue edit_json = Json(R"({"documentChanges":[
      {"kind":"create","uri":"file:///new.rs","options":{"overwrite":true}},
      {"textDocument":{"uri":"file:///mod.rs","version":7},
       "edits":[{"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}},"newText":"m"}]},
      {"kind":"rename","oldUri":"file:///mod.rs","newUri":"file:///renamed.rs",
       "options":{"ignoreIfExists":true}},
      {"kind":"delete","uri":"file:///dead.rs","options":{"recursive":true,"ignoreIfNotExists":true}},
      {"kind":"warp","uri":"file:///nonsense"},
      {"kind":"create","uri":""}]})");
  const LspClient::WorkspaceEdit edit = codec::ParseWorkspaceEdit(edit_json);

  Expect(edit.resource_ops.size() == 3, "known well-formed ops parse; unknown/malformed drop");
  Expect(edit.resource_ops[0].kind == Op::Kind::Create && edit.resource_ops[0].uri == "file:///new.rs" &&
             edit.resource_ops[0].overwrite,
         "create op parses uri + overwrite option");
  Expect(edit.resource_ops[1].kind == Op::Kind::Rename &&
             edit.resource_ops[1].uri == "file:///mod.rs" &&
             edit.resource_ops[1].new_uri == "file:///renamed.rs" &&
             edit.resource_ops[1].ignore_if_exists,
         "rename op parses oldUri/newUri + ignoreIfExists");
  Expect(edit.resource_ops[2].kind == Op::Kind::Delete && edit.resource_ops[2].recursive &&
             edit.resource_ops[2].ignore_if_not_exists,
         "delete op parses recursive + ignoreIfNotExists");

  // The edit listed BEFORE the rename of its file follows the file to its new
  // name (the host applies all resource ops before any text edit).
  Expect(edit.changes.count("file:///mod.rs") == 0, "the pre-rename bucket is re-keyed away");
  Expect(edit.changes.at("file:///renamed.rs").size() == 1,
         "the pre-rename edit lands under the post-rename uri");
  // The version stays keyed by the URI the server actually sent.
  Expect(edit.expected_versions.at("file:///mod.rs") == 7,
         "expected version stays keyed by the original uri");

  // The resource-op count shares the file cap.
  const JsonValue many_ops = Json(R"({"documentChanges":[
      {"kind":"create","uri":"file:///1"},
      {"kind":"create","uri":"file:///2"},
      {"kind":"create","uri":"file:///3"}]})");
  const LspClient::WorkspaceEdit capped_ops =
      codec::ParseWorkspaceEdit(many_ops, /*max_files=*/2, /*max_edits_total=*/10);
  Expect(capped_ops.resource_ops.size() == 2, "resource ops are bounded by the file cap");

  // Empty-edit versioned entries bypass the edit budget, so the version map is
  // bounded by the file cap too (hostile-server backstop).
  const JsonValue many_versions = Json(R"({"documentChanges":[
      {"textDocument":{"uri":"file:///v1","version":1},"edits":[]},
      {"textDocument":{"uri":"file:///v2","version":2},"edits":[]},
      {"textDocument":{"uri":"file:///v3","version":3},"edits":[]}]})");
  const LspClient::WorkspaceEdit capped_versions =
      codec::ParseWorkspaceEdit(many_versions, /*max_files=*/2, /*max_edits_total=*/10);
  Expect(capped_versions.expected_versions.size() == 2,
         "expected versions are bounded by the file cap");
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

  // Regression: parameter label offsets are UTF-16 code units, not bytes. For a
  // non-ASCII label they must be converted before slicing. Label "f(é, β)" has
  // é (2 bytes) and β (2 bytes); the UTF-16 offsets of é are [2,3] and of β [5,6].
  // Byte-slicing those offsets would split the multibyte characters.
  const std::string unicode_label = "f(\xc3\xa9, \xce\xb2)";
  const JsonValue unicode = Json(std::string(R"({"signatures":[{"label":")") + unicode_label +
                                 R"(","parameters":[{"label":[2,3]},{"label":[5,6]}]}]})");
  const LspClient::SignatureHelp unicode_parsed = codec::ParseSignatureHelp(unicode);
  Expect(unicode_parsed.signatures.size() == 1, "unicode-label signature parses");
  Expect(unicode_parsed.signatures[0].parameters[0].label == "\xc3\xa9",
         "UTF-16 offsets [2,3] resolve to the whole é, not a split byte");
  Expect(unicode_parsed.signatures[0].parameters[1].label == "\xce\xb2",
         "UTF-16 offsets [5,6] resolve to the whole β");

  // Out-of-range offsets are ignored rather than reading past the label.
  const JsonValue bad = Json(R"({"signatures":[{"label":"z","parameters":[{"label":[3,9]}]}]})");
  const LspClient::SignatureHelp bad_parsed = codec::ParseSignatureHelp(bad);
  Expect(bad_parsed.signatures[0].parameters[0].label.empty(),
         "out-of-range offset label yields empty, not a read past the string");

  // A result with no signatures is empty (nothing to show).
  const LspClient::SignatureHelp none = codec::ParseSignatureHelp(Json("{}"));
  Expect(none.signatures.empty(), "missing signatures array yields no signatures");
}

void TestLspProtocolParsesWorkspaceSymbols() {
  const auto symbols = codec::ParseWorkspaceSymbols(Json(R"json([
      {"name":"Widget","kind":5,"containerName":"ui",
       "location":{"uri":"file:///w.cpp",
                   "range":{"start":{"line":9,"character":6},"end":{"line":9,"character":12}}}},
      {"name":"main","kind":12,
       "location":{"uri":"file:///m.cpp",
                   "range":{"start":{"line":0,"character":0},"end":{"line":0,"character":4}}}}])json"));
  Expect(symbols.size() == 2, "both workspace symbols parse");
  Expect(symbols[0].name == "Widget" && symbols[0].container_name == "ui" && symbols[0].kind == 5,
         "name / containerName / kind parsed");
  Expect(symbols[0].location.uri == "file:///w.cpp" && symbols[0].location.range.start.line == 9,
         "symbol location parsed");
  Expect(symbols[1].container_name.empty(), "absent containerName is empty");

  // Non-array yields nothing.
  Expect(codec::ParseWorkspaceSymbols(Json("{}")).empty(), "non-array yields no symbols");
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

void TestLspProtocolParsesInlayHints() {
  const auto hints = codec::ParseInlayHints(Json(R"json([
      {"position":{"line":3,"character":8},"label":": i32","kind":1,"paddingLeft":true},
      {"position":{"line":5,"character":10},
       "label":[{"value":"name"},{"value":":"}],"kind":2,"paddingRight":true},
      {"position":{"line":6,"character":0},"label":""}])json"));
  Expect(hints.size() == 2, "empty-label hint is dropped, leaving two");
  Expect(hints[0].label == ": i32" && hints[0].kind == 1, "string label + kind parsed");
  Expect(hints[0].position.line == 3 && hints[0].position.character == 8, "position parsed");
  Expect(hints[0].padding_left && !hints[0].padding_right, "paddingLeft parsed");
  Expect(hints[1].label == "name:" && hints[1].kind == 2, "label parts flattened by value");
  Expect(hints[1].padding_right, "paddingRight parsed");

  // Non-array yields nothing; the cap truncates.
  Expect(codec::ParseInlayHints(Json("null")).empty(), "non-array yields no hints");
  const auto capped = codec::ParseInlayHints(Json(R"json([
      {"position":{"line":0,"character":0},"label":"a"},
      {"position":{"line":1,"character":0},"label":"b"}])json"), /*max_hints=*/1);
  Expect(capped.size() == 1, "the hint cap truncates");

  // Regression: the 512-byte label cap must truncate on a UTF-8 boundary, not a
  // raw byte, so a multi-byte code point straddling the cap is dropped whole
  // rather than leaving a split sequence.
  std::string long_label(511, 'a');
  long_label += "\xC3\xBC";  // 'ü' (2 bytes); byte 512 lands on its lead byte
  const std::string payload =
      "[{\"position\":{\"line\":0,\"character\":0},\"label\":\"" + long_label + "\"}]";
  const auto utf8_capped = codec::ParseInlayHints(Json(payload));
  Expect(utf8_capped.size() == 1, "the oversized-label hint should still parse");
  Expect(utf8_capped[0].label.size() <= 512, "the label must be capped at the byte limit");
  bool has_high_byte = false;
  for (const unsigned char byte : utf8_capped[0].label) {
    if (byte >= 0x80) has_high_byte = true;
  }
  Expect(!has_high_byte,
         "UTF-8-boundary truncation must drop the straddling code point, leaving no "
         "split multi-byte sequence");
}

void TestLspProtocolParsesDocumentHighlights() {
  const auto highlights = codec::ParseDocumentHighlights(Json(R"json([
      {"range":{"start":{"line":2,"character":4},"end":{"line":2,"character":9}},"kind":2},
      {"range":{"start":{"line":7,"character":0},"end":{"line":7,"character":5}},"kind":3},
      {"range":{"start":{"line":9,"character":1},"end":{"line":9,"character":6}}}])json"));
  Expect(highlights.size() == 3, "three well-formed highlights parse");
  Expect(highlights[0].kind == 2 && highlights[0].range.start.character == 4, "read kind parsed");
  Expect(highlights[1].kind == 3, "write kind parsed");
  Expect(highlights[2].kind == 1, "an absent kind defaults to Text(1)");

  // An out-of-vocabulary kind keeps the range (the server's authoritative answer)
  // and only loses its read/write tint.
  const auto odd_kind = codec::ParseDocumentHighlights(Json(R"json([
      {"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":3}},"kind":99}])json"));
  Expect(odd_kind.size() == 1 && odd_kind[0].kind == 1,
         "an unknown kind falls back to Text rather than dropping the range");

  // Empty and inverted ranges paint nothing, so they never reach the store.
  const auto degenerate = codec::ParseDocumentHighlights(Json(R"json([
      {"range":{"start":{"line":1,"character":4},"end":{"line":1,"character":4}}},
      {"range":{"start":{"line":2,"character":9},"end":{"line":2,"character":4}}},
      {"range":{"start":{"line":3,"character":2},"end":{"line":2,"character":8}}}])json"));
  Expect(degenerate.empty(), "empty and inverted ranges are dropped");

  Expect(codec::ParseDocumentHighlights(Json("null")).empty(), "non-array yields no highlights");
  const auto capped = codec::ParseDocumentHighlights(Json(R"json([
      {"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}}},
      {"range":{"start":{"line":1,"character":0},"end":{"line":1,"character":1}}}])json"),
      /*max_highlights=*/1);
  Expect(capped.size() == 1, "the highlight cap truncates");
}

void TestLspProtocolParsesCodeLenses() {
  const auto lenses = codec::ParseCodeLenses(Json(R"json([
      {"range":{"start":{"line":4,"character":0},"end":{"line":4,"character":8}},
       "command":{"title":"3 references","command":"editor.showRefs","arguments":[1,"x"]}},
      {"range":{"start":{"line":9,"character":0},"end":{"line":9,"character":4}},
       "data":{"id":77}},
      "not-a-lens"])json"));
  Expect(lenses.size() == 2,
         "an entry that is not a lens object is dropped (nothing to paint or resolve)");
  Expect(lenses[0].title == "3 references" && lenses[0].command == "editor.showRefs",
         "title and command parsed from the nested command object");
  Expect(lenses[0].arguments.size() == 2, "command arguments are kept verbatim");
  Expect(lenses[0].unresolved.IsNull(), "a resolved lens carries nothing to resolve");
  Expect(lenses[1].needs_resolve(), "a title-less lens with data is marked for resolve");
  // The unresolved lens must round-trip VERBATIM: servers correlate through `data`
  // and reject an object we rebuilt from the parsed fields.
  Expect(lenses[1].unresolved["data"]["id"].AsInt(0) == 77,
         "the original lens object (including `data`) is preserved for codeLens/resolve");
  // `data` is optional in the protocol, so a bare range is still resolvable — it
  // must not be discarded as unpaintable before resolve has had a chance to run.
  const auto bare = codec::ParseCodeLenses(Json(R"json([
      {"range":{"start":{"line":1,"character":0},"end":{"line":1,"character":2}}}])json"));
  Expect(bare.size() == 1 && bare[0].needs_resolve(),
         "a range-only lens without `data` is still marked for resolve");

  // codeLens/resolve returns the same shape, now with the command filled in.
  const auto resolved = codec::ParseCodeLens(Json(R"json(
      {"range":{"start":{"line":9,"character":0},"end":{"line":9,"character":4}},
       "command":{"title":"Run test","command":"rust-analyzer.runSingle"}})json"));
  Expect(resolved.title == "Run test" && resolved.command == "rust-analyzer.runSingle",
         "a resolved lens parses its filled-in command");
  Expect(resolved.unresolved.IsNull(), "a resolved lens needs no further resolve");

  Expect(codec::ParseCodeLenses(Json("null")).empty(), "non-array yields no lenses");
  const auto capped = codec::ParseCodeLenses(Json(R"json([
      {"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}},
       "command":{"title":"a","command":"c"}},
      {"range":{"start":{"line":1,"character":0},"end":{"line":1,"character":1}},
       "command":{"title":"b","command":"c"}}])json"), /*max_lenses=*/1);
  Expect(capped.size() == 1, "the lens cap truncates");
}

void TestLspProtocolParsesCallHierarchy() {
  const auto items = codec::ParseCallHierarchyItems(Json(R"json([
      {"name":"alpha","detail":"void alpha()","kind":12,"uri":"file:///a.cpp",
       "range":{"start":{"line":3,"character":0},"end":{"line":8,"character":1}},
       "selectionRange":{"start":{"line":3,"character":5},"end":{"line":3,"character":10}},
       "data":{"tok":5}},
      {"name":"no-uri","kind":12,
       "range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}}}])json"));
  Expect(items.size() == 1, "an item with no uri is dropped — nothing could navigate to it");
  Expect(items[0].name == "alpha" && items[0].detail == "void alpha()", "name/detail parsed");
  Expect(items[0].selection_range.start.character == 5,
         "selectionRange (the name, where navigation lands) is kept separately from range");
  Expect(items[0].raw["data"]["tok"].AsInt(0) == 5,
         "the item is preserved verbatim for the follow-up calls request");

  // An item without selectionRange falls back to range rather than to (0,0).
  const auto no_selection = codec::ParseCallHierarchyItems(Json(R"json([
      {"name":"beta","uri":"file:///b.cpp",
       "range":{"start":{"line":9,"character":2},"end":{"line":9,"character":6}}}])json"));
  Expect(no_selection.size() == 1 && no_selection[0].selection_range.start.line == 9,
         "a missing selectionRange falls back to the item's range");

  // Incoming edges are keyed `from`; outgoing `to`. fromRanges is read from the
  // same key in both directions.
  const std::string calls_json = R"json([
      {"from":{"name":"caller","uri":"file:///c.cpp",
               "range":{"start":{"line":1,"character":0},"end":{"line":4,"character":1}}},
       "to":{"name":"callee","uri":"file:///d.cpp",
             "range":{"start":{"line":2,"character":0},"end":{"line":6,"character":1}}},
       "fromRanges":[{"start":{"line":2,"character":4},"end":{"line":2,"character":9}},
                     {"start":{"line":3,"character":4},"end":{"line":3,"character":9}}]}])json";
  const auto incoming = codec::ParseCallHierarchyCalls(Json(calls_json), /*incoming=*/true);
  Expect(incoming.size() == 1 && incoming[0].item.name == "caller",
         "incoming calls take the far end from `from`");
  Expect(incoming[0].call_ranges.size() == 2, "both call sites parsed");
  const auto outgoing = codec::ParseCallHierarchyCalls(Json(calls_json), /*incoming=*/false);
  Expect(outgoing.size() == 1 && outgoing[0].item.name == "callee",
         "outgoing calls take the far end from `to`");
  Expect(outgoing[0].call_ranges.size() == 2, "fromRanges is read the same way in both directions");

  Expect(codec::ParseCallHierarchyItems(Json("null")).empty(), "non-array yields no items");
  Expect(codec::ParseCallHierarchyCalls(Json("null"), true).empty(), "non-array yields no calls");
}

}  // namespace

void RegisterLspProtocolTests(std::vector<TestCase>& tests) {
  AddTest(tests, "LspProtocol/ParsesCodeLenses", TestLspProtocolParsesCodeLenses);
  AddTest(tests, "LspProtocol/ParsesCallHierarchy", TestLspProtocolParsesCallHierarchy);
  AddTest(tests, "LspProtocol/ParsesInlayHints", TestLspProtocolParsesInlayHints);
  AddTest(tests, "LspProtocol/ParsesDocumentHighlights", TestLspProtocolParsesDocumentHighlights);
  AddTest(tests, "LspProtocol/ParsesWorkspaceEdit", TestLspProtocolParsesWorkspaceEdit);
  AddTest(tests, "LspProtocol/ParsesWorkspaceEditResourceOps",
          TestLspProtocolParsesWorkspaceEditResourceOps);
  AddTest(tests, "LspProtocol/ParsesSignatureHelp", TestLspProtocolParsesSignatureHelp);
  AddTest(tests, "LspProtocol/ParsesWorkspaceSymbols", TestLspProtocolParsesWorkspaceSymbols);
  AddTest(tests, "LspProtocol/ParsesPrepareRename", TestLspProtocolParsesPrepareRename);
  AddTest(tests, "LspProtocol/ParsesTextEdits", TestLspProtocolParsesTextEdits);
  AddTest(tests, "LspProtocol/ParsesHoverContents", TestLspProtocolParsesHoverContents);
  AddTest(tests, "LspProtocol/HoverContentsAreBounded", TestLspProtocolHoverContentsAreBounded);
  AddTest(tests, "LspProtocol/ClampsOutOfRangePositions",
          TestLspProtocolClampsOutOfRangePositions);
  AddTest(tests, "LspProtocol/RejectsFractionalIntegers",
          TestLspProtocolRejectsFractionalIntegers);
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
