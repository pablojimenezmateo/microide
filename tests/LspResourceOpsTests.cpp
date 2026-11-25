#include "TestSupport.h"

#include "workspace/lsp/LspService.h"
#include "workspace/services/WorkspaceEditService.h"
#include "workspace/registries/WorkspaceCodeActionRegistry.h"
#include "workspace/registries/WorkspaceCompletionRegistry.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/state/WorkspaceProjectState.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace microide::tests {
namespace {

using microide::workspace::CodeActionRegistry;
using microide::workspace::CompletionRegistry;
using microide::workspace::LspService;
using microide::workspace::WorkspaceEditService;
using microide::workspace::WorkspaceContext;
using microide::workspace::WorkspaceResourceOp;
using Kind = WorkspaceResourceOp::Kind;

struct Fixture {
  TemporaryDirectory dir;
  WorkspaceContext context;
  CompletionRegistry completions;
  CodeActionRegistry code_actions;
  LspService lsp;
  WorkspaceEditService service;
  std::vector<std::string> log;

  Fixture() {
    context.current_project_state.root = dir.path();
    lsp.Configure(context, completions, code_actions, LspService::Operations{});
    WorkspaceEditService::Operations ops;
    ops.reconcile_tabs_after_resource_rename = [this](const std::filesystem::path& from,
                                                      const std::filesystem::path& to) {
      log.push_back("rename:" + from.filename().string() + "->" + to.filename().string());
    };
    ops.reconcile_tabs_after_resource_delete = [this](const std::filesystem::path& path) {
      log.push_back("delete:" + path.filename().string());
    };
    ops.refresh_views_after_resource_ops = [this](const std::filesystem::path&) {
      log.push_back("refresh");
    };
    service.Configure(context, lsp, std::move(ops));
  }

