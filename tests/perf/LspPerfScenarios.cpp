// Performance scenarios for the LSP subsystem.
//
// These bring the language-server client to perf-coverage parity with the
// debugger/DAP set (`DebugPerfScenarios.cpp`): before this file the LSP wire
// path had zero perf coverage even though it re-runs on every edit, save, and
// keystroke. Every scenario here is a pure-unit micro-benchmark — it constructs
// the real wire payload directly and measures the exact hot path the host
// consumes, ignoring the app driver — so they are deterministic and
// allocation-stable, which is why they are gated (`baseline_gated = true`) and
// run in the smoke crash-pass (`smoke = true`), unlike the advisory
// live-subprocess scenarios.
//
// The decode helpers live in `lsp_protocol` (one home for the JSON <-> wire
// mapping) and the framing codec is `LspMessageFramer`; both are hot (speed)
// and hostile-input (correctness) surfaces, which is exactly why they were
// extracted into pure value types with deterministic coverage.
#include "perf/PerfHarness.h"

#include "util/JsonValue.h"
#include "workspace/LspMessageFraming.h"
#include "workspace/LspProtocol.h"
#include "workspace/WorkspaceLspClient.h"

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
using microide::workspace::LspMessageFramer;

// ---- Synthetic wire-payload builders --------------------------------------

// A `textDocument/semanticTokens/full` result: `{data: [...]}` where `data` is a
// flat run of 5-int groups (deltaLine, deltaStartChar, length, tokenType,
// tokenModifiers) relative to the previous token. Mixes same-line and next-line
// deltas so both branches of the delta accumulator are exercised, matching a
// real highlighted buffer (several tokens per line). This is the payload the
// editor re-decodes on every semantic-tokens refresh of a large file.
JsonValue MakeSemanticTokensResult(int token_count) {
  JsonArray data;
  data.reserve(static_cast<std::size_t>(token_count) * 5);
  for (int i = 0; i < token_count; ++i) {
    const bool new_line = (i % 8) == 0;  // ~8 tokens per line
    const std::int64_t delta_line = new_line ? 1 : 0;
    const std::int64_t delta_start = new_line ? (i % 40) : (5 + (i % 10));
    data.push_back(JsonValue(delta_line));
    data.push_back(JsonValue(delta_start));
    data.push_back(JsonValue(static_cast<std::int64_t>(4 + (i % 8))));   // length
    data.push_back(JsonValue(static_cast<std::int64_t>(i % 15)));        // tokenType
    data.push_back(JsonValue(static_cast<std::int64_t>(0)));             // tokenModifiers
  }
  JsonObject result;
  result["data"] = JsonValue(std::move(data));
  return JsonValue(std::move(result));
}

// A `textDocument/publishDiagnostics` diagnostics array (the notification's
// `diagnostics` field). Each entry carries a range, message, severity, and code;
// codes alternate between the numeric and string wire shapes so both branches of
// ParseDiagnostic's code handling run. This is decoded on every publish.
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
      d["code"] = JsonValue(static_cast<std::int64_t>(2000 + i));  // integer code
    } else {
      d["code"] = JsonValue(std::string("no-unused-vars"));         // string code
    }
    diags.push_back(JsonValue(std::move(d)));
  }
  return JsonValue(std::move(diags));
}

// Build one DocumentSymbol node with `child_count` children (each built by
// `make_child`). Every node carries an explicit (possibly empty) children array
// so the recursive parser walks a well-formed tree.
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
  sym["range"] = range;                 // copy: selectionRange reuses it below
  sym["selectionRange"] = std::move(range);
  sym["children"] = JsonValue(std::move(children));
  return JsonValue(std::move(sym));
}

// A `textDocument/documentSymbol` DocumentSymbol[] outline: `classes` top-level
// nodes, each with `methods` method children, each with `fields` leaf children.
// This is the tree the outline/breadcrumb rebuilds from on every save.
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
            MakeSymbolNode("field" + std::to_string(f), /*kind=*/8, line++, JsonArray{}));
      }
      method_nodes.push_back(MakeSymbolNode("method" + std::to_string(m), /*kind=*/6, line++,
                                            std::move(field_nodes)));
    }
    top.push_back(MakeSymbolNode("Class" + std::to_string(c), /*kind=*/5, line++,
                                 std::move(method_nodes)));
  }
  return JsonValue(std::move(top));
}

// Concatenate `count` Content-Length-framed JSON-RPC notifications into one wire
// buffer, mirroring what a chatty server streams (publishDiagnostics + progress).
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

// ---- Pure-unit scenarios ---------------------------------------------------

// Decoding semanticTokens/full is the per-refresh cost for syntax highlighting:
// resolve a large delta-encoded run into absolute (line, start_char, length,
// type) tokens. This is re-run on every edit that invalidates tokens on a big
// file.
void RunLspSemanticTokensDecode(ScenarioContext& context) {
  const JsonValue result = MakeSemanticTokensResult(/*token_count=*/8000);
  context.Measure("lsp_semantic_tokens.decode", [&]() {
    for (int iter = 0; iter < 200; ++iter) {
      const auto tokens = codec::ParseSemanticTokensData(result);
      volatile std::size_t sink = tokens.size();
      (void)sink;
    }
  });
}

// Parsing a publishDiagnostics array is the per-publish cost: re-materialize the
// full diagnostic list (ranges + messages + codes) the gutter/squiggle overlay
// consumes. Servers republish the whole file's diagnostics on every change.
void RunLspPublishDiagnosticsParse(ScenarioContext& context) {
  const JsonValue diagnostics = MakeDiagnosticsArray(/*count=*/1000);
  context.Measure("lsp_diagnostics.parse", [&]() {
    for (int iter = 0; iter < 200; ++iter) {
      const auto parsed = codec::ParseDiagnostics(diagnostics);
      volatile std::size_t sink = parsed.size();
      (void)sink;
    }
  });
}

// Parsing a DocumentSymbol[] outline is the per-save cost for the outline and
// breadcrumb surfaces: walk the recursive class/method/field tree, bounded by
// the depth + total-node budget. Exercises the recursion the flat editor
// scenarios never touch.
void RunLspDocumentSymbolsParse(ScenarioContext& context) {
  const JsonValue symbols =
      MakeDocumentSymbolsResult(/*classes=*/30, /*methods=*/15, /*fields=*/3);
  context.Measure("lsp_document_symbols.parse", [&]() {
    for (int iter = 0; iter < 200; ++iter) {
      const auto parsed = codec::ParseDocumentSymbols(symbols);
      volatile std::size_t sink = parsed.size();
      (void)sink;
    }
  });
}

// Framing throughput: feed a chatty server's Content-Length-delimited stream to
// the incremental LspMessageFramer in realistic partial chunks so the
// cross-chunk partial-frame state is exercised, and drain every complete
// message. This is the transport hot path on the I/O thread and the resync
// surface that must never desync on a boundary split.
void RunLspMessageFraming(ScenarioContext& context) {
  const std::string stream = MakeFramedStream(/*count=*/500);
  constexpr std::size_t kChunk = 1500;  // TCP-segment-ish partial delivery
  context.Measure("lsp_message_framing.parse", [&]() {
    for (int iter = 0; iter < 40; ++iter) {
      LspMessageFramer framer;
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

// ---- Registration ----------------------------------------------------------

const ScenarioRegistration g_perf_lsp_semantic_tokens_decode({Scenario{
    .name = "lsp_semantic_tokens_decode",
    .smoke = true,
    .baseline_gated = true,
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
    .run = RunLspMessageFraming,
}});

}  // namespace
}  // namespace microide::tests::perf
