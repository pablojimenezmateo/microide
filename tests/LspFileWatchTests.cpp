#include "TestSupport.h"

#include "util/JsonValue.h"
#include "workspace/FileUri.h"
#include "workspace/LspFileWatchRegistry.h"
#include "workspace/WorkspaceLspClient.h"
#include "workspace/WorkspaceLspManager.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::util::JsonArray;
using microide::util::JsonObject;
using microide::util::JsonValue;
using microide::workspace::LspClient;
using microide::workspace::LspFileChangeType;
using microide::workspace::LspFileWatchRegistry;

// Build the `registrations[]` entry a server sends in client/registerCapability
// for workspace/didChangeWatchedFiles.
JsonValue MakeWatcherRegistration(const std::string& id,
                                  const std::vector<std::string>& patterns,
                                  int kind = microide::workspace::kLspWatchKindAll) {
  JsonArray watchers;
  for (const std::string& pattern : patterns) {
    JsonObject watcher;
    watcher["globPattern"] = JsonValue(pattern);
    watcher["kind"] = JsonValue(static_cast<std::int64_t>(kind));
    watchers.push_back(JsonValue(std::move(watcher)));
  }
  JsonObject options;
  options["watchers"] = JsonValue(std::move(watchers));
  JsonObject registration;
  registration["id"] = JsonValue(id);
  registration["method"] = JsonValue("workspace/didChangeWatchedFiles");
  registration["registerOptions"] = JsonValue(std::move(options));
  return JsonValue(std::move(registration));
}

void TestRegistryMatchesRelativeGlobs() {
  LspFileWatchRegistry registry;
  Expect(registry.empty(), "a fresh registry holds no watchers");

  Expect(registry.Register(MakeWatcherRegistration("rust", {"**/*.rs", "**/Cargo.toml"}),
                           "/home/u/proj"),
         "a well-formed registration should be accepted");
  Expect(registry.size() == 1, "one registration entry should be stored");

  Expect(registry.WantsChange("src/main.rs", "/home/u/proj/src/main.rs",
                              LspFileChangeType::Changed),
         "**/*.rs must match a nested source file");
  Expect(registry.WantsChange("Cargo.toml", "/home/u/proj/Cargo.toml",
                              LspFileChangeType::Created),
         "**/Cargo.toml must match at the project root");
  Expect(!registry.WantsChange("README.md", "/home/u/proj/README.md",
                               LspFileChangeType::Changed),
         "an unregistered extension must not match");
}

void TestRegistryExpandsBraceAlternation() {
  // GlobMatches has no brace support of its own, so a registration carrying
  // "{c,cpp,h}" only works because the registry expands it first. This is the
  // shared expander the search scope box uses.
  LspFileWatchRegistry registry;
  Expect(registry.Register(MakeWatcherRegistration("clangd", {"**/*.{c,cpp,h,hpp}"}),
                           "/home/u/proj"),
         "a brace-alternation registration should be accepted");
  Expect(registry.PatternCountForTesting() == 4,
         "{c,cpp,h,hpp} must expand to four concrete patterns");

  for (const char* path : {"src/a.c", "src/a.cpp", "include/a.h", "include/a.hpp"}) {
    Expect(registry.WantsChange(path, std::string("/home/u/proj/") + path,
                                LspFileChangeType::Changed),
           "every brace alternative must match");
  }
  Expect(!registry.WantsChange("src/a.rs", "/home/u/proj/src/a.rs",
                               LspFileChangeType::Changed),
         "a non-listed extension must not match");
}

void TestRegistryHonorsWatchKindMask() {
  LspFileWatchRegistry registry;
  // kind=1 is Create only.
  Expect(registry.Register(
             MakeWatcherRegistration("creates", {"**/*.go"},
                                     microide::workspace::kLspWatchKindCreate),
             "/home/u/proj"),
         "a create-only registration should be accepted");

  Expect(registry.WantsChange("main.go", "/home/u/proj/main.go", LspFileChangeType::Created),
         "a create-only watcher must want creations");
  Expect(!registry.WantsChange("main.go", "/home/u/proj/main.go", LspFileChangeType::Changed),
         "a create-only watcher must not want modifications");
  Expect(!registry.WantsChange("main.go", "/home/u/proj/main.go", LspFileChangeType::Deleted),
         "a create-only watcher must not want deletions");
}

