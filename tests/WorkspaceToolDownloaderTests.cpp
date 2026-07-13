#include "TestSupport.h"

#include "workspace/WorkspaceToolDownloader.h"

#include <filesystem>
#include <string>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::ToolDownloader;

void TestToolDownloaderFallsBackToShasumWhenSha256sumMissing() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path bin_dir = temp_dir.path() / "bin";
  const std::filesystem::path cache_dir = temp_dir.path() / "cache";
  const std::filesystem::path source = temp_dir.path() / "tool.bin";
  std::filesystem::create_directories(bin_dir);
  WriteFile(source, "payload\n");

#if !defined(_WIN32)
  // Keep PATH scoped to this fixture so `sha256sum` is absent and fallback uses `shasum`.
  const std::filesystem::path shasum_path = bin_dir / "shasum";
  WriteFile(shasum_path,
            "#!/bin/sh\n"
            "echo \"d4e4877bac978b7952f0d544fc52ebff5411d351d129f1f056fa43f11da9af2b  $3\"\n");
  std::filesystem::permissions(shasum_path, std::filesystem::perms::owner_exec |
                                                std::filesystem::perms::owner_read |
                                                std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::add);
#endif

  ScopedEnvVar path_env("PATH", bin_dir.string());

  ToolDownloader downloader;
  downloader.SetCacheDir(cache_dir);
  const auto downloaded =
      downloader.Download("tool", source.string(),
                          "d4e4877bac978b7952f0d544fc52ebff5411d351d129f1f056fa43f11da9af2b");
  Expect(downloaded.has_value(),
         "tool download should succeed when sha256sum is missing but a fallback hash tool succeeds");
  Expect(downloaded == std::optional<std::filesystem::path>(cache_dir / "tool"),
         "successful download should return the cached tool path");
}

// Regression: the expected digest may be uppercase (manifests accept either case)
// while the computed digest is lowercase. Download must compare case-insensitively
// so a correct file with an uppercase expected sha still verifies.
void TestToolDownloaderAcceptsUppercaseExpectedSha() {
#if defined(_WIN32)
  return;
#else
  TemporaryDirectory temp_dir;
  const std::filesystem::path bin_dir = temp_dir.path() / "bin";
  const std::filesystem::path cache_dir = temp_dir.path() / "cache";
  const std::filesystem::path source = temp_dir.path() / "tool.bin";
  std::filesystem::create_directories(bin_dir);
  WriteFile(source, "payload\n");
  const std::filesystem::path shasum_path = bin_dir / "shasum";
  WriteFile(shasum_path,
            "#!/bin/sh\n"
            "echo \"d4e4877bac978b7952f0d544fc52ebff5411d351d129f1f056fa43f11da9af2b  $3\"\n");
  std::filesystem::permissions(shasum_path, std::filesystem::perms::owner_exec |
                                                std::filesystem::perms::owner_read |
                                                std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::add);
  ScopedEnvVar path_env("PATH", bin_dir.string());

  ToolDownloader downloader;
  downloader.SetCacheDir(cache_dir);
  // Same digest, uppercased. Must still verify against the lowercase computed hash.
  const auto downloaded = downloader.Download(
      "tool", source.string(), "D4E4877BAC978B7952F0D544FC52EBFF5411D351D129F1F056FA43F11DA9AF2B");
  Expect(downloaded.has_value(),
         "an uppercase expected sha256 must verify against the lowercase computed digest");
#endif
}

// A tool_id containing path-traversal (or an absolute-ish component) must be
// rejected so it can never read/write/probe outside the cache directory.
void TestToolDownloaderRejectsTraversalToolId() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path cache_dir = temp_dir.path() / "cache";
  // A file the traversal would target if the guard were missing.
  const std::filesystem::path outside = temp_dir.path() / "secret.bin";
  WriteFile(outside, "secret\n");

  ToolDownloader downloader;
  downloader.SetCacheDir(cache_dir);

  for (const char* evil : {"../secret.bin", "..", ".", "sub/evil", "a\\b"}) {
    Expect(!downloader.IsCached(evil), std::string("IsCached must reject unsafe id: ") + evil);
    Expect(!downloader.GetCachedTool(evil).has_value(),
           std::string("GetCachedTool must reject unsafe id: ") + evil);
    Expect(!downloader.Download(evil, outside.string(), std::string(64, 'a')).has_value(),
           std::string("Download must reject unsafe id: ") + evil);
  }

  // A well-formed id is still accepted by the cache lookups (absent -> not cached).
  Expect(!downloader.IsCached("valid-tool"), "a safe but absent id is simply not cached");
}

// An empty expected hash gives no integrity guarantee, so a cached copy must NOT be
// served blindly when there is no trusted local source to re-copy from. Regression for
// inventory J22.
void TestToolDownloaderEmptyHashDoesNotTrustCacheWithoutLocalSource() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path cache_dir = temp_dir.path() / "cache";
  std::filesystem::create_directories(cache_dir);
  // Simulate a previously cached (possibly stale/tampered) tool binary.
  WriteFile(cache_dir / "tool", "maybe-tampered\n");

  ToolDownloader downloader;
  downloader.SetCacheDir(cache_dir);

  // Empty hash + a non-local URL that the downloader cannot fetch: nothing to verify
  // against and no trusted local source, so the request must be rejected.
  Expect(!downloader.Download("tool", "https://example.invalid/tool.tgz", "").has_value(),
         "an empty sha256 must not blindly return a cached tool when no local source resolves");
}

// With an empty hash but an explicitly-trusted local source, the downloader re-copies
// from that source rather than trusting a stale cache. Regression for inventory J22.
void TestToolDownloaderEmptyHashReCopiesFromLocalSource() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path cache_dir = temp_dir.path() / "cache";
  const std::filesystem::path source = temp_dir.path() / "tool.bin";
  WriteFile(source, "fresh-bytes\n");
  std::filesystem::create_directories(cache_dir);
  WriteFile(cache_dir / "tool", "stale-bytes\n");  // pre-existing stale cache

  ToolDownloader downloader;
  downloader.SetCacheDir(cache_dir);
  const auto path = downloader.Download("tool", source.string(), "");
  Expect(path.has_value(),
         "empty hash with a trusted local source should re-copy and succeed");
  Expect(ReadFile(cache_dir / "tool") == "fresh-bytes\n",
         "empty-hash download must re-copy from the local source, not serve the stale cache");
}

}  // namespace

void RegisterWorkspaceToolDownloaderTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceToolDownloader/FallsBackToShasumWhenSha256sumMissing",
          TestToolDownloaderFallsBackToShasumWhenSha256sumMissing);
  AddTest(tests, "WorkspaceToolDownloader/AcceptsUppercaseExpectedSha",
          TestToolDownloaderAcceptsUppercaseExpectedSha);
  AddTest(tests, "WorkspaceToolDownloader/RejectsTraversalToolId",
          TestToolDownloaderRejectsTraversalToolId);
  AddTest(tests, "WorkspaceToolDownloader/EmptyHashDoesNotTrustCacheWithoutLocalSource",
          TestToolDownloaderEmptyHashDoesNotTrustCacheWithoutLocalSource);
  AddTest(tests, "WorkspaceToolDownloader/EmptyHashReCopiesFromLocalSource",
          TestToolDownloaderEmptyHashReCopiesFromLocalSource);
}

}  // namespace microide::tests
