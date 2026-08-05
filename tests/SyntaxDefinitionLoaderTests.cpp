#include "TestSupport.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#if MICROIDE_PERF_HARNESS_BUILD
#include "perf/AllocationCounter.h"
#endif

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

// The service's destructor must drain/join the worker before its result queues /
// wake callback are destroyed, so a queued job that reaches into `this` cannot
// outlive them even when an owner never calls Shutdown() (ASAN would flag a UAF).
void TestHighlightPrefetchServiceDestructorDrainsWithoutShutdown() {
  runtime_syntax::EnsureInitialized();
  const std::vector<std::string> lines = {"int main() { return 0; }"};
  const std::uint32_t definition_id =
      runtime_syntax::DetectState(std::filesystem::path("dtor.cpp"), lines).definition_id;
  {
    editor::HighlightPrefetchService service;
    service.PrewarmDefinition(definition_id);
    // Intentionally NO Shutdown(): the destructor at scope exit must join the
    // worker cleanly.
  }
  Expect(true, "destroying the service with queued work must not crash or leak the worker");
}

// Filetype detection reads a bounded HEAD of the buffer. The bound used to be
// stated only in lines ("at most 64 head lines"), which bounds nothing on a file
// that has no line breaks: a minified bundle or a newline-free JSON blob is ONE
// line of many megabytes, so the head copy was the whole document -- on the shell
// thread, once per keystroke, because DetectState runs from the highlight path.
//
// This pins the byte bound rather than a duration, so it cannot go flaky: under a
// perf-harness build the counting allocator says exactly how many bytes the call
// took, and 8 MB of line must not turn into 8 MB of head.
void TestDetectStateBoundsHeadCopyOnANewlineFreeBuffer() {
  runtime_syntax::EnsureInitialized();
  // One line, 8 MB, no newline anywhere -- the minified-bundle shape.
  const std::vector<std::string> huge_single_line = {
      std::string(8u * 1024u * 1024u, 'x')};

  // Detection still works: the extension decides, and it must keep deciding.
  const std::uint32_t expected =
      runtime_syntax::DetectState(std::filesystem::path("short.cpp"),
                                  std::vector<std::string>{"int main() { return 0; }"})
          .definition_id;
  const std::uint32_t huge =
      runtime_syntax::DetectState(std::filesystem::path("minified.cpp"), huge_single_line)
          .definition_id;
  Expect(huge == expected,
         "a newline-free buffer must still resolve the same definition as a short one");

#if MICROIDE_PERF_HARNESS_BUILD
  // Warm anything one-shot (lazy registry init, first-call regex state) so the
  // measured call below allocates only for the head it builds.
  (void)runtime_syntax::DetectState(std::filesystem::path("minified.cpp"), huge_single_line);
  const auto before = microide::tests::perf::Allocations::Snapshot();
  (void)runtime_syntax::DetectState(std::filesystem::path("minified.cpp"), huge_single_line);
  const auto delta = microide::tests::perf::Allocations::DeltaSince(before);
  // Generous: the cap is 4 KiB per head line and there is one line here. Anything
  // near the 8 MB line length means the head copied the buffer again.
  Expect(delta.bytes_allocated < 256 * 1024,
         "detecting a filetype on an 8 MB single-line buffer must not copy the buffer");
#endif
}

}  // namespace

void RegisterSyntaxDefinitionLoaderTests(std::vector<TestCase>& tests) {
  AddTest(tests, "SyntaxDefinitionLoader/DiscoverDedupesDirectoryContributedTwice",
          TestDiscoverDedupesDirectoryContributedTwice);
  AddTest(tests, "SyntaxDefinitionLoader/FingerprintTracksContentChanges",
          TestFingerprintTracksContentChanges);
  AddTest(tests, "SyntaxDefinitionLoader/CompileAndPrewarmDefinitionAreSafe",
          TestCompileAndPrewarmDefinitionAreSafe);
  AddTest(tests, "SyntaxDefinitionLoader/DetectStateBoundsHeadCopyOnANewlineFreeBuffer",
          TestDetectStateBoundsHeadCopyOnANewlineFreeBuffer);
  AddTest(tests, "SyntaxDefinitionLoader/HighlightPrefetchServiceDestructorDrainsWithoutShutdown",
          TestHighlightPrefetchServiceDestructorDrainsWithoutShutdown);
}

}  // namespace microide::tests
