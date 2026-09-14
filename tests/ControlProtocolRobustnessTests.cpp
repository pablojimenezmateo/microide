// Robustness and framing properties for the control channel wire.
//
// The control channel is JSONL over a socket: one request per line in, one
// response per line out. Two things must hold for arbitrary input, because the
// peer is an external program (a script, an agent) and not a cooperating part of
// this process:
//
//   1. ParseControlRequest must never throw and never accept garbage as valid.
//      It sees whatever bytes arrive, including truncated frames and binary.
//   2. A serialized response must be exactly ONE line. If any field's text can
//      smuggle a newline through unescaped, the client reads it as two frames
//      and every subsequent response is attributed to the wrong request -- a
//      desync that persists for the life of the connection, from nothing worse
//      than a filename with a newline in it.

#include "TestSupport.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "util/JsonValue.h"
#include "workspace/control/ControlProtocol.h"

namespace microide::tests {
namespace {

using microide::util::JsonValue;
using microide::util::ParseJson;
using microide::workspace::ControlRequest;
using microide::workspace::ControlResponse;
using microide::workspace::ParseControlRequest;
using microide::workspace::SerializeControlEvent;
using microide::workspace::SerializeControlResponse;

// Strings chosen to break a hand-rolled escaper: quotes, backslashes, every
// flavour of line break, control characters, a NUL, a lone surrogate's bytes,
// and invalid UTF-8.
const std::vector<std::string>& HostileText() {
  static const std::vector<std::string> text = {
      "",
      "plain",
      "with \"quotes\"",
      "with \\ backslash",
      "with \\\" both",
      "line\nbreak",
      "carriage\rreturn",
      "crlf\r\nboth",
      "tab\there",
      std::string("nul\0inside", 10),
      "\x01\x02\x1f control",
      "\x7f delete",
      "unicode \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E",
      "emoji \xF0\x9F\x98\x80",
      "invalid \xFF\xFE utf8",
      "truncated \xE6\x97",
      "lone surrogate bytes \xED\xA0\x80",
      "}{ braces ][",
      "\"}\n{\"",  // a whole fake frame boundary
  };
  return text;
}

std::string Render(std::string_view text) {
  std::string out;
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte >= 0x20 && byte < 0x7F) {
      out.push_back(c);
    } else {
      char buffer[8];
      std::snprintf(buffer, sizeof(buffer), "\\x%02X", byte);
      out += buffer;
    }
  }
  return out;
}

// ---- 1. a response is always exactly one line --------------------------------

void TestSerializedResponseIsAlwaysASingleLine() {
  for (const std::string& text : HostileText()) {
    for (const bool ok : {true, false}) {
      ControlResponse response;
      response.id = 7;
      response.ok = ok;
      response.feedback = text;
      response.error = text;
      response.result = JsonValue(text);

      const std::string line = SerializeControlResponse(response);
      Expect(line.find('\n') == std::string::npos,
             "a serialized response must not contain a raw newline: " + Render(text));
      Expect(line.find('\r') == std::string::npos,
             "a serialized response must not contain a raw carriage return: " + Render(text));
      Expect(line.find('\0') == std::string::npos,
             "a serialized response must not contain a raw NUL: " + Render(text));

      // And it must still be readable as JSON by the client on the other end.
      const auto reparsed = ParseJson(line);
      Expect(reparsed.has_value(),
             "a serialized response must be valid JSON, got '" + Render(line) + "' for " +
                 Render(text));
    }
  }
}

void TestSerializedEventIsAlwaysASingleLine() {
  for (const std::string& text : HostileText()) {
    util::JsonObject object;
    object.AppendInDocumentOrder("event", JsonValue(text));
    object.AppendInDocumentOrder("detail", JsonValue(text));
    const std::string line = SerializeControlEvent(JsonValue(std::move(object)));

    Expect(line.find('\n') == std::string::npos,
           "a serialized event must not contain a raw newline: " + Render(text));
    Expect(line.find('\r') == std::string::npos,
           "a serialized event must not contain a raw carriage return: " + Render(text));
    Expect(ParseJson(line).has_value(),
           "a serialized event must be valid JSON for " + Render(text));
  }
}

// The round trip that matters: whatever text goes into a response field comes
// back out of the client's parse byte for byte.
void TestResponseFieldsSurviveTheRoundTrip() {
  for (const std::string& text : HostileText()) {
    ControlResponse response;
    response.id = 42;
    response.ok = true;
    response.feedback = text;

    const auto parsed = ParseJson(SerializeControlResponse(response));
    Expect(parsed.has_value(), "the response should parse back: " + Render(text));
    if (!parsed->HasKey("feedback")) {
      continue;  // the field may legitimately be omitted when empty
    }
    const JsonValue& feedback = (*parsed)["feedback"];
    if (!feedback.IsString()) {
      continue;
    }
    Expect(feedback.AsString() == text,
           "feedback must survive the round trip: sent " + Render(text) + ", got " +
               Render(feedback.AsString()));
  }
}

// ---- 2. parsing never throws and never accepts garbage -----------------------

