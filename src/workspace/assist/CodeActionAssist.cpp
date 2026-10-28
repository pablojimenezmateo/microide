#include "workspace/assist/CodeActionAssist.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "util/JsonValue.h"
#include "workspace/FileUri.h"
#include "workspace/lsp/LspViewportPositions.h"
#include "workspace/lsp/LspWorkspaceEditOps.h"
#include "workspace/services/WorkspaceEditService.h"

namespace microide::workspace {

namespace {

constexpr const char* kLoading = "Loading\xE2\x80\xA6";

std::string JsonValueToArgumentString(const util::JsonValue& value) {
  return value.IsString() ? value.AsString() : util::SerializeJson(value);
}

}  // namespace

bool CodeActionAssist::ShowOverlay(std::string* error_message,
                                   const editor::SelectionRange* explicit_range) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  const AssistHost::Operations& ops = host_.ops();
  editor::TextViewport* viewport = ops.active_editable_viewport();
  if (viewport == nullptr || viewport->path().empty()) {
    return false;
  }

  const std::string language_id = viewport->language_id();
  const std::filesystem::path request_path = viewport->path();
  const editor::TextPosition caret{viewport->cursor_line(), viewport->cursor_column()};
  const editor::SelectionRange range =
      explicit_range != nullptr
          ? *explicit_range
          : viewport->selection_range().value_or(editor::SelectionRange{.start = caret, .end = caret});

  // Open the overlay in a loading state immediately, then query the plugin worker
  // and the language server CONCURRENTLY. Neither blocks the UI; each fills the
  // overlay on its own drain via PublishMerge (LSP-first, de-duped).
  auto& session = host_.context().current_project_state.overlay.workflow.code_actions;
  session.items.clear();
  session.selected_index = 0;
  session.source = "lsp";
  session.error = kLoading;
  ops.show_overlay(OverlayMode::CodeActions);

  auto merge = std::make_shared<Merge>();
  merge->language_id = language_id;
  merge->generation = ++request_generation_;

  LspClient* client = ops.lsp_client_for_viewport(*viewport, nullptr);
  merge->sources.lsp_authoritative = client != nullptr;
  if (client != nullptr) {
    ops.ensure_lsp_document_open(*viewport, *client, language_id);
    const lsp_encoding::PositionEncoding encoding = LspEncodingForClient(*client);
    std::vector<LspClient::Diagnostic> context_diagnostics;
    if (port_.collect_lsp_context_diagnostics) {
      context_diagnostics = port_.collect_lsp_context_diagnostics(*viewport, range, encoding);
    }
    ops.begin_tracked_lsp_request();
    client->RequestCodeActionAsync(
        FileUriForPath(request_path),
        LspClient::Range{
            .start =
                ByteColumnToLspPosition(*viewport, range.start.line, range.start.column, encoding),
            .end = ByteColumnToLspPosition(*viewport, range.end.line, range.end.column, encoding),
        },
        std::move(context_diagnostics),
        [this, request_path, merge](LspResult<std::vector<LspClient::CodeAction>> actions) {
          host_.ops().finish_tracked_lsp_request();
          merge->sources.lsp_pending = false;
          if (host_.IsStale(request_path) || merge->generation != request_generation_) {
            return;
          }
          merge->lsp_items = TransformLspCodeActions(actions);
          PublishMerge(merge, request_path);
        });
  } else {
    merge->sources.lsp_pending = false;
  }

  host_.plugins().Host().QueryCodeActionsAsync(
      language_id, request_path, range.start.line + 1, range.start.column + 1, range.end.line + 1,
      range.end.column + 1,
      [this, request_path, merge](std::vector<plugin::PluginHost::CodeActionCandidate> items,
                                  std::string /*provider_error*/) {
        merge->sources.plugin_pending = false;
        if (host_.IsStale(request_path) || merge->generation != request_generation_) {
          return;
        }
        merge->plugin_items = TransformPluginCodeActions(items);
        PublishMerge(merge, request_path);
      });
  return true;
}

std::vector<CodeActionSessionItem> CodeActionAssist::TransformPluginCodeActions(
    const std::vector<plugin::PluginHost::CodeActionCandidate>& items) {
  std::vector<CodeActionSessionItem> result;
  result.reserve(items.size());
  for (const auto& item : items) {
    result.push_back(CodeActionSessionItem{
        .title = item.title,
        .command = item.command,
        .arguments = item.arguments,
    });
  }
  return result;
}

