#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "plugin/PluginHost.h"
#include "workspace/WorkspaceContext.h"
#include "workspace/lsp/LspPositionEncoding.h"
#include "workspace/lsp/WorkspaceLspClient.h"
#include "workspace/state/WorkspaceProjectState.h"

namespace microide::project {
class FileWriteGate;
}

namespace microide::workspace {

class LspService;

struct ClosedFileEditBucket {
  std::filesystem::path path;
  std::vector<std::pair<editor::SelectionRange, std::string>> edits;
  lsp_encoding::PositionEncoding encoding = lsp_encoding::PositionEncoding::Utf8;
};

class WorkspaceEditService {
 public:
  struct Operations {
    std::function<editor::TextViewport*()> active_editable_viewport;
    std::function<void()> reset_caret_blink;
    std::function<void()> note_active_editable_edit;
    std::function<void()> request_frame;
    std::function<void(std::string detail)> confirm_rename_save;
    std::function<void(std::string message)> notify_error;
    std::function<void(const std::filesystem::path&)> notify_buffer_saved;
    std::function<void(std::vector<ClosedFileEditBucket>)> run_closed_file_edits_async;
    std::function<void(const std::filesystem::path&, const std::filesystem::path&)>
        reconcile_tabs_after_resource_rename;
    std::function<void(const editor::TextViewport&)> follow_applied_edit;
    std::function<void(const std::filesystem::path&)> reconcile_tabs_after_resource_delete;
    std::function<void(const std::filesystem::path&)> refresh_views_after_resource_ops;
    std::function<void(std::vector<std::filesystem::path>)> dispose_staged_paths_async;
  };

  void Configure(WorkspaceContext& context, LspService& lsp, Operations operations);

  bool ApplyPluginEdit(const plugin::PluginHost::WorkspaceEditRequest& request);
  bool ApplyLspEdit(const std::vector<CodeActionEdit>& edits, bool* out_any_rejected = nullptr);
  bool ApplyBufferEdits(const std::vector<CodeActionEdit>& edits);

 private:
  bool ApplyOpenBufferEdits(const std::vector<CodeActionEdit>& edits, bool lsp_columns, bool* out_any_rejected);

 public:
  void ApplyRename(const std::string& new_name, const std::vector<CodeActionEdit>& edits,
                   const std::vector<WorkspaceResourceOp>& resource_ops);
  void CommitPendingRename();
  void DiscardPendingRename() { pending_rename_.reset(); }

  struct DiskEditResult {
    std::size_t files_written = 0;
    std::size_t edits_applied = 0;
    bool any_failed = false;
  };
  DiskEditResult ApplyToClosedFiles(const std::vector<CodeActionEdit>& edits);
  static DiskEditResult RunClosedFileEdits(const std::vector<ClosedFileEditBucket>& buckets,
                                           project::FileWriteGate& gate);

  struct ResourceOpsResult {
    bool ok = true;
    bool any_applied = false;
    std::string error;
  };
  ResourceOpsResult ApplyResourceOps(const std::vector<WorkspaceResourceOp>& ops);
  bool ApplyFull(const std::vector<CodeActionEdit>& edits,
                 const std::vector<WorkspaceResourceOp>& resource_ops);
  bool ApplyServerEdit(const LspClient& client, LspClient::WorkspaceEdit edit);

 private:
  using EditBatch = std::vector<std::pair<editor::SelectionRange, std::string>>;

  editor::TextViewport* ResolveOpenViewport(const std::filesystem::path& path) const;
  static bool ApplyBatch(editor::TextViewport& viewport, const EditBatch& batch,
                         bool record_undo = true,
                         const std::function<void(const editor::TextViewport&)>& follow = {});
  void MarkFoldsDirty(const editor::TextViewport& edited);
  bool IsOpen(const std::filesystem::path& normalized) const;
  std::vector<ClosedFileEditBucket> PrepareClosedFileBuckets(const std::vector<CodeActionEdit>& edits) const;

  struct PendingRename {
    std::string new_name;
    std::vector<CodeActionEdit> edits;
    std::vector<std::filesystem::path> affected_paths;
    std::vector<WorkspaceResourceOp> resource_ops;
  };

  WorkspaceContext* context_ = nullptr;
  LspService* lsp_ = nullptr;
  Operations operations_{};
  std::optional<PendingRename> pending_rename_;
};

}
