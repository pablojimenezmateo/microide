#include "GitMergeConflictFixtures.h"

#include <fstream>

#include "TestSupport.h"

namespace microide::tests {
namespace {

GitMergeConflictFixture MakeFixtureRoot() {
  auto temp_dir = std::make_unique<TemporaryDirectory>();
  GitMergeConflictFixture fixture{
      .temp_dir = std::move(temp_dir),
      .root = {},
      .base = {},
      .incoming = {},
      .current = {},
      .output = {},
  };
  fixture.root = fixture.temp_dir->path() / "repo";
  InitializeGitRepo(fixture.root);
  return fixture;
}

void CommitFile(const GitMergeConflictFixture& fixture,
                const std::filesystem::path& relative_path,
                const std::string& content,
                std::string_view message) {
  WriteFile(fixture.root / relative_path, content);
  CommitAll(fixture.root, message, "GitMergeConflictFixtures");
}

void MergeExpectingConflicts(const GitMergeConflictFixture& fixture, std::string_view branch) {
  if (RunGitCommand(fixture.root, {"merge", std::string(branch)}) == 0) {
    return;
  }
  RequireGitCommandSuccess(fixture.root, {"status", "--porcelain"}, "merge status");
}

}  // namespace

GitMergeConflictFixture CreateBothModifiedConflictRepo() {
  GitMergeConflictFixture fixture = MakeFixtureRoot();
  CommitFile(fixture, "conflict.txt", "base line\nshared\n", "base");
  RequireGitCommandSuccess(fixture.root, {"branch", "incoming"}, "create incoming branch");
  RequireGitCommandSuccess(fixture.root, {"checkout", "incoming"}, "checkout incoming");
  WriteFile(fixture.root / "conflict.txt", "base line\nincoming change\n");
  CommitAll(fixture.root, "incoming edit", "incoming commit");
  RequireGitCommandSuccess(fixture.root, {"checkout", "main"}, "checkout main");
  WriteFile(fixture.root / "conflict.txt", "base line\ncurrent change\n");
  CommitAll(fixture.root, "current edit", "current commit");
  MergeExpectingConflicts(fixture, "incoming");
  fixture.base = fixture.root / "conflict.txt";
  fixture.incoming = fixture.base;
  fixture.current = fixture.base;
  fixture.output = fixture.base;
  return fixture;
}

GitMergeConflictFixture CreateBothAddedConflictRepo() {
  GitMergeConflictFixture fixture = MakeFixtureRoot();
  RequireGitCommandSuccess(fixture.root, {"branch", "incoming"}, "create incoming branch");
  WriteFile(fixture.root / "added.txt", "incoming added\n");
  RequireGitCommandSuccess(fixture.root, {"add", "added.txt"}, "stage incoming add");
  CommitAll(fixture.root, "incoming add", "incoming add commit");
  RequireGitCommandSuccess(fixture.root, {"checkout", "main"}, "checkout main");
  WriteFile(fixture.root / "added.txt", "current added\n");
  RequireGitCommandSuccess(fixture.root, {"add", "added.txt"}, "stage current add");
  CommitAll(fixture.root, "current add", "current add commit");
  MergeExpectingConflicts(fixture, "incoming");
  fixture.base = fixture.root / "added.txt";
  fixture.incoming = fixture.base;
  fixture.current = fixture.base;
  fixture.output = fixture.base;
  return fixture;
}

GitMergeConflictFixture CreateDeleteModifyConflictRepo() {
  GitMergeConflictFixture fixture = MakeFixtureRoot();
  CommitFile(fixture, "delete-me.txt", "keep base\n", "base delete file");
  RequireGitCommandSuccess(fixture.root, {"branch", "incoming"}, "create incoming branch");
  RequireGitCommandSuccess(fixture.root, {"checkout", "incoming"}, "checkout incoming");
  RequireGitCommandSuccess(fixture.root, {"rm", "delete-me.txt"}, "incoming delete");
  CommitAll(fixture.root, "incoming delete", "incoming delete commit");
  RequireGitCommandSuccess(fixture.root, {"checkout", "main"}, "checkout main");
  WriteFile(fixture.root / "delete-me.txt", "keep base\nmodified on current\n");
  CommitAll(fixture.root, "current modify", "current modify commit");
  MergeExpectingConflicts(fixture, "incoming");
  fixture.base = fixture.root / "delete-me.txt";
  fixture.incoming = fixture.base;
  fixture.current = fixture.base;
  fixture.output = fixture.base;
  return fixture;
}

GitMergeConflictFixture CreateRenameRenameConflictRepo() {
  GitMergeConflictFixture fixture = MakeFixtureRoot();
  CommitFile(fixture, "original.txt", "shared content\n", "base rename");
  RequireGitCommandSuccess(fixture.root, {"branch", "incoming"}, "create incoming branch");
  RequireGitCommandSuccess(fixture.root, {"checkout", "incoming"}, "checkout incoming");
  RequireGitCommandSuccess(fixture.root, {"mv", "original.txt", "incoming-name.txt"}, "incoming rename");
  CommitAll(fixture.root, "incoming rename", "incoming rename commit");
  RequireGitCommandSuccess(fixture.root, {"checkout", "main"}, "checkout main");
  RequireGitCommandSuccess(fixture.root, {"mv", "original.txt", "current-name.txt"}, "current rename");
  CommitAll(fixture.root, "current rename", "current rename commit");
  MergeExpectingConflicts(fixture, "incoming");
  fixture.base = fixture.root / "incoming-name.txt";
  fixture.incoming = fixture.base;
  fixture.current = fixture.root / "current-name.txt";
  fixture.output = fixture.base;
  return fixture;
}

GitMergeConflictFixture CreateBinaryConflictRepo() {
  GitMergeConflictFixture fixture = MakeFixtureRoot();
  std::string binary_base(12, '\0');
  binary_base.replace(0, 4, "BIN1");
  CommitFile(fixture, "binary.dat", binary_base, "base binary");
  RequireGitCommandSuccess(fixture.root, {"branch", "incoming"}, "create incoming branch");
  RequireGitCommandSuccess(fixture.root, {"checkout", "incoming"}, "checkout incoming");
  std::string binary_incoming(12, '\0');
  binary_incoming.replace(0, 4, "BIN2");
  WriteFile(fixture.root / "binary.dat", binary_incoming);
  CommitAll(fixture.root, "incoming binary", "incoming binary commit");
  RequireGitCommandSuccess(fixture.root, {"checkout", "main"}, "checkout main");
  std::string binary_current(12, '\0');
  binary_current.replace(0, 4, "BIN3");
  WriteFile(fixture.root / "binary.dat", binary_current);
  CommitAll(fixture.root, "current binary", "current binary commit");
  MergeExpectingConflicts(fixture, "incoming");
  fixture.base = fixture.root / "binary.dat";
  fixture.incoming = fixture.base;
  fixture.current = fixture.base;
  fixture.output = fixture.base;
  return fixture;
}

GitMergeConflictFixture CreateCrlfConflictRepo() {
  GitMergeConflictFixture fixture = MakeFixtureRoot();
  CommitFile(fixture, "crlf.txt", "line one\r\nline two\r\n", "base crlf");
  RequireGitCommandSuccess(fixture.root, {"branch", "incoming"}, "create incoming branch");
  RequireGitCommandSuccess(fixture.root, {"checkout", "incoming"}, "checkout incoming");
  WriteFile(fixture.root / "crlf.txt", "line one\r\nincoming two\r\n");
  CommitAll(fixture.root, "incoming crlf", "incoming crlf commit");
  RequireGitCommandSuccess(fixture.root, {"checkout", "main"}, "checkout main");
  WriteFile(fixture.root / "crlf.txt", "line one\ncurrent two\n");
  CommitAll(fixture.root, "current lf", "current lf commit");
  MergeExpectingConflicts(fixture, "incoming");
  fixture.base = fixture.root / "crlf.txt";
  fixture.incoming = fixture.base;
  fixture.current = fixture.base;
  fixture.output = fixture.base;
  return fixture;
}

GitMergeConflictFixture CreateLargeConflictRepo(std::size_t conflict_blocks) {
  GitMergeConflictFixture fixture = MakeFixtureRoot();
  std::string base = "header\n";
  std::string incoming = "header\n";
  std::string current = "header\n";
  for (std::size_t i = 0; i < conflict_blocks; ++i) {
    base += "shared-" + std::to_string(i) + "\n";
    incoming += "incoming-" + std::to_string(i) + "\n";
    current += "current-" + std::to_string(i) + "\n";
  }
  base += "footer\n";
  incoming += "footer\n";
  current += "footer\n";
  CommitFile(fixture, "large.txt", base, "base large");
  RequireGitCommandSuccess(fixture.root, {"branch", "incoming"}, "create incoming branch");
  RequireGitCommandSuccess(fixture.root, {"checkout", "incoming"}, "checkout incoming");
  WriteFile(fixture.root / "large.txt", incoming);
  CommitAll(fixture.root, "incoming large", "incoming large commit");
  RequireGitCommandSuccess(fixture.root, {"checkout", "main"}, "checkout main");
  WriteFile(fixture.root / "large.txt", current);
  CommitAll(fixture.root, "current large", "current large commit");
  MergeExpectingConflicts(fixture, "incoming");
  fixture.base = fixture.root / "large.txt";
  fixture.incoming = fixture.base;
  fixture.current = fixture.base;
  fixture.output = fixture.base;
  return fixture;
}

}  // namespace microide::tests
