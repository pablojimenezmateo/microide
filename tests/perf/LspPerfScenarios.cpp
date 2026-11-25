#include "perf/PerfHarness.h"

#include "util/JsonValue.h"
#include "workspace/JsonRpcMessageFraming.h"
#include "workspace/lsp/LspProtocol.h"
#include "workspace/lsp/WorkspaceLspClient.h"

#include <cstdint>
#include <string>
#include <vector>

namespace microide::tests::perf {
namespace {

namespace codec = microide::workspace::lsp_protocol;
using microide::util::JsonArray;
using microide::util::JsonObject;
using microide::util::JsonValue;
using microide::util::SerializeJson;
using microide::workspace::JsonRpcMessageFramer;

JsonValue MakeSemanticTokensResult(int token_count) {
  JsonArray data;
  data.reserve(static_cast<std::size_t>(token_count) * 5);
  for (int i = 0; i < token_count; ++i) {
    const bool new_line = (i % 8) == 0;
    const std::int64_t delta_line = new_line ? 1 : 0;
    const std::int64_t delta_start = new_line ? (i % 40) : (5 + (i % 10));
    data.push_back(JsonValue(delta_line));
    data.push_back(JsonValue(delta_start));
    data.push_back(JsonValue(static_cast<std::int64_t>(4 + (i % 8))));
    data.push_back(JsonValue(static_cast<std::int64_t>(i % 15)));
    data.push_back(JsonValue(static_cast<std::int64_t>(0)));
  }
  JsonObject result;
  result["data"] = JsonValue(std::move(data));
  return JsonValue(std::move(result));
}

JsonValue MakeDiagnosticsArray(int count) {
  JsonArray diags;
  diags.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    JsonObject start;
    start["line"] = JsonValue(static_cast<std::int64_t>(i));
    start["character"] = JsonValue(static_cast<std::int64_t>(i % 32));
    JsonObject end;
    end["line"] = JsonValue(static_cast<std::int64_t>(i));
    end["character"] = JsonValue(static_cast<std::int64_t>((i % 32) + 8));
    JsonObject range;
    range["start"] = JsonValue(std::move(start));
    range["end"] = JsonValue(std::move(end));

    JsonObject d;
    d["range"] = JsonValue(std::move(range));
    d["message"] = JsonValue(std::string("unused variable 'ident") + std::to_string(i) + "'");
    d["severity"] = JsonValue(static_cast<std::int64_t>((i % 4) + 1));
    if (i % 2 == 0) {
      d["code"] = JsonValue(static_cast<std::int64_t>(2000 + i));
    } else {
      d["code"] = JsonValue(std::string("no-unused-vars"));
    }
    diags.push_back(JsonValue(std::move(d)));
  }
  return JsonValue(std::move(diags));
}

JsonValue MakeSymbolNode(const std::string& name, int kind, int line, JsonArray children) {
  JsonObject start;
  start["line"] = JsonValue(static_cast<std::int64_t>(line));
  start["character"] = JsonValue(static_cast<std::int64_t>(0));
  JsonObject end;
  end["line"] = JsonValue(static_cast<std::int64_t>(line + 1));
  end["character"] = JsonValue(static_cast<std::int64_t>(0));
  JsonObject range;
  range["start"] = JsonValue(std::move(start));
  range["end"] = JsonValue(std::move(end));

  JsonObject sym;
  sym["name"] = JsonValue(name);
  sym["detail"] = JsonValue(std::string("detail"));
  sym["kind"] = JsonValue(static_cast<std::int64_t>(kind));
  sym["range"] = range;
  sym["selectionRange"] = std::move(range);
  sym["children"] = JsonValue(std::move(children));
  return JsonValue(std::move(sym));
}

JsonValue MakeDocumentSymbolsResult(int classes, int methods, int fields) {
  JsonArray top;
  top.reserve(static_cast<std::size_t>(classes));
  int line = 0;
  for (int c = 0; c < classes; ++c) {
    JsonArray method_nodes;
    method_nodes.reserve(static_cast<std::size_t>(methods));
    for (int m = 0; m < methods; ++m) {
      JsonArray field_nodes;
      field_nodes.reserve(static_cast<std::size_t>(fields));
      for (int f = 0; f < fields; ++f) {
        field_nodes.push_back(
            MakeSymbolNode("field" + std::to_string(f), 8, line++, JsonArray{}));
      }
      method_nodes.push_back(MakeSymbolNode("method" + std::to_string(m), 6, line++,
                                            std::move(field_nodes)));
    }
    top.push_back(MakeSymbolNode("Class" + std::to_string(c), 5, line++,
                                 std::move(method_nodes)));
  }
  return JsonValue(std::move(top));
}

std::string MakeFramedStream(int count) {
  std::string stream;
  for (int i = 0; i < count; ++i) {
    JsonObject params;
    params["uri"] = JsonValue(std::string("file:///src/file") + std::to_string(i) + ".cpp");
    params["version"] = JsonValue(static_cast<std::int64_t>(i));
    JsonObject msg;
    msg["jsonrpc"] = JsonValue(std::string("2.0"));
    msg["method"] = JsonValue(std::string("textDocument/publishDiagnostics"));
    msg["params"] = JsonValue(std::move(params));
    const std::string body = SerializeJson(JsonValue(std::move(msg)));
    stream += "Content-Length: ";
    stream += std::to_string(body.size());
    stream += "\r\n\r\n";
    stream += body;
  }
  return stream;
}

void RunLspSemanticTokensDecode(ScenarioContext& context) {
  const JsonValue result = MakeSemanticTokensResult(8000);
  context.Measure("lsp_semantic_tokens.decode", [&]() {
    for (int iter = 0; iter < 200; ++iter) {
      const auto tokens = codec::ParseSemanticTokensData(result);
      volatile std::size_t sink = tokens.size();
      (void)sink;
    }
  });
}

void RunLspPublishDiagnosticsParse(ScenarioContext& context) {
  const JsonValue diagnostics = MakeDiagnosticsArray(1000);
  context.Measure("lsp_diagnostics.parse", [&]() {
    for (int iter = 0; iter < 200; ++iter) {
      const auto parsed = codec::ParseDiagnostics(diagnostics);
      volatile std::size_t sink = parsed.size();
      (void)sink;
    }
  });
}

void RunLspDocumentSymbolsParse(ScenarioContext& context) {
  const JsonValue symbols =
      MakeDocumentSymbolsResult(30, 15, 3);
  context.Measure("lsp_document_symbols.parse", [&]() {
    for (int iter = 0; iter < 200; ++iter) {
      const auto parsed = codec::ParseDocumentSymbols(symbols);
      volatile std::size_t sink = parsed.size();
      (void)sink;
    }
  });
}

void RunJsonRpcMessageFraming(ScenarioContext& context) {
  const std::string stream = MakeFramedStream(500);
  constexpr std::size_t kChunk = 1500;
  context.Measure("lsp_message_framing.parse", [&]() {
    for (int iter = 0; iter < 40; ++iter) {
      JsonRpcMessageFramer framer;
      std::size_t messages = 0;
      for (std::size_t off = 0; off < stream.size(); off += kChunk) {
        framer.Append(std::string_view(stream).substr(off, kChunk));
        while (framer.Next().has_value()) {
          ++messages;
        }
      }
      volatile std::size_t sink = messages;
      (void)sink;
    }
  });
}

const ScenarioRegistration g_perf_lsp_semantic_tokens_decode({Scenario{
    .name = "lsp_semantic_tokens_decode",
    .smoke = true,
    .baseline_gated = true,
    .tolerance_p95_percent = tolerance::kJitterWallP95,
    .tolerance_max_percent = tolerance::kJitterWallMax,
    .run = RunLspSemanticTokensDecode,
}});
const ScenarioRegistration g_perf_lsp_publish_diagnostics_parse({Scenario{
    .name = "lsp_publish_diagnostics_parse",
    .smoke = true,
    .baseline_gated = true,
    .run = RunLspPublishDiagnosticsParse,
}});
const ScenarioRegistration g_perf_lsp_document_symbols_parse({Scenario{
    .name = "lsp_document_symbols_parse",
    .smoke = true,
    .baseline_gated = true,
    .run = RunLspDocumentSymbolsParse,
}});
const ScenarioRegistration g_perf_lsp_message_framing({Scenario{
    .name = "lsp_message_framing",
    .smoke = true,
    .baseline_gated = true,
    .run = RunJsonRpcMessageFraming,
}});

}
}
