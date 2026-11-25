#pragma once

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "project/remote/RemoteConnection.h"

namespace microide::project::remote {

class RemoteServerClient;

struct RemoteHostTarget {
  std::string user;
  std::string host;
  int port = 0;

  std::string Display() const;
};
std::optional<RemoteHostTarget> ParseRemoteHostTarget(std::string_view text, std::string* error);

std::vector<std::string> SshConfigHosts(const std::filesystem::path& config_file,
                                        const std::filesystem::path& ssh_directory);

class HostPortForwards {
 public:
  struct PortForward {
    std::uint16_t host_port = 0;
    std::uint16_t local_port = 0;
    friend bool operator==(const PortForward&, const PortForward&) = default;
  };

  std::vector<PortForward> list() const;
  bool empty() const;
  std::optional<PortForward> Find(std::uint16_t host_port) const;
  void Add(PortForward forward);
  std::optional<PortForward> Remove(std::uint16_t host_port);
  void Hold(const void* session);
  bool Release(const void* session);

 private:
  mutable std::mutex mutex_;
  std::vector<PortForward> forwards_;
  std::vector<const void*> holders_;
};

class RemoteHostSession {
 public:
  enum class State : std::uint8_t {
    Disconnected,
    Connecting,
    NeedsAuth,
    StartingServer,
    Installing,
    Ready,
    Reconnecting,
    Offline,
  };
  static std::string_view StateName(State state);

  struct Config {
    RemoteHostTarget target;
    std::vector<std::string> ssh = {"ssh"};
    std::filesystem::path control_dir;
    std::string server_command;
    std::string server_socket_dir;
    std::int64_t backfill_inflight_bytes = 0;
    std::int64_t max_manifest_files = 0;
    bool install = true;
    std::function<std::filesystem::path(std::string_view arch)> server_binary;
    std::string release;
    std::string workspace_root;
    std::chrono::milliseconds min_backoff{1000};
    std::chrono::milliseconds max_backoff{30000};
    std::chrono::milliseconds auth_poll{500};
    std::chrono::milliseconds auth_timeout{5 * 60 * 1000};
    std::shared_ptr<HostPortForwards> forwards;
  };

  struct Status {
    State state = State::Disconnected;
    std::string message;
    std::string error;
  };
  using Listener = std::function<void(const Status&)>;

  RemoteHostSession(Config config, Listener listener);
  ~RemoteHostSession();
  RemoteHostSession(const RemoteHostSession&) = delete;
  RemoteHostSession& operator=(const RemoteHostSession&) = delete;

  void Connect();
  void Disconnect();
  Status status() const;
  const Config& config() const { return config_; }
  const std::shared_ptr<RemoteConnection>& connection() const { return connection_; }

  std::filesystem::path control_path() const;
  std::vector<std::string> InteractiveMasterArgv() const;
  std::string SshCommandText() const;
  std::string InstallCommandText() const;
  std::string LingerCommandText() const;
  bool StopServer(std::string* error);
  bool update_installed() const { return update_installed_.load(); }

  using PortForward = HostPortForwards::PortForward;
  std::optional<PortForward> ForwardPort(std::uint16_t host_port, std::string* error);
  bool CancelForward(std::uint16_t host_port, std::string* error);
  std::vector<PortForward> forwards() const;

 private:
  enum class Wake : std::uint8_t { None, Connect, Disconnect, LinkDied, Stop };

  void Run();
  bool Attempt(bool reconnecting);
  bool MasterAlive();
  bool BringUpMaster(bool reconnecting);
  std::shared_ptr<RemoteServerClient> StartServer(bool* not_installed, bool* incompatible,
                                                  std::string* error);
  bool Install(std::string* error);
  std::string HostArchitecture();
  void Report(State state, std::string message, std::string error = {});
  bool WaitFor(std::chrono::milliseconds delay);

  std::vector<std::string> SshArgv(bool batch) const;
  std::vector<std::string> MasterArgv(bool batch) const;
  std::vector<std::string> RemoteArgv(std::string command) const;
  std::string AttachCommand() const;
  bool ForwardControl(std::string_view verb, const PortForward& forward, std::string* error);
  void ReapplyForwards();
  std::string ServerCommand(std::string_view verb) const;

  struct LinkSignal {
    std::mutex mutex;
    RemoteHostSession* session = nullptr;
  };
  void LinkDied();

  Config config_;
  Listener listener_;
  std::atomic_bool update_installed_{false};
  std::string host_architecture_;
  std::shared_ptr<LinkSignal> link_signal_ = std::make_shared<LinkSignal>();
  std::shared_ptr<RemoteConnection> connection_ = std::make_shared<RemoteConnection>();

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  Wake pending_ = Wake::None;
  Status status_;
  std::thread thread_;
};

}
