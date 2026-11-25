#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "project/remote/RemoteConnection.h"
#include "terminal/TerminalHostChannel.h"
#include "terminal/TerminalSession.h"

namespace microide::project::remote {

class RemoteServerClient;

class RemoteTerminalChannel final : public terminal::TerminalHostChannel,
                                    public Reattachable,
                                    public std::enable_shared_from_this<RemoteTerminalChannel> {
 public:
  using HostToLocal = std::function<std::filesystem::path(const std::filesystem::path&)>;

  static std::shared_ptr<RemoteTerminalChannel> Open(
      std::shared_ptr<RemoteServerClient> client,
      const terminal::HostTerminalSource::OpenRequest& request, terminal::TerminalSession& session,
      HostToLocal host_to_local);

  static std::shared_ptr<RemoteTerminalChannel> Attach(
      std::shared_ptr<RemoteServerClient> client, std::uint64_t handle,
      terminal::TerminalSession& session, HostToLocal host_to_local,
      std::optional<terminal::TerminalSession::HostResumePoint> resume = {});

  void Reattach(std::shared_ptr<RemoteServerClient> client) override;

  RemoteTerminalChannel(std::shared_ptr<RemoteServerClient> client,
                        terminal::TerminalSession& session, HostToLocal host_to_local);
  ~RemoteTerminalChannel() override;

  void Send(const terminal::TerminalInputEvent& event) override;
  void Resize(std::size_t rows, std::size_t columns) override;
  void Close() override;
  void Detach() override;
  terminal::HostIdentity identity() const override;
  terminal::HostReattach reattach() const override;
  std::optional<std::chrono::milliseconds> RoundTrip() const override;
  bool FetchHistory(std::uint64_t before, std::size_t count, HistoryDone done) override;

  std::uint64_t handle() const override;
  std::uint64_t received_bytes() const;

 private:
  void Attached(const std::shared_ptr<RemoteServerClient>& client, std::uint64_t handle,
                std::size_t credit_bytes, bool reattach);
  bool SendAttach(const std::shared_ptr<RemoteServerClient>& client, std::uint64_t handle,
                  const std::optional<terminal::TerminalSession::HostResumePoint>& resume,
                  bool reattach);
  bool SendOpen(const std::shared_ptr<RemoteServerClient>& client,
                const terminal::HostTerminalSource::OpenRequest& request);
  bool FallBackToOpen(const std::shared_ptr<RemoteServerClient>& client);
  void Ended(terminal::HostReattach outcome, const std::string& reason);
  void OpenFailed(const std::string& error);
  void ApplyFrame(std::string_view bytes);
  static void SendResize(const std::shared_ptr<RemoteServerClient>& client, std::uint64_t handle,
                         std::size_t rows, std::size_t columns);
  std::shared_ptr<RemoteServerClient> Client() const;

  HostToLocal host_to_local_;

  mutable std::mutex mutex_;
  std::shared_ptr<RemoteServerClient> client_;
  terminal::TerminalSession* session_ = nullptr;
  std::uint64_t handle_ = 0;
  bool attached_ = false;
  std::string pending_input_;
  std::size_t sent_rows_ = 0;
  std::size_t sent_columns_ = 0;
  std::size_t pending_rows_ = 0;
  std::size_t pending_columns_ = 0;
  std::size_t credit_bytes_ = 256 * 1024;
  std::uint64_t received_bytes_ = 0;
  std::uint64_t acked_bytes_ = 0;
  bool reattaching_ = false;
  bool detached_ = false;
  terminal::HostReattach reattach_state_ = terminal::HostReattach::None;
  std::optional<terminal::HostTerminalSource::OpenRequest> fallback_;
};

}
