#include "TestSupport.h"

#include <cstdint>
#include <optional>
#include <string>

#include "util/JsonValue.h"
#include "workspace/control/ControlProtocol.h"
#include "workspace/control/ControlSpec.h"
#include "workspace/ManPage.h"

namespace microide::tests {
namespace {

using microide::workspace::ControlChannelHelpText;
using microide::workspace::RenderManPage;
using microide::workspace::ControlQueryVerbs;
using microide::workspace::ControlRequest;
using microide::workspace::ControlResponse;
using microide::workspace::ControlSpecKeys;
using microide::workspace::ParseControlRequest;
using microide::workspace::SerializeControlResponse;
using microide::util::JsonValue;

// Property: a response always survives its own wire format, and no input line
// can make the parser lie or throw.
//
// The control channel is the surface an external tool — an LLM driver, a script
// — talks to, so its inputs are the least trusted in the app and its outputs are
// the ones a machine parses. Two properties cover the ways that goes wrong:
//
//   * ROUND TRIP. A serialized response must parse back as JSON and carry the
//     same id / ok / feedback / error / result. Feedback and error are built
//     from arbitrary product text — file paths, git output, error messages — so
//     they routinely contain quotes, backslashes, newlines, control bytes and
//     raw UTF-8. Any of those escaped wrongly produces a line the client cannot
//     parse, or worse, one that parses into something else.
//   * TOTALITY. `ParseControlRequest` is documented to never throw and to report
//     malformed input as {valid=false}. Feed it structurally hostile lines and
//     require exactly that: it returns, and a request it calls valid really does
//     carry exactly one of command/query.

std::string ControlRoundTripBody(std::size_t choice) {
  static const char* const kBodies[] = {
      "",
      "plain feedback",
      "with \"quotes\" and \\backslash\\",
      "line1\nline2\r\nline3",
      "tab\there and \x01 control",
      "\xc3\xa9 non-ascii \xe4\xb8\xad \xf0\x9f\x98\x80",
      "trailing backslash \\",
      "json-looking {\"id\":7,\"ok\":false}",
      "very long ................................................................",
  };
  return kBodies[choice % (sizeof(kBodies) / sizeof(kBodies[0]))];
}

void TestControlResponseSurvivesItsOwnWireFormat() {
  std::uint64_t seed = 0xCBBB9D5DC1059ED8ULL;
  const auto next = [&seed]() {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<std::size_t>(seed >> 33);
  };
  for (int iteration = 0; iteration < 300; ++iteration) {
    ControlResponse response;
    if (next() % 4 != 0) {
      response.id = static_cast<std::int64_t>(next()) - 1000;
    }
    response.ok = (next() % 2) == 0;
    response.feedback = ControlRoundTripBody(next());
    response.error = ControlRoundTripBody(next());
    if (next() % 3 == 0) {
      util::JsonObject object;
      object["text"] = util::JsonValue(ControlRoundTripBody(next()));
      object["count"] = util::JsonValue(static_cast<std::int64_t>(next() % 1000));
      response.result = util::JsonValue(std::move(object));
    }

    const std::string line = SerializeControlResponse(response);
    Expect(line.find('\n') == std::string::npos,
           "a response must be one line — the socket layer frames on newlines");

    const std::optional<util::JsonValue> parsed = util::ParseJson(line);
    Expect(parsed.has_value() && parsed->IsObject(),
           "a serialized response must parse back as a JSON object");
    const util::JsonValue& value = *parsed;
    Expect(value["ok"].IsBool() && value["ok"].AsBool(!response.ok) == response.ok,
           "ok must survive the round trip");
    if (response.id.has_value()) {
      Expect(value["id"].AsInt(*response.id + 1) == *response.id, "id must survive");
    }
    // feedback/error are only emitted when non-empty; when emitted they must be
    // byte-identical, which is what the escaping has to get right.
    if (!response.feedback.empty()) {
      Expect(value["feedback"].AsString() == response.feedback,
             "feedback must survive the round trip byte for byte");
    }
    if (!response.error.empty()) {
      Expect(value["error"].AsString() == response.error,
             "error must survive the round trip byte for byte");
    }
  }
}

void TestControlRequestParserIsTotal() {
  static const char* const kHostileLines[] = {
      "", " ", "\n", "{", "}", "[]", "null", "true", "42", "\"string\"",
      "{}", "{\"id\":1}", "{\"command\":\"\"}", "{\"query\":\"\"}",
      "{\"command\":\"open\",\"query\":\"tabs\"}",           // both set
      "{\"command\":null}", "{\"query\":null}", "{\"id\":null,\"command\":\"open\"}",
      "{\"id\":\"not-a-number\",\"command\":\"open\"}",
      "{\"id\":9223372036854775807,\"command\":\"open\"}",
      "{\"id\":-9223372036854775808,\"command\":\"open\"}",
      "{\"id\":1.5,\"command\":\"open\"}",
      "{\"command\":123}", "{\"command\":[\"open\"]}", "{\"command\":{\"a\":1}}",
      "{\"command\":\"open \\u0000 nul\"}",
      "{\"command\":\"\\ud800\"}",                            // lone surrogate
      "{\"args\":{\"a\":1},\"query\":\"tabs\"}",
      "{\"query\":\"tabs\",\"args\":[1,2,3]}",
      "{\"query\":\"tabs\",\"args\":\"not-an-object\"}",
      "  {\"command\":\"open\"}  ",
      "{\"command\":\"open\"}trailing",
      "{\"command\":\"open\"}\n{\"command\":\"save\"}",       // two objects on a line
  };
  for (const char* line : kHostileLines) {
    const ControlRequest request = ParseControlRequest(line);
    if (!request.valid) {
      Expect(!request.parse_error.empty(),
             "an invalid request must say why — the client prints this");
      continue;
    }
    Expect(request.is_command() != request.is_query(),
           "a valid request carries exactly one of command / query");
  }

  // And over generated junk, including embedded NULs and arbitrary bytes: the
  // contract is that it returns at all.
  std::uint64_t seed = 0x629A292A367CD507ULL;
  const auto next = [&seed]() {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<std::size_t>(seed >> 33);
  };
  static const char kAlphabet[] = "{}[]\",:\\ \t\n0123456789abctruefalsnl\x01\x7f\xc3\xa9";
  for (int iteration = 0; iteration < 2000; ++iteration) {
    std::string line;
    const std::size_t length = next() % 40;
    for (std::size_t i = 0; i < length; ++i) {
      line.push_back(kAlphabet[next() % (sizeof(kAlphabet) - 1)]);
    }
    const ControlRequest request = ParseControlRequest(line);
    Expect(!request.valid || (request.is_command() != request.is_query()),
           "a valid request carries exactly one of command / query");
    Expect(request.valid || !request.parse_error.empty(),
           "an invalid request must report a reason");
  }
}

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

// The runbook the agent reads must lead with control-send and never resurrect the
// broken `set-setting debug.enabled true` prelude or socat guidance.
void TestHelpTextLeadsWithControlSend() {
  const std::string help = ControlChannelHelpText();
  Expect(help.find("control-send") != std::string::npos,
         "help text should document the control-send client");
  Expect(help.find("debug-run") != std::string::npos,
         "help text should document ad-hoc debug-run");
  Expect(help.find("socat") == std::string::npos,
         "help text must not carry the broken socat recipe");
  Expect(help.find("set-setting debug.enabled true") == std::string::npos,
         "the debug.enabled prelude is obsolete now that the channel auto-enables");
}

// The committed man page is generated from RenderManPage(); fail if it was edited
// by hand or left stale so the shipped docs cannot drift from the implementation.
void TestManPageMatchesGenerator() {
  const std::filesystem::path repo_root =
      std::filesystem::path(MICROIDE_TEST_SOURCE_DIR).parent_path();
  const std::filesystem::path man_path = repo_root / "docs" / "microide.1";
  const std::string committed = ReadFile(man_path);
  Expect(!committed.empty(), "docs/microide.1 should exist and be non-empty");
  Expect(committed == RenderManPage(),
         "docs/microide.1 is stale -- run tools/gen-man.sh to regenerate it");
}

// Negative guard: the docs an agent might read must not carry the broken socat
// recipe, and must point at control-send instead.
void TestDocsHaveNoSocatRecipe() {
  const std::filesystem::path repo_root =
      std::filesystem::path(MICROIDE_TEST_SOURCE_DIR).parent_path();
  for (const std::filesystem::path& doc :
       {repo_root / "docs" / "microide.1",
        repo_root / "dev-docs" / "control" / "control-channel.md"}) {
    const std::string text = ReadFile(doc);
    Expect(!text.empty(), "doc should exist and be non-empty");
    Expect(text.find("socat") == std::string::npos,
           "doc must not carry the broken socat recipe");
    Expect(text.find("control-send") != std::string::npos,
           "doc should point at the control-send client");
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
  AddTest(tests, "ControlProtocol/HelpTextLeadsWithControlSend",
          TestHelpTextLeadsWithControlSend);
  AddTest(tests, "ControlProtocol/ManPageMatchesGenerator", TestManPageMatchesGenerator);
  AddTest(tests, "ControlProtocol/DocsHaveNoSocatRecipe", TestDocsHaveNoSocatRecipe);
  AddTest(tests, "ControlProtocol/ResponseSurvivesItsOwnWireFormat",
          TestControlResponseSurvivesItsOwnWireFormat);
  AddTest(tests, "ControlProtocol/RequestParserIsTotal", TestControlRequestParserIsTotal);
}

}  // namespace microide::tests
