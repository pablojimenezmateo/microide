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

void TestToolDownloaderFileUriPercentDecodesLocalHost() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path cache_dir = temp_dir.path() / "cache";
  const std::filesystem::path source = temp_dir.path() / "my tool.bin";  // space in name
  WriteFile(source, "uri-bytes\n");

  ToolDownloader downloader;
  downloader.SetCacheDir(cache_dir);
  // file://<empty host>/<percent-encoded path> resolves to the real local file.
  const std::string encoded = "file://" + (temp_dir.path() / "my%20tool.bin").generic_string();
  const auto path = downloader.Download("tool", encoded, "");
  Expect(path.has_value(), "a percent-encoded file:// URL resolves to the local file");
  Expect(ReadFile(cache_dir / "tool") == "uri-bytes\n", "the decoded local source is copied");
}

void TestToolDownloaderRejectsRemoteFileUri() {
  TemporaryDirectory temp_dir;
  ToolDownloader downloader;
  downloader.SetCacheDir(temp_dir.path() / "cache");
  // file://remote/... is NOT this machine and must not be read as the relative
  // path "remote/...".
  Expect(!downloader.Download("tool", "file://remote/etc/passwd", "").has_value(),
         "a non-local file:// host is rejected");
}

void TestToolDownloaderHashMismatchLeavesNoPartialFile() {
#if defined(_WIN32)
  return;
#else
  TemporaryDirectory temp_dir;
  const std::filesystem::path cache_dir = temp_dir.path() / "cache";
  const std::filesystem::path source = temp_dir.path() / "tool.bin";
  WriteFile(source, "payload\n");

  ToolDownloader downloader;
  downloader.SetCacheDir(cache_dir);
  // A wrong expected hash must fail AND leave neither the published cache entry
  // nor the temp ".partial" sibling behind (atomic temp-then-verify-then-rename).
  const auto path = downloader.Download("tool", source.string(), std::string(64, 'b'));
  Expect(!path.has_value(), "a hash mismatch fails the download");
  Expect(!std::filesystem::exists(cache_dir / "tool"),
         "no partial/unverified executable is published at the cache path");
  Expect(!std::filesystem::exists(cache_dir / "tool.partial"),
         "the temporary partial file is cleaned up on mismatch");
#endif
}

// Regression: GetCachedTool must verify the cached file's digest before handing
// it to a launcher when an expected sha is supplied — a mismatch (stale/tampered
// cache) returns nothing; an empty expected digest returns the path unverified.
void TestToolDownloaderGetCachedToolVerifiesHash() {
#if defined(_WIN32)
  return;
#else
  TemporaryDirectory temp_dir;
  const std::filesystem::path bin_dir = temp_dir.path() / "bin";
  const std::filesystem::path cache_dir = temp_dir.path() / "cache";
  std::filesystem::create_directories(bin_dir);
  std::filesystem::create_directories(cache_dir);
  WriteFile(cache_dir / "tool", "cached-bytes\n");

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

  Expect(downloader
             .GetCachedTool("tool",
                            "d4e4877bac978b7952f0d544fc52ebff5411d351d129f1f056fa43f11da9af2b")
             .has_value(),
         "GetCachedTool returns the path when the expected digest matches");
  Expect(!downloader.GetCachedTool("tool", std::string(64, 'f')).has_value(),
         "GetCachedTool rejects a cached file whose digest does not match");
  Expect(downloader.GetCachedTool("tool").has_value(),
         "GetCachedTool returns the path unverified when no expected digest is given");
#endif
}

// Regression: there is NO networking. Any remote scheme is rejected regardless of
// the expected hash — the downloader only resolves local file:// URIs and paths.
void TestToolDownloaderRejectsRemoteSchemesNoNetworking() {
  TemporaryDirectory temp_dir;
  ToolDownloader downloader;
  downloader.SetCacheDir(temp_dir.path() / "cache");
  Expect(!downloader.Download("tool", "https://example.invalid/tool.tgz", std::string(64, 'a'))
              .has_value(),
         "an https URL is rejected: there is no networking");
  Expect(!downloader.Download("tool", "http://example.invalid/tool", "").has_value(),
         "an http URL is rejected: there is no networking");
  Expect(!downloader.Download("tool", "ftp://example.invalid/tool", "").has_value(),
         "an ftp URL is rejected: there is no networking");
}

}  // namespace

void RegisterWorkspaceToolDownloaderTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceToolDownloader/GetCachedToolVerifiesHash",
          TestToolDownloaderGetCachedToolVerifiesHash);
  AddTest(tests, "WorkspaceToolDownloader/RejectsRemoteSchemesNoNetworking",
          TestToolDownloaderRejectsRemoteSchemesNoNetworking);
  AddTest(tests, "WorkspaceToolDownloader/FileUriPercentDecodesLocalHost",
          TestToolDownloaderFileUriPercentDecodesLocalHost);
  AddTest(tests, "WorkspaceToolDownloader/RejectsRemoteFileUri",
          TestToolDownloaderRejectsRemoteFileUri);
  AddTest(tests, "WorkspaceToolDownloader/HashMismatchLeavesNoPartialFile",
          TestToolDownloaderHashMismatchLeavesNoPartialFile);
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