  std::filesystem::path At(std::string_view relative) const { return dir.path() / relative; }
};

void WriteText(const std::filesystem::path& path, std::string_view text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

WorkspaceResourceOp Create(std::filesystem::path path) {
  return WorkspaceResourceOp{.kind = Kind::Create, .path = std::move(path)};
}

WorkspaceResourceOp Rename(std::filesystem::path from, std::filesystem::path to) {
  return WorkspaceResourceOp{.kind = Kind::Rename, .path = std::move(from),
                             .new_path = std::move(to)};
}

WorkspaceResourceOp Delete(std::filesystem::path path) {
  return WorkspaceResourceOp{.kind = Kind::Delete, .path = std::move(path)};
}

void TestCreateRenameDeleteApplyInOrder() {
  Fixture fx;
  WriteText(fx.At("old.rs"), "mod a;\n");

  const auto result = fx.service.ApplyResourceOps({
      Create(fx.At("nested/new.rs")),
      Rename(fx.At("old.rs"), fx.At("renamed.rs")),
  });

  Expect(result.ok, "a well-formed op batch should apply");
  Expect(result.any_applied, "the batch mutated the filesystem");
  Expect(std::filesystem::exists(fx.At("nested/new.rs")),
         "create should materialize the file and its parent directory");
  Expect(!std::filesystem::exists(fx.At("old.rs")) && std::filesystem::exists(fx.At("renamed.rs")),
         "rename should move the file");
  Expect(ReadText(fx.At("renamed.rs")) == "mod a;\n", "rename must preserve file contents");
  Expect(fx.log == std::vector<std::string>{"rename:old.rs->renamed.rs", "refresh"},
         "only the rename reconciles; the view refresh runs once after the batch");
}

void TestReconcileFollowsApplyOrderNotOpKind() {
  Fixture fx;
  WriteText(fx.At("a.rs"), "a\n");
  WriteText(fx.At("b.rs"), "b\n");

  const auto result = fx.service.ApplyResourceOps({
      Delete(fx.At("b.rs")),
      Rename(fx.At("a.rs"), fx.At("b.rs")),
  });

  Expect(result.ok, "delete-then-rename-onto-the-deleted-path is a legal batch");
  Expect(ReadText(fx.At("b.rs")) == "a\n", "b.rs should hold the renamed a.rs contents");
  Expect(fx.log == std::vector<std::string>{"delete:b.rs", "rename:a.rs->b.rs", "refresh"},
         "reconcile hooks must run in apply order, not grouped delete-after-rename");
}

void TestPreconditionFailureLeavesNothingApplied() {
  Fixture fx;
  WriteText(fx.At("keep.rs"), "keep\n");
  WriteText(fx.At("clash.rs"), "clash\n");

  const auto result = fx.service.ApplyResourceOps({
      Create(fx.At("fresh.rs")),
      Create(fx.At("clash.rs")),
  });

  Expect(!result.ok && !result.any_applied, "a failed precondition fails the whole batch");
  Expect(!result.error.empty(), "the failure should carry a message");
  Expect(!std::filesystem::exists(fx.At("fresh.rs")),
         "no op may run when a later op fails validation");
  Expect(ReadText(fx.At("clash.rs")) == "clash\n", "the existing file must be untouched");
  Expect(fx.log.empty(), "a rejected batch reconciles nothing");
}

void TestValidationUsesTheSimulatedOverlay() {
  Fixture fx;
  WriteText(fx.At("src.rs"), "src\n");

  const auto created_then_renamed = fx.service.ApplyResourceOps({
      Create(fx.At("staged.rs")),
      Rename(fx.At("staged.rs"), fx.At("final.rs")),
  });
  Expect(created_then_renamed.ok, "the rename must validate against the yet-to-be-created file");
  Expect(std::filesystem::exists(fx.At("final.rs")) && !std::filesystem::exists(fx.At("staged.rs")),
         "the created file should end up at the rename destination");

  const auto deleted_then_created = fx.service.ApplyResourceOps({
      Delete(fx.At("src.rs")),
      Create(fx.At("src.rs")),
  });
  Expect(deleted_then_created.ok, "recreating a path the batch just deleted is legal");
  Expect(ReadText(fx.At("src.rs")).empty(), "the recreated file starts empty");
}

void TestIgnoreOptionsResolveToNoOps() {
  Fixture fx;
  WriteText(fx.At("present.rs"), "present\n");

  WorkspaceResourceOp create_existing = Create(fx.At("present.rs"));
  create_existing.ignore_if_exists = true;
  WorkspaceResourceOp delete_missing = Delete(fx.At("absent.rs"));
  delete_missing.ignore_if_not_exists = true;

  const auto result = fx.service.ApplyResourceOps({create_existing, delete_missing});
  Expect(result.ok, "ignore-option ops that resolve to no-ops are not failures");
  Expect(!result.any_applied, "an all-no-op batch reports nothing applied");
  Expect(ReadText(fx.At("present.rs")) == "present\n",
         "ignoreIfExists must not truncate the existing file");
  Expect(fx.log.empty(), "a no-op batch neither reconciles nor refreshes");
}

void TestOverwriteWinsOverIgnoreIfExists() {
  Fixture fx;
  WriteText(fx.At("target.rs"), "old contents\n");

  WorkspaceResourceOp op = Create(fx.At("target.rs"));
  op.overwrite = true;
  op.ignore_if_exists = true;

  const auto result = fx.service.ApplyResourceOps({op});
  Expect(result.ok && result.any_applied, "overwrite should apply");
  Expect(ReadText(fx.At("target.rs")).empty(), "overwrite creates an empty file");
}

void TestTargetsOutsideTheProjectRootAreRejected() {
  Fixture fx;
  const std::filesystem::path outside = fx.dir.path().parent_path() / "escaped.rs";
  std::filesystem::remove(outside);

  Expect(!fx.service.ApplyResourceOps({Create(outside)}).ok,
         "a create outside the project root must fail the batch");
  Expect(!std::filesystem::exists(outside), "the escaping op must not touch the filesystem");

  WriteText(fx.At("inside.rs"), "inside\n");
  Expect(!fx.service.ApplyResourceOps({Rename(fx.At("inside.rs"), outside)}).ok,
         "a rename destination outside the project root must fail the batch");
  Expect(std::filesystem::exists(fx.At("inside.rs")) && !std::filesystem::exists(outside),
         "the rejected rename must leave the source in place");

  Expect(!fx.service.ApplyResourceOps({Create(fx.At("../traversed.rs"))}).ok,
         "a ..-traversal target must fail the batch");
}

void TestTargetsEscapingThroughASymlinkAreRejected() {
  Fixture fx;
  const std::filesystem::path outside_dir = fx.dir.path().parent_path() / "outside-root";
  std::error_code ec;
  std::filesystem::remove_all(outside_dir, ec);
  std::filesystem::create_directories(outside_dir, ec);
  Expect(!ec, "outside-the-root fixture directory created");
  std::filesystem::create_symlink(outside_dir, fx.At("link"), ec);
  Expect(!ec, "symlink fixture created");

  const std::filesystem::path through_link = fx.At("link") / "escaped.rs";
  Expect(!fx.service.ApplyResourceOps({Create(through_link)}).ok,
         "a create through a symlink out of the project must fail the batch");
  Expect(!std::filesystem::exists(outside_dir / "escaped.rs"),
         "nothing may be written outside the project root");

  WriteText(fx.At("inside.rs"), "inside\n");
  Expect(!fx.service.ApplyResourceOps({Rename(fx.At("inside.rs"), through_link)}).ok,
         "a rename destination through a symlink out of the project must fail");
  Expect(std::filesystem::exists(fx.At("inside.rs")),
         "the rejected rename must leave the source in place");

  std::filesystem::create_directories(fx.At("real"), ec);
  std::filesystem::create_symlink(fx.At("real"), fx.At("inner_link"), ec);
  Expect(!ec, "inside-the-root symlink fixture created");
  Expect(fx.service.ApplyResourceOps({Create(fx.At("inner_link") / "ok.rs")}).ok,
         "a create through a symlink that stays inside the project must succeed");
  Expect(std::filesystem::exists(fx.At("real") / "ok.rs"),
         "the file lands at the link's real location inside the project");

  std::filesystem::remove_all(outside_dir, ec);
}

void TestNonRecursiveDirectoryDeleteIsRefused() {
  Fixture fx;
  WriteText(fx.At("pkg/mod.rs"), "mod\n");

  Expect(!fx.service.ApplyResourceOps({Delete(fx.At("pkg"))}).ok,
         "deleting a non-empty directory without `recursive` must fail");
  Expect(std::filesystem::exists(fx.At("pkg/mod.rs")), "the directory must survive the refusal");

  WorkspaceResourceOp recursive = Delete(fx.At("pkg"));
  recursive.recursive = true;
  Expect(fx.service.ApplyResourceOps({recursive}).ok,
         "`recursive` permits the directory delete");
  Expect(!std::filesystem::exists(fx.At("pkg")), "the recursive delete should remove the tree");
}

void TestMidBatchFailureRollsBackCompletedOps() {
  Fixture fx;
  WriteText(fx.At("doomed.rs"), "original\n");
  WriteText(fx.At("source.rs"), "source\n");

  WriteText(fx.At("blocker"), "not a directory\n");

  const auto result = fx.service.ApplyResourceOps({
      Delete(fx.At("doomed.rs")),
      Rename(fx.At("source.rs"), fx.At("blocker/child.rs")),
  });

  Expect(!result.ok, "an apply-time I/O failure fails the batch");
  Expect(!result.any_applied, "a rolled-back batch reports nothing applied");
  Expect(std::filesystem::exists(fx.At("doomed.rs")) &&
             ReadText(fx.At("doomed.rs")) == "original\n",
         "rollback must restore the staged delete byte-identically");
  Expect(std::filesystem::exists(fx.At("source.rs")), "the un-run rename leaves its source alone");
  Expect(fx.log.empty(), "a rolled-back batch reconciles nothing");
}

void TestStagedBackupsAreDisposedAfterSuccess() {
  Fixture fx;
  WriteText(fx.At("gone.rs"), "gone\n");

  Expect(fx.service.ApplyResourceOps({Delete(fx.At("gone.rs"))}).ok,
         "the delete should apply");
  Expect(!std::filesystem::exists(fx.At("gone.rs")), "the deleted file is gone");
  for (const auto& entry : std::filesystem::directory_iterator(fx.dir.path())) {
    Expect(entry.path().filename().string().rfind(".microide-lsp-staged-", 0) != 0,
           "a successful batch must dispose its staged backups");
  }
}

void TestEmptyBatchIsASuccessfulNoOp() {
  Fixture fx;
  const auto result = fx.service.ApplyResourceOps({});
  Expect(result.ok && !result.any_applied, "an empty op list is a successful no-op");
  Expect(fx.log.empty(), "an empty batch reconciles nothing");
}

}

void RegisterLspResourceOpsTests(std::vector<TestCase>& tests) {
  AddTest(tests, "LspResourceOps/CreateRenameDeleteApplyInOrder",
          TestCreateRenameDeleteApplyInOrder);
  AddTest(tests, "LspResourceOps/ReconcileFollowsApplyOrderNotOpKind",
          TestReconcileFollowsApplyOrderNotOpKind);
  AddTest(tests, "LspResourceOps/PreconditionFailureLeavesNothingApplied",
          TestPreconditionFailureLeavesNothingApplied);
  AddTest(tests, "LspResourceOps/ValidationUsesTheSimulatedOverlay",
          TestValidationUsesTheSimulatedOverlay);
  AddTest(tests, "LspResourceOps/IgnoreOptionsResolveToNoOps", TestIgnoreOptionsResolveToNoOps);
  AddTest(tests, "LspResourceOps/OverwriteWinsOverIgnoreIfExists",
          TestOverwriteWinsOverIgnoreIfExists);
  AddTest(tests, "LspResourceOps/TargetsOutsideTheProjectRootAreRejected",
          TestTargetsOutsideTheProjectRootAreRejected);
  AddTest(tests, "LspResourceOps/TargetsEscapingThroughASymlinkAreRejected",
          TestTargetsEscapingThroughASymlinkAreRejected);
  AddTest(tests, "LspResourceOps/NonRecursiveDirectoryDeleteIsRefused",
          TestNonRecursiveDirectoryDeleteIsRefused);
  AddTest(tests, "LspResourceOps/MidBatchFailureRollsBackCompletedOps",
          TestMidBatchFailureRollsBackCompletedOps);
  AddTest(tests, "LspResourceOps/StagedBackupsAreDisposedAfterSuccess",
          TestStagedBackupsAreDisposedAfterSuccess);
  AddTest(tests, "LspResourceOps/EmptyBatchIsASuccessfulNoOp", TestEmptyBatchIsASuccessfulNoOp);
}

}
