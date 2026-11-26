#pragma once

#include "platform/AsyncSubprocess.h"
#include "platform/ProcessLauncher.h"
#include "util/JsonValue.h"
#include "workspace/debug/DapProtocol.h"

#include <SDL3/SDL.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace microide::workspace {

class DapClient {
 public:
  using ResponseCallback = std::function<void(const dap_protocol::DapResponse& response)>;
  using EventCallback =
      std::function<void(const std::string& event, const util::JsonValue& body)>;

  DapClient();
  ~DapClient();
  DapClient(const DapClient&) = delete;
  DapClient& operator=(const DapClient&) = delete;

  void SetWakeChannel(Uint32 event_type);

  void SetEventCallback(EventCallback callback);

  bool Start(const platform::ProcessLauncher& launcher, const std::vector<std::string>& command,
             const std::string& adapter_id,
             const std::string& cwd = {}, const platform::SubprocessSandbox& sandbox = {});

  bool IsRunning() const;

  bool IsInitializing() const;

  bool IsInitialized() const;

  dap_protocol::DapCapabilities Capabilities() const;

  void ApplyCapabilitiesUpdate(const util::JsonValue& capabilities_body);

  std::string LastError() const;

  void DrainCallbacks();

  bool HasPendingRequests() const;
  std::size_t PendingRequestCount() const;

  bool SendRequestAsync(const std::string& command, util::JsonValue arguments,
                        ResponseCallback callback);

  void EnableTestStubMode();
  void SetTerminateDebuggeeOnDisconnect(bool terminate);
  void SetTestRequestHandler(
      std::function<void(const std::string& command, const util::JsonValue& arguments,
                         ResponseCallback)> handler);
  void InjectTestEvent(const std::string& event, util::JsonValue body);

  void BeginShutdown();
  void Shutdown();
  bool IsShuttingDown() const;
  bool IsShutdownComplete() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}