void TestRegistryResolvesRelativePatternInsideProject() {
  LspFileWatchRegistry registry;
  JsonObject glob;
  glob["baseUri"] = JsonValue(microide::workspace::FileUriForPath("/home/u/proj/crates/core"));
  glob["pattern"] = JsonValue("**/*.rs");
  JsonObject watcher;
  watcher["globPattern"] = JsonValue(std::move(glob));
  JsonArray watchers;
  watchers.push_back(JsonValue(std::move(watcher)));
  JsonObject options;
  options["watchers"] = JsonValue(std::move(watchers));
  JsonObject registration;
  registration["id"] = JsonValue("scoped");
  registration["method"] = JsonValue("workspace/didChangeWatchedFiles");
  registration["registerOptions"] = JsonValue(std::move(options));

  Expect(registry.Register(JsonValue(std::move(registration)), "/home/u/proj"),
         "a RelativePattern registration should be accepted");

  Expect(registry.WantsChange("crates/core/src/lib.rs", "/home/u/proj/crates/core/src/lib.rs",
                              LspFileChangeType::Changed),
         "a RelativePattern must match inside its base directory");
  Expect(!registry.WantsChange("crates/other/src/lib.rs", "/home/u/proj/crates/other/src/lib.rs",
                               LspFileChangeType::Changed),
         "a RelativePattern must not match outside its base directory");
}

void TestRegistryUnregisterAndReregister() {
  LspFileWatchRegistry registry;
  registry.Register(MakeWatcherRegistration("id-1", {"**/*.rs"}), "/home/u/proj");
  Expect(registry.WantsChange("a.rs", "/home/u/proj/a.rs", LspFileChangeType::Changed),
         "the initial registration should match");

  // Re-registering the same id supersedes rather than duplicating.
  registry.Register(MakeWatcherRegistration("id-1", {"**/*.toml"}), "/home/u/proj");
  Expect(registry.size() == 1, "re-registering an id must replace, not duplicate");
  Expect(!registry.WantsChange("a.rs", "/home/u/proj/a.rs", LspFileChangeType::Changed),
         "the superseded pattern must stop matching");
  Expect(registry.WantsChange("a.toml", "/home/u/proj/a.toml", LspFileChangeType::Changed),
         "the replacement pattern must match");

  Expect(registry.Unregister("id-1"), "unregistering a known id should report success");
  Expect(registry.empty(), "the registry should be empty after unregistering its only entry");
  Expect(!registry.Unregister("id-1"), "unregistering an unknown id should report no-op");
}

void TestRegistryRejectsMalformedRegistrations() {
  LspFileWatchRegistry registry;
  Expect(!registry.Register(JsonValue(JsonObject{}), "/home/u/proj"),
         "a registration with no registerOptions must be rejected");

  JsonObject empty_options;
  empty_options["watchers"] = JsonValue(JsonArray{});
  JsonObject registration;
  registration["registerOptions"] = JsonValue(std::move(empty_options));
  Expect(!registry.Register(JsonValue(std::move(registration)), "/home/u/proj"),
         "a registration with an empty watcher list must be rejected");
  Expect(registry.empty(), "a rejected registration must leave the registry untouched");
}

void TestRegistryBoundsPatternExplosion() {
  // A pathological brace product must not become an unbounded per-file match loop.
  LspFileWatchRegistry registry;
  Expect(registry.Register(
             MakeWatcherRegistration(
                 "greedy", {"**/{a,b,c,d}/{e,f,g,h}/{i,j,k,l}/{m,n,o,p}/{q,r,s,t}/*.x"}),
             "/home/u/proj"),
         "a large brace product should still register");
  Expect(registry.PatternCountForTesting() <=
             microide::workspace::kMaxLspFileWatchPatternsPerRegistration,
         "the expanded pattern count must be capped");
}

