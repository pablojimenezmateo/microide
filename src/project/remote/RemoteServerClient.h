#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/AsyncSubprocess.h"
#include "project/GitMetadataSource.h"
#include "project/remote/RemoteFrame.h"
#include "project/remote/RemotePeer.h"
#include "project/remote/RemoteProtocol.h"

namespace microide::project::remote {

class RemoteServerClient {
 public:
  struct ProcessEvents {
    std::function<void(FrameType stream, std::string_view bytes)> output;
    std::function<void(std::optional<int> code, std::optional<int> signal)> exit;
  };

  struct Spawned {
    std::uint64_t handle = 0;
    int pid = -1;
    std::string error;
  };

  struct GitMetadata {
    bool repository = false;
    std::string git_dir;
    bool operator==(const GitMetadata&) const = default;
  };

  static std::shared_ptr<RemoteServerClient> Create();
  ~RemoteServerClient();
  RemoteServerClient(const RemoteServerClient&) = delete;
  RemoteServerClient& operator=(const RemoteServerClient&) = delete;

  bool ConnectCommand(const std::vector<std::string>& argv, const HelloRequest& hello,
                      std::string* error);
  bool ConnectFds(int read_fd, int write_fd, const HelloRequest& hello, std::string* error);

  bool connected() const { return connected_ && !peer_.closed(); }
  bool incompatible() const { return incompatible_; }
  std::optional<int> command_exit_code(std::chrono::milliseconds wait);
  void SetClosedHandler(std::function<void(std::string_view reason)> handler) {
    closed_handler_ = std::move(handler);
  }
  const HelloReply& hello() const { return hello_reply_; }
  RemotePeer& peer() { return peer_; }

  Spawned Spawn(const std::vector<std::string>& argv, const std::filesystem::path& host_cwd,
                const std::vector<std::pair<std::string, std::optional<std::string>>>& env,
                bool keep_on_detach, ProcessEvents events);
  void RegisterProcess(std::uint64_t handle, ProcessEvents events);
  bool AttachProcess(std::uint64_t handle, std::uint64_t stdout_offset,
                     std::uint64_t stderr_offset, std::string* error);
  bool WriteStdin(std::uint64_t handle, std::string_view bytes);
  bool CloseStdin(std::uint64_t handle);
  bool Signal(std::uint64_t handle, std::string_view signal);
  void Ack(std::uint64_t handle, std::uint64_t stdout_offset, std::uint64_t stderr_offset);
  void Release(std::uint64_t handle);

  std::optional<GitMetadata> QueryGitMetadata(const std::filesystem::path& host_root);
  std::optional<ServerStatus> Status(std::string* error, bool activity = false);
  using ActivityHandler = std::function<void(const ServerStatus& status)>;
  std::optional<ServerStatus> SubscribeActivity(ActivityHandler changed, std::string* error);
  void UnsubscribeActivity();

  bool SubscribeGitStatus(std::function<void(const std::string& host_root)> changed,
                          std::string* error);
  std::optional<project::GitStatusOutput> CurrentGitStatus(const std::string& host_root);
  std::uint64_t last_git_generation() const { return last_git_generation_.load(); }
  std::optional<GitMetadata> PushedGitMetadata(const std::string& host_root);
  struct HostPorts {
    std::vector<std::uint16_t> ports;
    std::map<std::uint16_t, std::string> roots;
  };
  using PortsHandler = std::function<void(const HostPorts& ports)>;
  std::optional<HostPorts> SubscribePorts(PortsHandler changed, std::string* error);
  void BeginGitMutation();
  void EndGitMutation();

  std::optional<util::JsonValue> RunSearch(const util::JsonValue& params,
                                           std::function<void(const util::JsonValue&)> on_results,
                                           const std::function<bool()>& cancelled,
                                           std::string* error);

  struct TerminalEvents {
    std::function<void(std::string_view frame)> frame;
    std::function<void(std::string_view reason)> lost;
  };
  void RegisterTerminal(std::uint64_t handle, std::shared_ptr<TerminalEvents> events);
  void UnregisterTerminal(std::uint64_t handle);

  using StreamContent = std::function<void(FrameType type, std::string bytes)>;
  std::uint64_t RequestStream(std::string_view method, const util::JsonValue& params, Lane lane,
                              StreamContent content, RemotePeer::ResponseHandler done);

  void SetWatchMovedHandler(std::function<void(std::uint64_t manifest_id)> handler) {
    std::lock_guard lock(mutex_);
    watch_moved_handler_ = std::make_shared<std::function<void(std::uint64_t)>>(std::move(handler));
  }
  void SetWatchHandler(std::function<void(std::uint64_t manifest_id, std::string bytes)> handler) {
    std::lock_guard lock(mutex_);
    watch_handler_ = std::make_shared<std::function<void(std::uint64_t, std::string)>>(std::move(handler));
  }

  std::optional<util::JsonValue> Call(std::string_view method, const util::JsonValue& params,
                                      std::string* error,
                                      std::chrono::milliseconds timeout = std::chrono::seconds(30));

 private:
  RemoteServerClient() = default;

  struct Orphan {
    std::vector<std::pair<FrameType, std::string>> output;
    std::optional<util::JsonValue> exit;
    std::size_t bytes = 0;
  };

  void InstallRouting();
  bool Handshake(const HelloRequest& hello, std::string* error);
  void Deliver(std::uint64_t handle, FrameType type, std::string bytes);
  void DeliverExit(std::uint64_t handle, const util::JsonValue& params);
  void DeliverTerminalFrame(std::uint64_t handle, std::string bytes);

  platform::AsyncSubprocess command_;
  int command_stderr_fd_ = -1;
  void AppendCommandStderr(std::string* error);
  RemotePeer peer_;
  std::atomic<bool> connected_{false};
  bool incompatible_ = false;
  std::function<void(std::string_view)> closed_handler_;
  HelloReply hello_reply_;

  std::mutex mutex_;
  std::map<std::uint64_t, std::shared_ptr<ProcessEvents>> processes_;
  std::map<std::uint64_t, Orphan> orphans_;
  std::map<std::uint64_t, std::shared_ptr<TerminalEvents>> terminals_;
  std::map<std::uint64_t, Orphan> terminal_orphans_;
  std::map<std::uint64_t, std::shared_ptr<StreamContent>> streams_;
  std::shared_ptr<std::function<void(std::uint64_t, std::string)>> watch_handler_;
  std::shared_ptr<std::function<void(std::uint64_t)>> watch_moved_handler_;
  struct PushedGitStatus {
    project::GitStatusOutput status;
    std::uint64_t generation = 0;
    std::optional<GitMetadata> metadata;
  };
  std::map<std::string, PushedGitStatus, std::less<>> pushed_git_;
  std::map<std::uint64_t, std::shared_ptr<std::function<void(const util::JsonValue&)>>> search_routes_;
  std::shared_ptr<std::function<void(const std::string&)>> git_status_handler_;
  std::shared_ptr<PortsHandler> ports_handler_;
  std::shared_ptr<ActivityHandler> activity_handler_;
  int git_mutations_in_flight_ = 0;
  std::uint64_t git_required_generation_ = 0;
  std::atomic<std::uint64_t> last_git_generation_{0};
};

}
