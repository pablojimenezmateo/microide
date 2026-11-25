#include "workspace/services/RemoteHostService.h"

#include <algorithm>

#include "platform/AppDirectories.h"
#include "platform/TransientFileStore.h"
#include "project/remote/RemoteProject.h"
#include "project/remote/RemoteWorkspace.h"
#include "util/TextFileIO.h"

namespace microide::workspace {

namespace remote = project::remote;

void RemoteHostService::BeginTerminalEdit(const std::filesystem::path& root, std::string epoch,
                                          terminal::TerminalHostEdit edit) {
  const auto project = projects_.find(root);
  if (project == projects_.end() || project->second.closed) {
    return;
  }
  std::string name = std::filesystem::path(edit.path).filename().string();
  if (name.empty()) {
    name = "file";
  }
  std::string error;
  const std::optional<std::filesystem::path> copy = platform::StoreTransientFile(
      platform::ResolveAppDirectory(platform::UserDirectoryKind::State, "microide") / "host-edit", name,
      edit.text, &error);
  if (!copy.has_value()) {
    if (operations_.notify) {
      operations_.notify(NotificationService::Request{
          .tone = NotificationService::Tone::Warning,
          .message = name + " from " + project->second.host + " could not be opened for editing: " + error});
    }
    remote::RemoteWorkspace(project->second.project->session().connection())
        .FinishTerminalEdit(epoch, edit.token, std::nullopt, [](std::optional<bool>) {});
    return;
  }
  terminal_edits_.push_back(TerminalEdit{
      .root = root, .epoch = std::move(epoch), .token = edit.token, .copy = copy->lexically_normal()});
  if (operations_.open_file_at) {
    operations_.open_file_at(*copy, edit.line > 0 ? edit.line - 1 : 0, 0);
  }
}

bool RemoteHostService::FinishTerminalEdit(const std::filesystem::path& path) {
  if (terminal_edits_.empty()) {
    return false;
  }
  const auto edit = std::ranges::find(terminal_edits_, path.lexically_normal(), &TerminalEdit::copy);
  if (edit == terminal_edits_.end()) {
    return false;
  }
  edit->closed = true;
  SendTerminalEdit(*edit);
  return true;
}

void RemoteHostService::SendTerminalEdit(TerminalEdit& edit) {
  const auto project = projects_.find(edit.root);
  if (edit.sending || project == projects_.end() || project->second.closed) {
    return;
  }
  edit.sending = true;
  std::shared_ptr<remote::RemoteConnection> connection = project->second.project->session().connection();
  work_.Post([this, connection, epoch = edit.epoch, token = edit.token, copy = edit.copy]() {
    remote::RemoteWorkspace(connection).FinishTerminalEdit(
        epoch, token, util::ReadTextFile(copy), [this, epoch, token](std::optional<bool> delivered) {
          mailbox_.Post([this, epoch, token, delivered]() { OnTerminalEditAnswered(token, epoch, delivered); });
        });
  });
}

void RemoteHostService::OnTerminalEditAnswered(std::uint64_t token, const std::string& epoch,
                                               std::optional<bool> delivered) {
  const auto edit = std::ranges::find_if(terminal_edits_, [&](const TerminalEdit& candidate) {
    return candidate.token == token && candidate.epoch == epoch;
  });
  if (edit == terminal_edits_.end()) {
    return;
  }
  edit->sending = false;
  if (!delivered.has_value()) {
    return;
  }
  std::error_code ignored;
  if (*delivered) {
    std::filesystem::remove_all(edit->copy.parent_path(), ignored);
  } else if (operations_.notify) {
    operations_.notify(NotificationService::Request{
        .tone = NotificationService::Tone::Info,
        .message = "Nothing on the host waits for " + edit->copy.filename().string() +
                   " any more; what you wrote is kept in " + edit->copy.string()});
  }
  terminal_edits_.erase(edit);
}

void RemoteHostService::ResendTerminalEdits(const std::filesystem::path& root) {
  for (TerminalEdit& edit : terminal_edits_) {
    if (edit.closed && edit.root == root) {
      SendTerminalEdit(edit);
    }
  }
}

}
