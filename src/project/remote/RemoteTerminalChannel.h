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

// The client's end of one host terminal over a RemoteServerClient: what a
// TerminalSession in host mode sends through, and what applies the host's frames
// to it (dev-docs/design/remote-projects.md § 6.5).
//
// The open is asynchronous. Starting a terminal must not cost the UI thread a
// round trip to a far host, so `term/open` is sent and Open returns at once;
// input and size changes made before the reply are queued and flushed when it
// lands, and the session shows its first frame when that arrives.
class RemoteTerminalChannel final : public terminal::TerminalHostChannel,
                                    public Reattachable,
                                    public std::enable_shared_from_this<RemoteTerminalChannel> {
 public:
  using HostToLocal = std::function<std::filesystem::path(const std::filesystem::path&)>;

  // `request.working_directory` is already a HOST path. `host_to_local` maps the
  // shell's OSC 7 reports back into the editor's tree.
  static std::shared_ptr<RemoteTerminalChannel> Open(
      std::shared_ptr<RemoteServerClient> client,
      const terminal::HostTerminalSource::OpenRequest& request, terminal::TerminalSession& session,
      HostToLocal host_to_local);

  // Take over host terminal `handle` — a reattach after a dropped link, or from
  // another machine. Without `resume` the first frame is a cold one (the screen
  // plus prefetched history; the session's contents are replaced). With it — the
  // session's own host_resume_point() — the session keeps its scrollback and gets
  // only what the host printed since.
  static std::shared_ptr<RemoteTerminalChannel> Attach(
      std::shared_ptr<RemoteServerClient> client, std::uint64_t handle,
      terminal::TerminalSession& session, HostToLocal host_to_local,
      std::optional<terminal::TerminalSession::HostResumePoint> resume = {});

  // Carry on over `client` (a reconnect): a warm reattach from the session's own
  // resume point. Input typed meanwhile is held and sent once it is answered.
  void Reattach(std::shared_ptr<RemoteServerClient> client) override;

  RemoteTerminalChannel(std::shared_ptr<RemoteServerClient> client,
                        terminal::TerminalSession& session, HostToLocal host_to_local);
  ~RemoteTerminalChannel() override;

  void Send(const terminal::TerminalInputEvent& event) override;
  void Resize(std::size_t rows, std::size_t columns) override;
  void Close() override;
  std::optional<std::chrono::milliseconds> RoundTrip() const override;

  // The host's handle; 0 until term/open answers.
  std::uint64_t handle() const;

 private:
  void Attached(const std::shared_ptr<RemoteServerClient>& client, std::uint64_t handle,
                std::size_t credit_bytes, bool reattach);
  bool SendAttach(const std::shared_ptr<RemoteServerClient>& client, std::uint64_t handle,
                  const std::optional<terminal::TerminalSession::HostResumePoint>& resume,
                  bool reattach);
  void OpenFailed(const std::string& error);
  void ApplyFrame(std::string_view bytes);
  static void SendResize(const std::shared_ptr<RemoteServerClient>& client, std::uint64_t handle,
                         std::size_t rows, std::size_t columns);
  std::shared_ptr<RemoteServerClient> Client() const;

  HostToLocal host_to_local_;

  // Guards everything below. Held while a frame is applied, so Close returning
  // means no frame is touching the session.
  mutable std::mutex mutex_;
  std::shared_ptr<RemoteServerClient> client_;   // replaced by a reconnect
  terminal::TerminalSession* session_ = nullptr;  // null once closed
  std::uint64_t handle_ = 0;                      // 0 until term/open answers
  bool attached_ = false;                         // the open/attach was answered on client_
  std::string pending_input_;                     // encoded events before that
  std::size_t pending_rows_ = 0;
  std::size_t pending_columns_ = 0;
  std::size_t credit_bytes_ = 256 * 1024;
  std::uint64_t received_bytes_ = 0;
  std::uint64_t acked_bytes_ = 0;
  bool reattaching_ = false;
};

}  // namespace microide::project::remote
