// Microbench for the per-keystroke LSP `textDocument/didChange` serialization cost.
//
// Track A of the lsp-dedup-and-feature-wiring perf pass moved this work off the UI
// thread: LspClient::DidChange used to build the didChange JsonValue and run the
// Content-Length + JSON serialization synchronously on the calling (UI) thread every
// keystroke; it now defers both into the outbound builder that runs on the per-server
// I/O thread. This bench measures the magnitude of what moved — the JSON serialization
// of a whole-document (full-sync) didChange at a few document sizes — so the win is a
// committed, reproducible number rather than a claim. It intentionally uses only the
// public util::JsonValue / SerializeJson API (no subprocess, fully deterministic).
//
// Build:   cmake --build build --target microide_lsp_serialize_bench
// Run:     ./build/microide_lsp_serialize_bench [iterations]

#include "util/JsonValue.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using microide::util::JsonArray;
using microide::util::JsonObject;
using microide::util::JsonValue;
using microide::util::SerializeJson;

// A representative source document of roughly `line_count` lines of code-like text.
std::string MakeDocument(std::size_t line_count) {
  std::string text;
  text.reserve(line_count * 48);
  for (std::size_t i = 0; i < line_count; ++i) {
    text += "    const auto value_";
    text += std::to_string(i);
    text += " = compute(step, index_";
    text += std::to_string(i % 97);
    text += ");\n";
  }
  return text;
}

// Build the didChange notification JsonValue for a full-document sync, exactly as
// LspClient::DidChange does (uri + version + single full-text contentChange).
JsonValue MakeDidChange(const std::string& uri, int version, const std::string& text) {
  JsonObject text_doc;
  text_doc["uri"] = JsonValue(uri);
  text_doc["version"] = JsonValue(static_cast<std::int64_t>(version));
  JsonObject change;
  change["text"] = JsonValue(text);
  JsonArray changes;
  changes.push_back(JsonValue(std::move(change)));
  JsonObject params;
  params["textDocument"] = JsonValue(std::move(text_doc));
  params["contentChanges"] = JsonValue(std::move(changes));
  JsonObject msg;
  msg["jsonrpc"] = JsonValue("2.0");
  msg["method"] = JsonValue("textDocument/didChange");
  msg["params"] = JsonValue(std::move(params));
  return JsonValue(std::move(msg));
}

double MedianMicros(std::vector<double>& samples) {
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t iterations = 200;
  if (argc > 1) {
    iterations = static_cast<std::size_t>(std::strtoul(argv[1], nullptr, 10));
    if (iterations == 0) iterations = 200;
  }

  const std::string uri = "file:///home/user/project/src/module/widget.cpp";
  const std::size_t line_sizes[] = {200, 1000, 5000, 20000};

  std::printf("lsp didChange full-sync serialization (median of %zu iters)\n", iterations);
  std::printf("%8s  %10s  %14s  %14s\n", "lines", "bytes", "build_us", "serialize_us");

  std::uint64_t sink = 0;
  for (std::size_t lines : line_sizes) {
    const std::string doc = MakeDocument(lines);

    std::vector<double> build_samples;
    std::vector<double> serialize_samples;
    build_samples.reserve(iterations);
    serialize_samples.reserve(iterations);

    for (std::size_t it = 0; it < iterations; ++it) {
      // build: constructing the didChange JsonValue (the whole-document copy the
      // calling thread still pays once, into the deferred builder's capture).
      const auto build_start = Clock::now();
      JsonValue message = MakeDidChange(uri, static_cast<int>(it + 1), doc);
      const auto build_end = Clock::now();

      // serialize: JSON + Content-Length production — the work that moved OFF the UI
      // thread onto the per-server I/O thread.
      const auto serialize_start = Clock::now();
      const std::string wire = SerializeJson(message);
      const auto serialize_end = Clock::now();

      sink += wire.size();
      build_samples.push_back(
          std::chrono::duration<double, std::micro>(build_end - build_start).count());
      serialize_samples.push_back(
          std::chrono::duration<double, std::micro>(serialize_end - serialize_start).count());
    }

    std::printf("%8zu  %10zu  %14.2f  %14.2f\n", lines, doc.size(),
                MedianMicros(build_samples), MedianMicros(serialize_samples));
  }

  // Defeat dead-code elimination of the serialized payloads.
  if (sink == 0xFFFFFFFFFFFFFFFFULL) {
    std::fprintf(stderr, "unreachable\n");
  }
  return 0;
}
