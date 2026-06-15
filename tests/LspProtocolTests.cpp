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

}  // namespace

void RegisterLspProtocolTests(std::vector<TestCase>& tests) {
  AddTest(tests, "LspProtocol/ParsesPositionAndRange", TestLspProtocolParsesPositionAndRange);
  AddTest(tests, "LspProtocol/ParsesLocations", TestLspProtocolParsesLocationsArrayAndSingleAndLink);
  AddTest(tests, "LspProtocol/ParsesDiagnosticSeverityDefault",
          TestLspProtocolParsesDiagnosticSeverityDefault);
  AddTest(tests, "LspProtocol/ParsesDocumentSymbolShapes",
          TestLspProtocolParsesDocumentSymbolShapes);
  AddTest(tests, "LspProtocol/EncodersRoundTrip", TestLspProtocolEncodersRoundTrip);
}

}  // namespace microide::tests
