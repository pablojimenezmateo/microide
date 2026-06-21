#pragma once

#include "platform/AsyncSubprocess.h"
#include "util/JsonValue.h"
#include "workspace/DapProtocol.h"

#include <SDL3/SDL.h>

#include <functional>
#include <string>
#include <vector>

namespace microide::workspace {

// Single Debug Adapter Protocol connection over stdio. Mirrors LspClient: the
// transport is JSON with `Content-Length` framing, a dedicated I/O thread blocks
// in poll() over stdout + a self-pipe wake, and results are delivered on the main
// thread through DrainCallbacks(). Call SetWakeEventType() once before Start() so
// the I/O thread can wake the main event loop when messages are ready.
//
// This class owns only the transport and the initialize handshake. Higher-level
// orchestration (launch/attach sequencing, breakpoints, stepping, variables) is
// built on top of SendRequestAsync() / the event callback by DebugSession.
class DapClient {
 public:
  // Delivered on the main thread. `response` carries success/body/message.
  using ResponseCallback = std::function<void(const dap_protocol::DapResponse& response)>;
  // Delivered on the main thread for every adapter-initiated event.
  using EventCallback =
      std::function<void(const std::string& event, const util::JsonValue& body)>;

  DapClient();
  ~DapClient();
  DapClient(const DapClient&) = delete;
  DapClient& operator=(const DapClient&) = delete;

  // SDL custom event type used to wake the main loop when callbacks are ready.
  // Call before Start().
  void SetWakeEventType(Uint32 event_type);

  // Install the adapter-event sink. Call before Start().
  void SetEventCallback(EventCallback callback);

  // Spawn the adapter and begin the asynchronous `initialize` handshake.
  // `adapter_id` is forwarded as the DAP `adapterID` and used in traces.
  bool Start(const std::vector<std::string>& command, const std::string& adapter_id,
             const std::string& cwd = {}, const platform::SubprocessSandbox& sandbox = {});

  // True while the adapter process is running.
  bool IsRunning() const;

  // True while the initialize handshake is in flight.
  bool IsInitializing() const;

  // True once the `initialize` response has been received.
  bool IsInitialized() const;

  // Capabilities reported by the adapter's initialize response (snapshot copy).
  dap_protocol::DapCapabilities Capabilities() const;

  // Apply a DAP `capabilities`-event update. `capabilities_body` is the event's
  // inner `capabilities` object (a partial Capabilities). Merges under the same
  // lock that guards Capabilities() so a live UI read never tears.
  void ApplyCapabilitiesUpdate(const util::JsonValue& capabilities_body);

  // Last startup/runtime error captured by the client.
  const std::string& LastError() const;

  // Call from the main thread each frame to dispatch pending callbacks/events.
  void DrainCallbacks();

  // True when a request is in flight (awaiting a response) or a response has
  // arrived but not yet been drained. The idle loop uses this to poll on a short
  // interval so an async response is delivered promptly even when otherwise fully
  // idle (a blocking wait would otherwise rely solely on a cross-thread wake).
  bool HasPendingRequests() const;

  // Send a DAP request. `arguments` is forwarded verbatim (Null to omit). The
  // response is delivered to `callback` on the main thread. Requests sent before
  // the initialize response are deferred and flushed once initialized. Returns
  // false only when the request can never be sent (client shutting down / dead).
  bool SendRequestAsync(const std::string& command, util::JsonValue arguments,
                        ResponseCallback callback);

  // Unit tests: behave as a connected adapter without spawning a subprocess.
  void EnableTestStubMode();
  void DisableTestStubMode();
  // Answer requests in stub mode. The handler invokes the supplied callback with
  // a synthesized response (immediately or later) — wired through DrainCallbacks.
  void SetTestRequestHandler(
      std::function<void(const std::string& command, const util::JsonValue& arguments,
                         ResponseCallback)> handler);
  // Inject an adapter event in stub mode (delivered via DrainCallbacks).
  void InjectTestEvent(const std::string& event, util::JsonValue body);

  // Shutdown: send `disconnect` and close the connection.
  void BeginShutdown();
  void Shutdown();
  bool IsShuttingDown() const;
  bool IsShutdownComplete() const;

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace microide::workspace
