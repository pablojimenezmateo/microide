// End-to-end LSP coverage against a REAL language server (clangd), complementing
// the in-tree Python fake-server protocol tests. This closes the gap flagged in
// dev-docs/project/active-work.md: all prior end-to-end coverage drove a fake
// server, so production-server quirks (real capability/encoding negotiation,
// indexing progress, diagnostics for genuine compiler errors, hover/definition/
// completion/rename payloads) were never exercised.
//
// The suite is OPT-IN by availability: if no clangd binary is on PATH (or named by
// $MICROIDE_TEST_LSP_CLANGD) the test logs a skip and passes, so CI stays green on
// machines without a language server installed. Set MICROIDE_TEST_LSP_CLANGD to a
// clangd path to force a specific binary.
#include "TestSupport.h"

#include "util/JsonValue.h"
#include "workspace/FileUri.h"
#include "workspace/WorkspaceLspClient.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::LspClient;

// Locate a clangd binary: an explicit $MICROIDE_TEST_LSP_CLANGD override first,
// then a PATH scan. Empty string means "not available -> skip".
std::string LocateClangd() {
  if (const char* override_path = std::getenv("MICROIDE_TEST_LSP_CLANGD");
      override_path != nullptr && override_path[0] != '\0') {
    std::error_code ec;
    if (std::filesystem::exists(override_path, ec)) {
      return override_path;
    }
    return {};
  }
  const char* path_env = std::getenv("PATH");
  if (path_env == nullptr) {
    return {};
  }
  const std::string path = path_env;
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t colon = path.find(':', start);
    const std::string dir =
        path.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
    if (!dir.empty()) {
      std::error_code ec;
      const std::filesystem::path candidate = std::filesystem::path(dir) / "clangd";
      if (std::filesystem::exists(candidate, ec) && !std::filesystem::is_directory(candidate, ec)) {
        return candidate.string();
      }
    }
    if (colon == std::string::npos) {
      break;
    }
    start = colon + 1;
  }
  return {};
}

// Drain main-thread callbacks until `ready` returns true or the deadline passes.
template <typename Predicate>
bool PumpUntil(LspClient& client, Predicate&& ready, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    client.DrainCallbacks();
    if (ready()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  client.DrainCallbacks();
  return ready();
}

void TestLspRealServerClangdDrivesFullFeatureSet() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#else
  const std::string clangd = LocateClangd();
  if (clangd.empty()) {
    std::fprintf(stderr,
                 "[lsp-e2e] SKIP: no clangd on PATH (set MICROIDE_TEST_LSP_CLANGD to enable the "
                 "real-server end-to-end suite)\n");
    return;
  }

  TemporaryDirectory temp_dir;
  const std::filesystem::path root = temp_dir.path();
  const std::filesystem::path source = root / "sample.cpp";
  // A deliberate syntax error (missing ';') guarantees clangd publishes at least
  // one diagnostic even without a compile_commands.json.
  WriteFile(source,
            "int add(int a, int b) { return a + b }\n"
            "int main() { return add(1, 2); }\n");
  const std::string root_uri = workspace::FileUriForPath(root);
  const std::string uri = workspace::FileUriForPath(source);

  LspClient client;
  const bool started =
      client.Start({clangd, "--log=error", "--background-index=false"}, root_uri, "cpp",
                   root.string());
  Expect(started, "clangd should start");

  // 1. Initialize handshake + capability/encoding negotiation.
  const bool initialized = PumpUntil(
      client, [&] { return client.IsInitialized(); }, 15000);
  Expect(initialized, "clangd should complete the initialize handshake");
  const std::string encoding = client.ServerPositionEncoding();
  Expect(encoding == "utf-8" || encoding == "utf-16",
         "clangd should negotiate a supported position encoding (utf-8 preferred)");

  // 2. didOpen -> publishDiagnostics for the genuine compiler error.
  bool got_diagnostics = false;
  client.SetDiagnosticsCallback(
      [&](std::string diag_uri, std::vector<LspClient::Diagnostic> diags) {
        if (diag_uri == uri && !diags.empty()) {
          got_diagnostics = true;
        }
      });
  client.DidOpen(uri, "cpp", ReadFile(source));
  Expect(PumpUntil(client, [&] { return got_diagnostics; }, 15000),
         "clangd should publish diagnostics for the deliberate syntax error");

  // The `add` call sits at columns 20-22 of line 1 (0-based):
  //   "int main() { return add(1, 2); }"
  //    0123456789...          ^add starts at 20
  constexpr LspClient::Position kAddCall{1, 21};  // inside `add`

  // 3. Hover over the `add` call. Content varies by clangd version, so assert the
  //    callback fired with a hover payload, not its exact text.
  bool hover_done = false;
  bool hover_present = false;
  client.RequestHoverAsync(uri, kAddCall, [&](std::optional<util::JsonValue> hover) {
    hover_done = true;
    hover_present = hover.has_value() && !hover->IsNull();
  });
  Expect(PumpUntil(client, [&] { return hover_done; }, 10000),
         "clangd hover request should deliver a response");
  Expect(hover_present, "hovering the `add` call should return hover contents");

  // 4. Go-to-definition on the same `add` call -> the definition on line 0.
  bool definition_done = false;
  std::size_t definition_count = 0;
  client.RequestGoToDefinitionAsync(uri, kAddCall,
                                    [&](std::optional<std::vector<LspClient::Location>> locs) {
                                      definition_done = true;
                                      definition_count = locs.has_value() ? locs->size() : 0;
                                    });
  Expect(PumpUntil(client, [&] { return definition_done; }, 10000),
         "clangd definition request should deliver a response");
  Expect(definition_count >= 1, "go-to-definition on `add` should resolve to its declaration");

  // 5. Completion inside the `add` identifier — assert the request round-trips with
  //    items (clangd offers `add` among the candidates).
  bool completion_done = false;
  std::size_t completion_count = 0;
  client.RequestCompletionAsync(uri, LspClient::Position{1, 22},
                                [&](std::optional<std::vector<LspClient::CompletionItem>> items) {
                                  completion_done = true;
                                  completion_count = items.has_value() ? items->size() : 0;
                                });
  Expect(PumpUntil(client, [&] { return completion_done; }, 10000),
         "clangd completion request should deliver a response");
  Expect(completion_count >= 1, "completion inside main() should return at least one candidate");

  // 6. Rename `add` -> `sum`: the WorkspaceEdit must touch this file (declaration +
  //    call site), proving cross-reference rename works against a real server.
  bool rename_done = false;
  std::size_t rename_edit_count = 0;
  client.RequestRenameAsync(uri, LspClient::Position{0, 4}, "sum",
                            [&](std::optional<LspClient::WorkspaceEdit> edit) {
                              rename_done = true;
                              if (edit.has_value()) {
                                const auto it = edit->changes.find(uri);
                                if (it != edit->changes.end()) {
                                  rename_edit_count = it->second.size();
                                }
                              }
                            });
  Expect(PumpUntil(client, [&] { return rename_done; }, 10000),
         "clangd rename request should deliver a response");
  Expect(rename_edit_count >= 2,
         "renaming `add` should edit both its declaration and its call site");

  client.Shutdown();
#endif
}

}  // namespace

void RegisterLspRealServerE2ETests(std::vector<TestCase>& tests) {
  AddTest(tests, "LspRealServer/ClangdDrivesFullFeatureSet",
          TestLspRealServerClangdDrivesFullFeatureSet);
}

}  // namespace microide::tests
