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

}  // namespace

void RegisterWorkspaceToolDownloaderTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WorkspaceToolDownloader/FallsBackToShasumWhenSha256sumMissing",
          TestToolDownloaderFallsBackToShasumWhenSha256sumMissing);
}

}  // namespace microide::tests
