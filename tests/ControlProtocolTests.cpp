#include "TestSupport.h"

#include <string>

#include "util/JsonValue.h"
#include "workspace/ControlProtocol.h"
#include "workspace/ControlSpec.h"

namespace microide::tests {
namespace {

using microide::workspace::ControlChannelHelpText;
using microide::workspace::ControlQueryVerbs;
using microide::workspace::ControlRequest;
using microide::workspace::ControlResponse;
using microide::workspace::ControlSpecKeys;
using microide::workspace::ParseControlRequest;
using microide::workspace::SerializeControlResponse;

void TestParseCommandRequest() {
  const ControlRequest request = ParseControlRequest(R"({"id":7,"command":"debug-step-over"})");
  Expect(request.valid, "command request should be valid");
  Expect(request.is_command(), "should be a command request");
  Expect(request.id.has_value() && *request.id == 7, "id should round-trip");
  Expect(request.command == "debug-step-over", "command string should round-trip");
}

void TestParseQueryRequest() {
  const ControlRequest request = ParseControlRequest(R"({"query":"debug-state"})");
  Expect(request.valid, "query request should be valid");
  Expect(request.is_query(), "should be a query request");
  Expect(!request.id.has_value(), "missing id should stay empty");
  Expect(request.query == "debug-state", "query verb should round-trip");
}

void TestParseRejectsMalformed() {
  Expect(!ParseControlRequest("not json").valid, "garbage should be rejected");
  Expect(!ParseControlRequest("[]").valid, "non-object should be rejected");
  Expect(!ParseControlRequest(R"({})").valid, "missing command/query should be rejected");
  Expect(!ParseControlRequest(R"({"command":"x","query":"y"})").valid,
         "both command and query should be rejected");
  Expect(!ParseControlRequest(R"({"command":""})").valid, "empty command should be rejected");
  Expect(!ParseControlRequest(R"({"id":"x","command":"y"})").valid,
         "non-integer id should be rejected");
}

void TestSerializeResponseRoundTrips() {
  ControlResponse response;
  response.id = 3;
  response.ok = true;
  response.feedback = "done";
  const std::string line = SerializeControlResponse(response);
  const auto parsed = util::ParseJson(line);
  Expect(parsed.has_value() && parsed->IsObject(), "response should serialize to a JSON object");
  Expect((*parsed)["id"].AsInt() == 3, "id should serialize");
  Expect((*parsed)["ok"].AsBool() == true, "ok should serialize");
  Expect((*parsed)["feedback"].AsString() == "done", "feedback should serialize");
  Expect(line.find('\n') == std::string::npos, "serialized response must be a single line");
}

void TestHelpTextListsVerbsAndSpecKeys() {
  const std::string help = ControlChannelHelpText();
  Expect(!help.empty(), "help text should be non-empty");
  for (const std::string_view verb : ControlQueryVerbs()) {
    Expect(help.find(verb) != std::string::npos,
           "help text should document every query verb");
  }
  for (const std::string_view key : ControlSpecKeys()) {
    Expect(help.find(key) != std::string::npos, "help text should document every spec key");
  }
}

}  // namespace

void RegisterControlProtocolTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ControlProtocol/ParseCommandRequest", TestParseCommandRequest);
  AddTest(tests, "ControlProtocol/ParseQueryRequest", TestParseQueryRequest);
  AddTest(tests, "ControlProtocol/ParseRejectsMalformed", TestParseRejectsMalformed);
  AddTest(tests, "ControlProtocol/SerializeResponseRoundTrips", TestSerializeResponseRoundTrips);
  AddTest(tests, "ControlProtocol/HelpTextListsVerbsAndSpecKeys",
          TestHelpTextListsVerbsAndSpecKeys);
}

}  // namespace microide::tests