// The whole point of the feature: a server that registered watchers is told about
// on-disk changes it did not make, and a server that registered none is not.
void TestManagerNotifiesOnlyServersThatRegisteredWatchers() {
  microide::workspace::LspManager manager;

  auto watching = std::make_unique<LspClient>();
  LspClient* const watching_raw = watching.get();
  watching->EnableTestStubMode();
  watching->RegisterFileWatchersForTesting(MakeWatcherRegistration("rust", {"**/*.rs"}),
                                           "/home/u/proj");
  manager.InstallTestClientForTesting(std::vector<std::string>{"rust"}, std::move(watching));

  auto silent = std::make_unique<LspClient>();
  LspClient* const silent_raw = silent.get();
  silent->EnableTestStubMode();
  manager.InstallTestClientForTesting(std::vector<std::string>{"python"}, std::move(silent));

  Expect(watching_raw->WantsWatchedFiles(),
         "a client with a registration must report that it wants watched files");
  Expect(!silent_raw->WantsWatchedFiles(),
         "a client with no registration must report that it wants nothing");

  std::vector<microide::workspace::LspManager::WatchedFileChange> changes;
  changes.push_back({.relative_path = "src/main.rs",
                     .absolute_path = "/home/u/proj/src/main.rs",
                     .uri = microide::workspace::FileUriForPath("/home/u/proj/src/main.rs"),
                     .type = LspFileChangeType::Changed});
  changes.push_back({.relative_path = "docs/readme.md",
                     .absolute_path = "/home/u/proj/docs/readme.md",
                     .uri = microide::workspace::FileUriForPath("/home/u/proj/docs/readme.md"),
                     .type = LspFileChangeType::Created});

  Expect(manager.NotifyWatchedFileChanges(changes) == 1,
         "exactly the one server whose globs matched should be notified");

  // A batch that matches nobody notifies nobody, without touching either client.
  std::vector<microide::workspace::LspManager::WatchedFileChange> unmatched;
  unmatched.push_back({.relative_path = "docs/readme.md",
                       .absolute_path = "/home/u/proj/docs/readme.md",
                       .uri = microide::workspace::FileUriForPath("/home/u/proj/docs/readme.md"),
                       .type = LspFileChangeType::Changed});
  Expect(manager.NotifyWatchedFileChanges(unmatched) == 0,
         "a batch matching no registered glob must notify no server");

  Expect(manager.NotifyWatchedFileChanges({}) == 0, "an empty batch must be a no-op");

  manager.ShutdownAll();
}

}  // namespace

void RegisterLspFileWatchTests(std::vector<TestCase>& tests) {
  AddTest(tests, "LspFileWatch/RegistryMatchesRelativeGlobs", TestRegistryMatchesRelativeGlobs);
  AddTest(tests, "LspFileWatch/RegistryExpandsBraceAlternation",
          TestRegistryExpandsBraceAlternation);
  AddTest(tests, "LspFileWatch/RegistryHonorsWatchKindMask", TestRegistryHonorsWatchKindMask);
  AddTest(tests, "LspFileWatch/RegistryResolvesRelativePatternInsideProject",
          TestRegistryResolvesRelativePatternInsideProject);
  AddTest(tests, "LspFileWatch/RegistryUnregisterAndReregister",
          TestRegistryUnregisterAndReregister);
  AddTest(tests, "LspFileWatch/RegistryRejectsMalformedRegistrations",
          TestRegistryRejectsMalformedRegistrations);
  AddTest(tests, "LspFileWatch/RegistryBoundsPatternExplosion",
          TestRegistryBoundsPatternExplosion);
  AddTest(tests, "LspFileWatch/ManagerNotifiesOnlyServersThatRegisteredWatchers",
          TestManagerNotifiesOnlyServersThatRegisteredWatchers);
}

}  // namespace microide::tests