std::vector<CodeActionSessionItem> CodeActionAssist::TransformLspCodeActions(
    const LspResult<std::vector<LspClient::CodeAction>>& actions) {
  std::vector<CodeActionSessionItem> result;
  if (!actions.has_value()) {
    return result;
  }
  // Shared aggregate budget across ALL actions so a server returning many large
  // (but individually capped) inline WorkspaceEdits cannot force the overlay to hold
  // the sum of every action's edit payload before the user selects one. Past the
  // budget, an action's inline fix is not materialized (edits_truncated set).
  // TD-2026-07-17A-057.
  constexpr std::size_t kMaxAggregateEdits = 50000;
  constexpr std::size_t kMaxAggregateEditBytes = 16u * 1024 * 1024;  // 16 MiB
  std::size_t total_edits = 0;
  std::size_t total_edit_bytes = 0;
  result.reserve(actions->size());
  for (const auto& action : *actions) {
    std::vector<std::string> arguments;
    arguments.reserve(action.arguments.size());
    for (const auto& argument : action.arguments) {
      arguments.push_back(JsonValueToArgumentString(argument));
    }
    std::vector<CodeActionEdit> edits;
    std::vector<WorkspaceResourceOp> resource_ops;
    bool edits_truncated = false;
    if (action.has_edit) {
      // Flatten the action's file resource ops (e.g. rust-analyzer "extract
      // module" creates the target file). One undecodable op URI disables the
      // whole inline fix — applying the text edits without their ops would leave
      // the workspace inconsistent.
      if (std::optional<std::vector<WorkspaceResourceOp>> flattened =
              lsp_workspace_edit::FlattenResourceOps(action.edit.resource_ops);
          flattened.has_value()) {
        resource_ops = *std::move(flattened);
      } else {
        edits_truncated = true;
      }
      for (const auto& [uri, text_edits] : action.edit.changes) {
        if (edits_truncated) {
          break;
        }
        const std::optional<std::filesystem::path> path = PathFromFileUri(uri);
        if (!path.has_value()) {
          // An undecodable / non-local / malformed URI must never fall back to
          // the empty path, which the apply layer interprets as "edit the active
          // buffer". Skip these edits rather than corrupt whatever file is focused.
          continue;
        }
        for (const auto& [lsp_range, new_text] : text_edits) {
          if (total_edits >= kMaxAggregateEdits ||
              total_edit_bytes + new_text.size() > kMaxAggregateEditBytes) {
            // Budget exhausted: drop this action's (partial) edits AND its
            // resource ops so the overlay never holds a half-applied fix, and
            // mark it disabled.
            edits.clear();
            resource_ops.clear();
            edits_truncated = true;
            break;
          }
          ++total_edits;
          total_edit_bytes += new_text.size();
          edits.push_back(assist::ToCodeActionEdit(*path, lsp_range, new_text));
        }
      }
    }
    result.push_back(CodeActionSessionItem{
        .title = action.title,
        .command = action.command,
        .arguments = std::move(arguments),
        .edits = std::move(edits),
        .resource_ops = std::move(resource_ops),
        .edits_truncated = edits_truncated,
    });
  }
  return result;
}

void CodeActionAssist::PublishMerge(const std::shared_ptr<Merge>& merge,
                                    const std::filesystem::path& request_path) {
  if (host_.IsStale(request_path)) {
    return;
  }
  auto& session = host_.context().current_project_state.overlay.workflow.code_actions;
  const auto& primary = merge->sources.lsp_authoritative ? merge->lsp_items : merge->plugin_items;
  const auto& secondary =
      merge->sources.lsp_authoritative ? merge->plugin_items : merge->lsp_items;
  // De-dupe by title + command so an identical action offered by both sources
  // appears once (an empty command still distinguishes edit-only quickfixes).
  session.items = assist_merge::RankedUnion(
      primary, secondary,
      [](const CodeActionSessionItem& item) { return item.title + "\x1f" + item.command; });
  if (session.selected_index >= session.items.size()) {
    session.selected_index = 0;
  }
  session.source = merge->lsp_items.empty() ? "plugin" : "lsp";
  if (merge->sources.AnyPending()) {
    session.error = session.items.empty() ? kLoading : std::string{};
  } else if (session.items.empty()) {
    session.error = "No code actions available";
    host_.MaybeLogLspUnavailable(merge->language_id, merge->sources.lsp_authoritative);
  } else {
    session.error.clear();
  }
  host_.ops().request_overlay_redraw();
}

bool CodeActionAssist::Finish(bool applied, std::string error) {
  if (applied) {
    host_.ops().dismiss_overlay(true);
  } else {
    host_.context().current_project_state.overlay.workflow.code_actions.error = std::move(error);
    host_.ops().request_overlay_redraw();
  }
  return applied;
}

bool CodeActionAssist::ExecuteSelected() {
  auto& session = host_.context().current_project_state.overlay.workflow.code_actions;
  if (session.items.empty()) {
    return false;
  }
  const AssistHost::Operations& ops = host_.ops();
  const CodeActionSessionItem& action =
      session.items[std::min(session.selected_index, session.items.size() - 1)];

  // Inline WorkspaceEdit actions (clangd quickfixes like "remove #include X")
  // apply directly to the open buffers; command-style actions dispatch through
  // the command registry. An action with neither is inert. File resource ops
  // (create/rename/delete) run FIRST — the edits are keyed by the post-op paths
  // (e.g. rust-analyzer "extract module" creates the file its edits then fill).
  if (!action.resource_ops.empty()) {
    // Ops-carrying fix: the full applier runs the ops (rollback-safe), then the
    // edits against open buffers AND closed files on disk (the edits typically
    // fill the file the ops just created).
    return Finish(host_.edits().ApplyFull(action.edits, action.resource_ops), "Could not apply fix");
  }
  if (!action.edits.empty()) {
    return Finish(host_.edits().ApplyLspEdit(action.edits), "Could not apply fix");
  }
  if (action.command.empty()) {
    return false;
  }
  std::string error_message;
  const bool executed = port_.execute_command_name(action.command, action.arguments, &error_message);
  return Finish(executed, std::move(error_message));
}

}  // namespace microide::workspace