void TestParseRejectsMalformedInputWithoutThrowing() {
  const std::vector<std::string> malformed = {
      "",
      " ",
      "\n",
      "not json at all",
      "{",
      "}",
      "[]",
      "[1,2,3]",
      "null",
      "true",
      "42",
      "\"a string\"",
      "{\"command\":}",
      "{\"command\":123}",
      "{\"command\":null}",
      "{\"query\":[]}",
      "{\"id\":\"not a number\",\"command\":\"x\"}",
      "{}",                                   // neither command nor query
      "{\"command\":\"\",\"query\":\"\"}",     // both empty
      "{\"command\":\"a\",\"query\":\"b\"}",   // both set
      std::string("{\"command\":\"a\0b\"}", 18),
      "{\"command\":\"unterminated",
      "{\"command\":\"\xFF\xFE\"}",
      "{\"args\":{\"x\":1}}",                  // args with no verb
  };
  for (const std::string& line : malformed) {
    const ControlRequest request = ParseControlRequest(line);
    if (request.valid) {
      Expect(request.is_command() != request.is_query(),
             "a request accepted as valid must be exactly one of command/query: " +
                 Render(line));
    } else {
      Expect(!request.parse_error.empty(),
             "a rejected request must say why: " + Render(line));
    }
  }
}

// Every prefix of a well-formed request is a frame that could arrive truncated.
// None may be accepted, and none may crash.
void TestEveryTruncationOfAValidRequestIsRejected() {
  const std::vector<std::string> valid = {
      "{\"id\":1,\"command\":\"save\"}",
      "{\"id\":2,\"query\":\"state\"}",
      "{\"id\":3,\"query\":\"state\",\"args\":{\"deep\":{\"x\":[1,2,3]}}}",
  };
  for (const std::string& line : valid) {
    Expect(ParseControlRequest(line).valid, "the intact request must parse: " + line);
    for (std::size_t length = 0; length < line.size(); ++length) {
      const ControlRequest request = ParseControlRequest(line.substr(0, length));
      Expect(!request.valid,
             "a truncated request must not be accepted: '" + line.substr(0, length) + "'");
      Expect(!request.parse_error.empty(), "a truncated request must say why");
    }
  }
}

// Single-byte corruption of a valid frame: must never be accepted as a DIFFERENT
// valid request without saying so, and must never crash.
void TestByteCorruptionOfAValidRequestStaysSafe() {
  const std::string base = "{\"id\":1,\"command\":\"save\"}";
  for (std::size_t index = 0; index < base.size(); ++index) {
    for (const char replacement : {'\0', '\n', '"', '\\', '{', '}', '[', ']', ':', ',', '\xFF'}) {
      std::string corrupted = base;
      corrupted[index] = replacement;
      const ControlRequest request = ParseControlRequest(corrupted);
      if (request.valid) {
        Expect(request.is_command() != request.is_query(),
               "a corrupted frame accepted as valid must still name exactly one verb: " +
                   Render(corrupted));
      } else {
        Expect(!request.parse_error.empty(),
               "a rejected corrupted frame must say why: " + Render(corrupted));
      }
    }
  }
}

// Deeply nested args are the classic recursive-descent stack overflow. The
// parser must refuse depth rather than recurse into it.
void TestDeeplyNestedArgsDoNotOverflow() {
  for (const std::size_t depth : {std::size_t{16}, std::size_t{256}, std::size_t{20000}}) {
    std::string line = "{\"query\":\"state\",\"args\":";
    line += std::string(depth, '[');
    line += std::string(depth, ']');
    line += "}";
    const ControlRequest request = ParseControlRequest(line);
    Expect(request.valid || !request.parse_error.empty(),
           "a deeply nested frame must either parse or explain itself, depth " +
               std::to_string(depth));
  }
  for (const std::size_t depth : {std::size_t{16}, std::size_t{256}, std::size_t{20000}}) {
    std::string line = "{\"query\":\"state\",\"args\":";
    for (std::size_t i = 0; i < depth; ++i) line += "{\"k\":";
    line += "1";
    line += std::string(depth, '}');
    line += "}";
    const ControlRequest request = ParseControlRequest(line);
    Expect(request.valid || !request.parse_error.empty(),
           "a deeply nested object must either parse or explain itself, depth " +
               std::to_string(depth));
  }
}

}  // namespace

void RegisterControlProtocolRobustnessTests(std::vector<TestCase>& tests) {
  AddTest(tests, "ControlProtocolRobustness/SerializedResponseIsAlwaysASingleLine",
          TestSerializedResponseIsAlwaysASingleLine);
  AddTest(tests, "ControlProtocolRobustness/SerializedEventIsAlwaysASingleLine",
          TestSerializedEventIsAlwaysASingleLine);
  AddTest(tests, "ControlProtocolRobustness/ResponseFieldsSurviveTheRoundTrip",
          TestResponseFieldsSurviveTheRoundTrip);
  AddTest(tests, "ControlProtocolRobustness/ParseRejectsMalformedInputWithoutThrowing",
          TestParseRejectsMalformedInputWithoutThrowing);
  AddTest(tests, "ControlProtocolRobustness/EveryTruncationOfAValidRequestIsRejected",
          TestEveryTruncationOfAValidRequestIsRejected);
  AddTest(tests, "ControlProtocolRobustness/ByteCorruptionOfAValidRequestStaysSafe",
          TestByteCorruptionOfAValidRequestStaysSafe);
  AddTest(tests, "ControlProtocolRobustness/DeeplyNestedArgsDoNotOverflow",
          TestDeeplyNestedArgsDoNotOverflow);
}

}  // namespace microide::tests
