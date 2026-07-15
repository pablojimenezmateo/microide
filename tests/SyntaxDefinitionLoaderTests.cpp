#include "TestSupport.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "editor/HighlightPrefetchService.h"
#include "editor/RuntimeSyntaxRegistry.h"
#include "editor/SyntaxDefinitionLoader.h"

// Coverage for the syntax-reload loader speed wins:
//  - DiscoverDefinitionFiles dedup (a directory contributed twice loads each
//    file once) — observed through SyntaxSourceFingerprint, which folds one
//    entry per discovered file.
//  - SyntaxSourceFingerprint content-based change detection (fingerprint tracks
//    file bytes; unchanged files reuse a cached hash without re-reading).
//  - CompileDefinition / HighlightPrefetchService::PrewarmDefinition safety.

namespace microide::tests {
namespace {

namespace runtime_syntax = microide::editor::runtime_syntax;

void TestDiscoverDedupesDirectoryContributedTwice() {
  TemporaryDirectory dir;
  WriteFile(dir.path() / "a.lua", "return {}");
  WriteFile(dir.path() / "b.lua", "return {} -- second");

  runtime_syntax::SyntaxSourceFingerprint single;
  runtime_syntax::SyntaxSourceFingerprint doubled;
  const std::uint64_t once = single.Compute({dir.path()});
  // The same directory listed twice must not fold each file's hash twice: with
  // dedup the fingerprint is identical to listing it once.
  const std::uint64_t twice = doubled.Compute({dir.path(), dir.path()});
  Expect(once == twice,
         "duplicate syntax directory must produce an identical fingerprint (files deduped)");
}

void TestFingerprintTracksContentChanges() {
  TemporaryDirectory dir;
  const std::filesystem::path file = dir.path() / "grammar.lua";
  WriteFile(file, "return {}");

  runtime_syntax::SyntaxSourceFingerprint fingerprint;
  const std::uint64_t before = fingerprint.Compute({dir.path()});

  // Recompute with no change: identical fingerprint (and, internally, the cached
  // content hash is reused instead of re-reading the file).
  const std::uint64_t unchanged = fingerprint.Compute({dir.path()});
  Expect(before == unchanged, "unchanged sources must yield an identical fingerprint");

  // Edit the file to different bytes of a different length. A size change forces
  // a cache miss + re-read even if the coarse mtime clock has not ticked, so the
  // content edit is always detected.
  WriteFile(file, "return {} -- edited body, deliberately longer than before");
  const std::uint64_t after = fingerprint.Compute({dir.path()});
#if MICROIDE_HAS_LUA_PLUGINS
  Expect(before != after, "a content edit must change the fingerprint");
#else
  // Without Lua plugins the fingerprint is a constant 0 by contract.
  Expect(after == 0, "fingerprint is 0 when Lua plugins are disabled");
#endif
}

void TestCompileAndPrewarmDefinitionAreSafe() {
  // No-op ids must not crash: 0, and an id far past the registry.
  runtime_syntax::CompileDefinition(0);
  runtime_syntax::CompileDefinition(9'999'999);

  // Resolve a real definition id off a detectable filetype and compile it twice
  // (idempotent via std::call_once).
  runtime_syntax::EnsureInitialized();
  const std::vector<std::string> lines = {"#include <cstdio>", "int main() { return 0; }"};
  const std::uint32_t definition_id =
      runtime_syntax::DetectState(std::filesystem::path("prewarm.cpp"), lines).definition_id;
  runtime_syntax::CompileDefinition(definition_id);
  runtime_syntax::CompileDefinition(definition_id);

  // PrewarmDefinition posts the compile to the worker; Shutdown drains/joins it.
  editor::HighlightPrefetchService service;
  service.PrewarmDefinition(definition_id);
  service.PrewarmDefinition(0);
  service.Shutdown();
  Expect(true, "CompileDefinition/PrewarmDefinition complete without crashing");
}

}  // namespace

void RegisterSyntaxDefinitionLoaderTests(std::vector<TestCase>& tests) {
  AddTest(tests, "SyntaxDefinitionLoader/DiscoverDedupesDirectoryContributedTwice",
          TestDiscoverDedupesDirectoryContributedTwice);
  AddTest(tests, "SyntaxDefinitionLoader/FingerprintTracksContentChanges",
          TestFingerprintTracksContentChanges);
  AddTest(tests, "SyntaxDefinitionLoader/CompileAndPrewarmDefinitionAreSafe",
          TestCompileAndPrewarmDefinitionAreSafe);
}

}  // namespace microide::tests
