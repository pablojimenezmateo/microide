#include "workspace/actions/WorkspaceActionCoordinator.h"

#include <string>
#include <utility>
#include <vector>

#include "util/Parse.h"
#include "workspace/services/RemoteHostService.h"

namespace microide::workspace {

ActionCoordinator::DispatchResult ActionCoordinator::ExecuteRemote(ActionId id, const std::vector<std::string>& args,
                                                                   ActionSource ,
                                                                   std::string* rejection_feedback) {
  const auto reject = RejectInto(rejection_feedback);
  switch (id) {
    case ActionId::RemoteOpenLocalTerminal:
    case ActionId::RemoteOpenFolder:
    case ActionId::RemoteResolveConflict:
    case ActionId::RemoteResolveAllConflicts:
    case ActionId::RemoteBlockedSave:
    case ActionId::RemoteShowConflicts:
    case ActionId::RemoteShowLog:
    case ActionId::RemoteShowStatus:
    case ActionId::RemoteStopServer:
    case ActionId::RemoteAttachHostTerminals:
    case ActionId::RemoteCloseHostTerminals:
    case ActionId::RemoteEndHostProcess:
    case ActionId::RemoteDisconnect:
    case ActionId::RemoteReconnect:
    case ActionId::RemoteRemoveMirror:
    case ActionId::RemoteCopyCommand:
    case ActionId::RemoteShowHostChanges:
    case ActionId::RemoteClearHostChanges:
    case ActionId::RemoteFileHistory:
    case ActionId::RemoteOpenOfferedSession:
    case ActionId::RemoteApplyHeldDeletes:
    case ActionId::RemoteForwardPort:
    case ActionId::RemoteUnforwardPort: {
      RemoteHostService* hosts = context_.RemoteHosts();
      if (hosts == nullptr) {
        return reject("Remote hosts are not available here");
      }
      const auto host_arg = [&]() -> std::string { return args.empty() ? hosts->SoleHost() : args[0]; };
      const auto bring_remote_project_forward = [&](std::filesystem::path& root) -> std::string {
        root = args.empty() ? context_.ProjectRoot() : std::filesystem::path(args[0]);
        if (!hosts->IsRemoteProject(root)) {
          return args.empty() ? "The active project is not a remote project"
                              : root.string() + " is not an open remote project";
        }
        if (root != context_.ProjectRoot() && !context_.OpenProject(root, true, true)) {
          return "Cannot bring " + root.string() + " forward";
        }
        return {};
      };
      switch (id) {
        case ActionId::RemoteOpenLocalTerminal:
          if (!hosts->IsRemoteProject(context_.ProjectRoot())) {
            return reject("The active project is not a remote project: Terminal > New is local here");
          }
          if (!hosts->OpenLocalTerminal()) {
            return reject("Cannot open a local terminal");
          }
          return DispatchResult::Handled;
        case ActionId::RemoteOpenFolder: {
          if (args.empty()) {
            context_.OpenRemoteHostPrompt();
            return DispatchResult::Handled;
          }
          std::string error;
          const std::optional<std::filesystem::path> tree = hosts->PrepareRemoteFolder(args[0], &error);
          if (!tree.has_value()) {
            return reject("Cannot open \"" + args[0] + "\": " + error);
          }
          if (!context_.OpenProject(*tree, true, true)) {
            return reject("Cannot open the mirror of \"" + args[0] + "\"");
          }
          return DispatchResult::Handled;
        }
        case ActionId::RemoteResolveConflict: {
          if (args.size() < 2 ||
              (args[1] != "local" && args[1] != "remote" && args[1] != "compare" && args[1] != "merge")) {
            return reject("usage: remote-resolve <path> <local|remote|compare|merge>");
          }
          const bool known = args[1] == "compare" ? hosts->CompareConflict(args[0])
                             : args[1] == "merge" ? hosts->MergeConflict(args[0])
                                                  : hosts->ResolveConflict(args[0], args[1] == "local");
          if (!known) {
            return reject("\"" + args[0] + "\" is not a conflict in a remote project");
          }
          return DispatchResult::Handled;
        }
        case ActionId::RemoteResolveAllConflicts: {
          const std::string& mode = args.empty() ? std::string() : args.back();
          if (args.empty() || args.size() > 2 || (mode != "local" && mode != "remote")) {
            return reject("usage: remote-resolve-all [root] <local|remote>");
          }
          if (args.size() == 1 && !context_.HasProjectRoot()) {
            return reject("No active project");
          }
          const std::filesystem::path root = args.size() == 2 ? std::filesystem::path(args[0]) : context_.ProjectRoot();
          if (!hosts->ResolveAllConflicts(root, mode == "local")) {
            return reject("No conflicts in " + root.string());
          }
          return DispatchResult::Handled;
        }
        case ActionId::RemoteBlockedSave: {
          if (args.size() != 2 || (args[1] != "retry" && args[1] != "replace")) {
            return reject("usage: remote-blocked <path|root> <retry|replace>");
          }
          if (!hosts->SettleBlockedSaves(args[0], args[1] == "replace")) {
            return reject("No save refused by the host at \"" + args[0] + "\"");
          }
          return DispatchResult::Handled;
        }
        case ActionId::RemoteShowConflicts: {
          std::filesystem::path root;
          if (std::string refusal = bring_remote_project_forward(root); !refusal.empty()) {
            return reject(std::move(refusal));
          }
          const SidebarViewSpec* view = FindBuiltinSidebarView(SidebarMode::Conflicts);
          if (view == nullptr ||
              !context_.ShowSidebarView(SidebarViewInfo{.id = view->id, .label = view->label, .mode = view->mode}, {},
                                        {})) {
            return reject("Cannot show the Conflicts view");
          }
          return DispatchResult::Handled;
        }
        case ActionId::RemoteShowLog: {
          const std::string host = host_arg();
          if (host.empty() || !hosts->ShowLog(host)) {
            return reject(host.empty() ? "usage: remote-show-log <host>" : "Not connected to \"" + host + "\"");
          }
          return DispatchResult::Handled;
        }
        case ActionId::RemoteShowStatus: {
          std::string host = args.empty() ? hosts->HostOfProject(context_.ProjectRoot()) : args[0];
          if (host.empty()) {
            host = hosts->SoleHost();
          }
          if (host.empty() || !hosts->ShowHost(host)) {
            return reject(host.empty() ? "usage: remote-status <host>" : "No remote host \"" + host + "\"");
          }
          const SidebarViewSpec* view = FindBuiltinSidebarView(SidebarMode::Host);
          if (view == nullptr ||
              !context_.ShowSidebarView(SidebarViewInfo{.id = view->id, .label = view->label, .mode = view->mode}, {},
                                        {})) {
            return reject("Cannot show the Host view");
          }
          context_.SetCommandFeedback(hosts->StatusText());
          return DispatchResult::Handled;
        }
        case ActionId::RemoteEndHostProcess: {
          const std::optional<std::int64_t> handle = args.size() == 2 ? util::ParseInt64(args[1]) : std::nullopt;
          if (!handle.has_value() || *handle <= 0) {
            return reject("usage: remote-end-process <host> <handle>");
          }
          if (!hosts->EndHostProcess(args[0], static_cast<std::uint64_t>(*handle))) {
            return reject("No remote host \"" + args[0] + "\"");
          }
          return DispatchResult::Handled;
        }
        case ActionId::RemoteStopServer: {
          const std::string host = host_arg();
          if (host.empty() || !hosts->StopServer(host)) {
            return reject(host.empty() ? "usage: remote-stop-server <host>" : "No remote host \"" + host + "\"");
          }
          return DispatchResult::Handled;
        }
        case ActionId::RemoteAttachHostTerminals:
        case ActionId::RemoteCloseHostTerminals: {
          const bool attach = id == ActionId::RemoteAttachHostTerminals;
          std::vector<std::uint64_t> handles;
          for (std::size_t i = 1; i < args.size(); ++i) {
            const std::optional<std::int64_t> handle = util::ParseInt64(args[i]);
            if (!handle.has_value() || *handle <= 0) {
              return reject("Not a terminal handle: \"" + args[i] + "\"");
            }
            handles.push_back(static_cast<std::uint64_t>(*handle));
          }
          if (handles.empty()) {
            return reject(attach ? "usage: remote-attach-terminals <host> <handle>..."
                                 : "usage: remote-close-terminals <host> <handle>...");
          }
          std::string error;
          if (attach ? !hosts->AttachHostTerminals(context_.ProjectRoot(), args[0], handles, &error)
                     : !hosts->CloseHostTerminals(args[0], handles)) {
            return reject(attach ? error : "No remote host \"" + args[0] + "\"");
          }
          return DispatchResult::Handled;
        }
        case ActionId::RemoteDisconnect:
          hosts->Disconnect(args.empty() ? std::string_view() : std::string_view(args[0]));
          return DispatchResult::Handled;
        case ActionId::RemoteReconnect: {
          const std::string host = host_arg();
          if (host.empty() || !hosts->Reconnect(host)) {
            return reject(host.empty() ? "usage: remote-reconnect <host>" : "No remote host \"" + host + "\"");
          }
          return DispatchResult::Handled;
        }
        case ActionId::RemoteCopyCommand: {
          if (args.size() < 2) {
            return reject("usage: remote-copy-command <host> <ssh|install|linger>");
          }
          const std::optional<std::string> text = hosts->CommandText(args[0], args[1]);
          if (!text.has_value() || !context_.WriteClipboardText(*text)) {
            return reject("No remote host \"" + args[0] + "\"");
          }
          context_.Notify(NotificationService::Tone::Info, "Copied: " + *text);
          return DispatchResult::Handled;
        }
        case ActionId::RemoteForwardPort:
        case ActionId::RemoteUnforwardPort: {
          const bool forward = id == ActionId::RemoteForwardPort;
          const std::optional<int> port = args.empty() ? std::nullopt : util::ParseInt(args[0]);
          if (!port.has_value() || *port < 1 || *port > 65535) {
            return reject(forward ? "usage: remote-forward-port <port> [host]"
                                  : "usage: remote-unforward-port <port> [host]");
          }
          std::string host = args.size() > 1 ? args[1] : hosts->HostOfProject(context_.ProjectRoot());
          if (host.empty()) {
            host = hosts->SoleHost();
          }
          const auto port_number = static_cast<std::uint16_t>(*port);
          if (host.empty() &&
              context_.OpenHostPick(
                  forward ? "Forward Port " + std::to_string(*port) + " on"
                          : "Stop Forwarding Port " + std::to_string(*port) + " on",
                  std::string(forward ? "remote-forward-port " : "remote-unforward-port ") + std::to_string(*port))) {
            return DispatchResult::Handled;
          }
          if (host.empty() ||
              !(forward ? hosts->ForwardPort(host, port_number) : hosts->CancelForward(host, port_number))) {
            return reject(host.empty() ? "Name the host: remote-" + std::string(forward ? "" : "un") +
                                             "forward-port <port> <host>"
                                       : "No remote host \"" + host + "\"");
          }
          return DispatchResult::Handled;
        }
        case ActionId::RemoteRemoveMirror: {
          if (args.empty()) {
            return reject("usage: remote-remove-mirror <root> [discard]");
          }
          const bool discard = args.size() > 1 && args[1] == "discard";
          std::string error;
          if (!hosts->RemoveMirror(std::filesystem::path(args[0]), discard, &error)) {
            return reject(error);
          }
          return DispatchResult::Handled;
        }
        case ActionId::RemoteApplyHeldDeletes: {
          if (args.empty() && !context_.HasProjectRoot()) {
            return reject("No active project");
          }
          const std::filesystem::path root = args.empty() ? context_.ProjectRoot() : std::filesystem::path(args[0]);
          if (!hosts->ApplyHeldDeletes(root)) {
            return reject("No held deletes in " + root.string());
          }
          return DispatchResult::Handled;
        }
        case ActionId::RemoteOpenOfferedSession: {
          std::filesystem::path root;
          if (std::string refusal = bring_remote_project_forward(root); !refusal.empty()) {
            return reject(std::move(refusal));
          }
          if (!hosts->OpenOfferedSession(root)) {
            return reject("No other machine has left files open for this project");
          }
          return DispatchResult::Handled;
        }
        case ActionId::RemoteFileHistory: {
          const std::filesystem::path root = context_.ProjectRoot();
          const std::filesystem::path path = context_.ActiveTabPath();
          if (!hosts->IsRemoteProject(root) || path.empty()) {
            return reject("File History is for a file of a remote project");
          }
          if (!args.empty()) {
            if (!hosts->OpenFileVersion(root, path, args[0])) {
              return reject("No such earlier version of " + path.filename().string());
            }
            return DispatchResult::Handled;
          }
          if (!context_.OpenFileHistoryPick(hosts->FileVersions(root, path), path.filename().string())) {
            return reject("No earlier version of " + path.filename().string() +
                          " is kept here: the host has not replaced it since this mirror was made");
          }
          return DispatchResult::Handled;
        }
        case ActionId::RemoteShowHostChanges:
        case ActionId::RemoteClearHostChanges: {
          std::filesystem::path root;
          if (std::string refusal = bring_remote_project_forward(root); !refusal.empty()) {
            return reject(std::move(refusal));
          }
          if (id == ActionId::RemoteClearHostChanges) {
            hosts->ClearHostChanges(root);
            return DispatchResult::Handled;
          }
          const SidebarViewSpec* view = FindBuiltinSidebarView(SidebarMode::HostChanges);
          if (view == nullptr ||
              !context_.ShowSidebarView(SidebarViewInfo{.id = view->id, .label = view->label, .mode = view->mode}, {},
                                        {})) {
            return reject("Cannot show Changed on Host");
          }
          return DispatchResult::Handled;
        }
        default:
          return reject("unreachable");
      }
    }
    default:
      return DispatchResult::Unhandled;
  }
}

}
