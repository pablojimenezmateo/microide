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

  const std::uint64_t unchanged = fingerprint.Compute({dir.path()});
  Expect(before == unchanged, "unchanged sources must yield an identical fingerprint");

  WriteFile(file, "return {} -- edited body, deliberately longer than before");
  const std::uint64_t after = fingerprint.Compute({dir.path()});
#if MICROIDE_HAS_LUA_PLUGINS
  Expect(before != after, "a content edit must change the fingerprint");
#else
  Expect(after == 0, "fingerprint is 0 when Lua plugins are disabled");
#endif
}

void TestCompileAndPrewarmDefinitionAreSafe() {
  runtime_syntax::CompileDefinition(0);
  runtime_syntax::CompileDefinition(9'999'999);

  runtime_syntax::EnsureInitialized();
  const std::vector<std::string> lines = {"#include <cstdio>", "int main() { return 0; }"};
  const std::uint32_t definition_id =
      runtime_syntax::DetectState(std::filesystem::path("prewarm.cpp"), lines).definition_id;
  runtime_syntax::CompileDefinition(definition_id);
  runtime_syntax::CompileDefinition(definition_id);

  editor::HighlightPrefetchService service;
  service.PrewarmDefinition(definition_id);
  service.PrewarmDefinition(0);
  service.Shutdown();
  Expect(true, "CompileDefinition/PrewarmDefinition complete without crashing");
}

void TestHighlightPrefetchServiceDestructorDrainsWithoutShutdown() {
  runtime_syntax::EnsureInitialized();
  const std::vector<std::string> lines = {"int main() { return 0; }"};
  const std::uint32_t definition_id =
      runtime_syntax::DetectState(std::filesystem::path("dtor.cpp"), lines).definition_id;
  {
    editor::HighlightPrefetchService service;
    service.PrewarmDefinition(definition_id);
  }
  Expect(true, "destroying the service with queued work must not crash or leak the worker");
}

void TestDetectStateBoundsHeadCopyOnANewlineFreeBuffer() {
  runtime_syntax::EnsureInitialized();
  const std::vector<std::string> huge_single_line = {
      std::string(8u * 1024u * 1024u, 'x')};

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
  (void)runtime_syntax::DetectState(std::filesystem::path("minified.cpp"), huge_single_line);
  const auto before = microide::tests::perf::Allocations::Snapshot();
  (void)runtime_syntax::DetectState(std::filesystem::path("minified.cpp"), huge_single_line);
  const auto delta = microide::tests::perf::Allocations::DeltaSince(before);
  Expect(delta.bytes_allocated < 256 * 1024,
         "detecting a filetype on an 8 MB single-line buffer must not copy the buffer");
#endif
}

}

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

}
