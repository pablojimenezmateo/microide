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
  const auto downloaded =
      downloader.Download("tool", source.string(),
                          "d4e4877bac978b7952f0d544fc52ebff5411d351d129f1f056fa43f11da9af2b");
  Expect(downloaded.has_value(),
         "tool download should succeed when sha256sum is missing but a fallback hash tool succeeds");
  Expect(downloaded == std::optional<std::filesystem::path>(cache_dir / "tool"),
         "successful download should return the cached tool path");
}

void TestToolDownloaderWithoutUserCacheDirectoryRefuses() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path source = temp_dir.path() / "tool.bin";
  WriteFile(source, "payload\n");
  ScopedEnvVar home("HOME", "");
  ScopedEnvVar xdg_cache("XDG_CACHE_HOME", "");

  ToolDownloader downloader;
  Expect(!downloader.Download("tool", source.string(), "").has_value(),
         "a downloader with no cache directory must refuse to cache");
  Expect(!downloader.IsCached("tool"), "a downloader with no cache directory caches nothing");
  Expect(!std::filesystem::exists(std::filesystem::current_path() / "~"),
         "no cache tree may appear under the working directory");
}

void TestToolDownloaderAcceptsUppercaseExpectedSha() {
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
  const auto downloaded = downloader.Download(
      "tool", source.string(), "D4E4877BAC978B7952F0D544FC52EBFF5411D351D129F1F056FA43F11DA9AF2B");
  Expect(downloaded.has_value(),
         "an uppercase expected sha256 must verify against the lowercase computed digest");
}

void TestToolDownloaderRejectsTraversalToolId() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path cache_dir = temp_dir.path() / "cache";
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

  Expect(!downloader.IsCached("valid-tool"), "a safe but absent id is simply not cached");
}

void TestToolDownloaderEmptyHashDoesNotTrustCacheWithoutLocalSource() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path cache_dir = temp_dir.path() / "cache";
  std::filesystem::create_directories(cache_dir);
  WriteFile(cache_dir / "tool", "maybe-tampered\n");

  ToolDownloader downloader;
  downloader.SetCacheDir(cache_dir);

  Expect(!downloader.Download("tool", "https://example.invalid/tool.tgz", "").has_value(),
         "an empty sha256 must not blindly return a cached tool when no local source resolves");
}

void TestToolDownloaderEmptyHashReCopiesFromLocalSource() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path cache_dir = temp_dir.path() / "cache";
  const std::filesystem::path source = temp_dir.path() / "tool.bin";
  WriteFile(source, "fresh-bytes\n");
  std::filesystem::create_directories(cache_dir);
  WriteFile(cache_dir / "tool", "stale-bytes\n");

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
  const std::filesystem::path source = temp_dir.path() / "my tool.bin";
  WriteFile(source, "uri-bytes\n");

  ToolDownloader downloader;
  downloader.SetCacheDir(cache_dir);
  const std::string encoded = "file://" + (temp_dir.path() / "my%20tool.bin").generic_string();
  const auto path = downloader.Download("tool", encoded, "");
  Expect(path.has_value(), "a percent-encoded file:// URL resolves to the local file");
  Expect(ReadFile(cache_dir / "tool") == "uri-bytes\n", "the decoded local source is copied");
}

void TestToolDownloaderRejectsRemoteFileUri() {
  TemporaryDirectory temp_dir;
  ToolDownloader downloader;
  downloader.SetCacheDir(temp_dir.path() / "cache");
  Expect(!downloader.Download("tool", "file://remote/etc/passwd", "").has_value(),
         "a non-local file:// host is rejected");
}

void TestToolDownloaderRejectsMalformedFileUri() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path cache_dir = temp_dir.path() / "cache";
  const std::filesystem::path source = temp_dir.path() / "tool.bin";
  WriteFile(source, "payload\n");
  const std::string sha = "d4e4877bac978b7952f0d544fc52ebff5411d351d129f1f056fa43f11da9af2b";

  ToolDownloader downloader;
  downloader.SetCacheDir(cache_dir);

  const std::string base = "file://" + source.generic_string();
  Expect(downloader.Download("tool", base, sha).has_value(),
         "a well-formed file:// URL still resolves under the strict decoder");
  Expect(!downloader.Download("tool2", base + "%zz", sha).has_value(),
         "a malformed %-escape in a file:// tool URL must reject the whole URI");
  Expect(!downloader.Download("tool3", base + "%00tail", sha).has_value(),
         "a decoded NUL in a file:// tool URL must be rejected, not truncate the path");
}

void TestToolDownloaderHashMismatchLeavesNoPartialFile() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path cache_dir = temp_dir.path() / "cache";
  const std::filesystem::path source = temp_dir.path() / "tool.bin";
  WriteFile(source, "payload\n");

  ToolDownloader downloader;
  downloader.SetCacheDir(cache_dir);
  const auto path = downloader.Download("tool", source.string(), std::string(64, 'b'));
  Expect(!path.has_value(), "a hash mismatch fails the download");
  Expect(!std::filesystem::exists(cache_dir / "tool"),
         "no partial/unverified executable is published at the cache path");
  Expect(!std::filesystem::exists(cache_dir / "tool.partial"),
         "the temporary partial file is cleaned up on mismatch");
}

void TestToolDownloaderGetCachedToolVerifiesHash() {
  TemporaryDirectory temp_dir;
  const std::filesystem::path bin_dir = temp_dir.path() / "bin";
  const std::filesystem::path cache_dir = temp_dir.path() / "cache";
  std::filesystem::create_directories(bin_dir);
  std::filesystem::create_directories(cache_dir);
  WriteFile(cache_dir / "tool", "cached-bytes\n");

  ScopedEnvVar path_env("PATH", bin_dir.string());

  ToolDownloader downloader;
  downloader.SetCacheDir(cache_dir);

  Expect(downloader
             .GetCachedTool("tool",
                            "c6c1e528822cfad8fef7151f1eef6d626e884d873ae7ac4b3ac8d90768787a19")
             .has_value(),
         "GetCachedTool returns the path when the expected (in-process) digest matches");
  Expect(!downloader.GetCachedTool("tool", std::string(64, 'f')).has_value(),
         "GetCachedTool rejects a cached file whose digest does not match");
  Expect(downloader.GetCachedTool("tool").has_value(),
         "GetCachedTool returns the path unverified when no expected digest is given");
}

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

}

void RegisterWorkspaceToolDownloaderTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceToolDownloader/GetCachedToolVerifiesHash",
          TestToolDownloaderGetCachedToolVerifiesHash);
  AddTest(tests, "WorkspaceToolDownloader/RejectsRemoteSchemesNoNetworking",
          TestToolDownloaderRejectsRemoteSchemesNoNetworking);
  AddTest(tests, "WorkspaceToolDownloader/FileUriPercentDecodesLocalHost",
          TestToolDownloaderFileUriPercentDecodesLocalHost);
  AddTest(tests, "WorkspaceToolDownloader/RejectsMalformedFileUri",
          TestToolDownloaderRejectsMalformedFileUri);
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
  AddTest(tests, "WorkspaceToolDownloader/WithoutUserCacheDirectoryRefuses",
          TestToolDownloaderWithoutUserCacheDirectoryRefuses);
  AddTest(tests, "WorkspaceToolDownloader/EmptyHashReCopiesFromLocalSource",
          TestToolDownloaderEmptyHashReCopiesFromLocalSource);
}

}
