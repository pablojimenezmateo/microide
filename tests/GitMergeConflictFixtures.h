#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace microide::tests {

class TemporaryDirectory;

struct GitMergeConflictFixture {
  std::unique_ptr<TemporaryDirectory> temp_dir;
  std::filesystem::path root;
  std::filesystem::path base;
  std::filesystem::path incoming;
  std::filesystem::path current;
  std::filesystem::path output;
};

GitMergeConflictFixture CreateBothModifiedConflictRepo();
GitMergeConflictFixture CreateBothAddedConflictRepo();
GitMergeConflictFixture CreateDeleteModifyConflictRepo();
GitMergeConflictFixture CreateRenameRenameConflictRepo();
GitMergeConflictFixture CreateBinaryConflictRepo();
GitMergeConflictFixture CreateCrlfConflictRepo();
GitMergeConflictFixture CreateLargeConflictRepo(std::size_t conflict_blocks);

}  // namespace microide::tests
