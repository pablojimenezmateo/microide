#pragma once

#include "workspace/PendingRequestTable.h"
#include "workspace/debug/DapProtocol.h"
#include "workspace/JsonRpcMessageFraming.h"
#include "workspace/debug/WorkspaceDapClient.h"
#include "workspace/ProtocolNumeric.h"
#include "workspace/StdioClientQueue.h"
#include "workspace/StdioJsonRpcClientTransport.h"
#include "util/DebugTrace.h"
#include "util/MainThreadMailbox.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/WakePipe.h"
#include "util/ThreadName.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace microide::workspace {

namespace {

constexpr std::chrono::milliseconds kStartupRequestTimeout{30000};
constexpr std::chrono::milliseconds kInteractiveRequestTimeout{6000};

inline std::chrono::milliseconds RequestTimeoutFor(const std::string& command) {
  if (command == "launch" || command == "attach" || command == "initialize" ||
      command == "configurationDone" || command == "restart") {
    return kStartupRequestTimeout;
  }
  return kInteractiveRequestTimeout;
}

inline bool DapResponseSeqInRange(const util::JsonValue& seq_value, int* out) {
  if (!protocol_numeric::IsIntegralJsonNumber(seq_value)) {
    return false;
  }
  const std::int64_t raw = seq_value.AsInt();
  if (raw < static_cast<std::int64_t>(std::numeric_limits<int>::min()) ||
      raw > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  *out = static_cast<int>(raw);
  return true;
}

constexpr std::size_t kMaxQueuedMessages = 50000;
constexpr std::size_t kMaxQueuedBytes = 512u * 1024 * 1024;

constexpr std::size_t kMaxDapMessageBytes = 64ull * 1024 * 1024;
constexpr std::size_t kMaxDapReadBufferBytes = kMaxDapMessageBytes + (1ull * 1024 * 1024);
constexpr std::size_t kMaxDapEarlyMessages = 10000;

bool DapLifecycleTraceEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("MICROIDE_TRACE_DAP_LIFECYCLE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  return enabled;
}

void TraceDapLifecycle(std::string_view adapter_id, int pid, std::string_view phase,
                       std::string_view detail = {}) {
  if (!DapLifecycleTraceEnabled()) {
    return;
  }
  if (detail.empty()) {
    std::fprintf(stderr, "[dap] %.*s pid=%d %.*s\n", static_cast<int>(adapter_id.size()),
                 adapter_id.data(), pid, static_cast<int>(phase.size()), phase.data());
    return;
  }
  std::fprintf(stderr, "[dap] %.*s pid=%d %.*s | %.*s\n", static_cast<int>(adapter_id.size()),
               adapter_id.data(), pid, static_cast<int>(phase.size()), phase.data(),
               static_cast<int>(detail.size()), detail.data());
}

}

struct DapClient::Impl : StdioJsonRpcClientTransport<DapClient::Impl> {
  Impl()
      : StdioJsonRpcClientTransport(StdioTransportConfig{
            .messages_sent = util::PerfCounterId::DapMessagesSent,
            .bytes_sent = util::PerfCounterId::DapBytesSent,
            .bytes_received = util::PerfCounterId::DapBytesReceived,
            .max_message_bytes = kMaxDapMessageBytes,
            .max_read_buffer_bytes = kMaxDapReadBufferBytes,
            .max_queued_messages = kMaxQueuedMessages,
            .max_queued_bytes = kMaxQueuedBytes,
            .peer_label = "dap",
            .send_error = "failed to send message to debug adapter",
        }) {}

  void TraceSend(const util::JsonValue& msg) const { util::DebugTrace::Message("send", msg); }
  void TraceLifecycle(std::string_view phase, std::string_view detail = {}) {
    TraceDapLifecycle(adapter_id, proc.pid(), phase, detail);
  }
  void WakeMainThread() { main_mailbox.PushWake(); }

  std::mutex mutex;

  std::atomic<bool> terminate_debuggee_on_disconnect{false};
  std::condition_variable shutdown_cv;
  bool shutdown_response_received = false;
  int shutdown_request_seq = 0;

  PendingRequestTable<std::function<void(util::JsonValue)>> pending_requests;
  util::MainThreadMailbox main_mailbox;

  std::function<void(const std::string&, const util::JsonValue&, ResponseCallback)>
      test_request_handler;

  EventCallback event_callback;

  int next_seq = 1;
  std::string adapter_id;
  std::string last_error;
  dap_protocol::DapCapabilities capabilities;

  void SetLastError(std::string message) {
    std::lock_guard lock(mutex);
    last_error = std::move(message);
  }

  int GetNextSeq() {
    std::lock_guard lock(mutex);
    return next_seq++;
  }

  void ResetProtocolState() {
    std::lock_guard lock(mutex);
    pending_requests.clear();
    main_mailbox.Clear();
    shutdown_response_received = false;
    shutdown_request_seq = 0;
  }

  void DispatchMessage(util::JsonValue msg) {
    util::PerformanceTrace::Scope perf_scope("dap::DispatchMessage");
    util::AddPerformanceCounter(util::PerfCounterId::DapMessagesReceived);
    util::DebugTrace::Message("recv", msg);
    const std::string& type = msg["type"].AsString();
    if (type == "response") {
      HandleResponse(std::move(msg));
    } else if (type == "event") {
      HandleEvent(std::move(msg));
    } else if (type == "request") {
      HandleReverseRequest(std::move(msg));
    }
  }

  void HandleResponse(util::JsonValue msg) {
    int request_seq = 0;
    if (!DapResponseSeqInRange(msg["request_seq"], &request_seq)) {
      return;
    }
    if (shutting_down.load(std::memory_order_acquire)) {
      std::lock_guard lock(mutex);
      if (request_seq == shutdown_request_seq) {
        shutdown_response_received = true;
        shutdown_cv.notify_all();
      }
      return;
    }
    std::function<void(util::JsonValue)> cb;
    {
      std::lock_guard lock(mutex);
      if (auto entry = pending_requests.Take(request_seq)) {
        cb = std::move(entry->callback);
      }
    }
    if (!cb) {
      return;
    }
    main_mailbox.Post([cb = std::move(cb), m = std::move(msg)]() mutable { cb(std::move(m)); });
  }

  void HandleEvent(util::JsonValue msg) {
    std::string event = msg["event"].AsString();
    util::JsonValue body = msg["body"];
    EventCallback cb;
    {
      std::lock_guard lock(mutex);
      cb = event_callback;
    }
    if (cb) {
      main_mailbox.Post(
          [cb = std::move(cb), e = std::move(event), b = std::move(body)]() mutable { cb(e, b); });
    }
  }

  void HandleReverseRequest(util::JsonValue msg) {
    int request_seq = 0;
    if (!DapResponseSeqInRange(msg["seq"], &request_seq)) {
      return;
    }
    const std::string& command = msg["command"].AsString();
    const int seq = GetNextSeq();
    SendMessageAfterInitialize(dap_protocol::MakeResponse(
        seq, request_seq, command, false, "unsupported reverse request: " + command,
        util::JsonValue(nullptr)));
  }

  int RegisterPendingRequest(std::function<void(util::JsonValue)> cb,
                             std::chrono::milliseconds timeout) {
    std::lock_guard lock(mutex);
    const int seq = next_seq++;
    pending_requests.Add(seq, std::move(cb), timeout);
    return seq;
  }

  bool RemovePendingRequest(int seq) {
    std::lock_guard lock(mutex);
    return pending_requests.Remove(seq);
  }

  void FailPendingRequests(bool only_expired) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(mutex);
    const std::size_t failed = pending_requests.Sweep(only_expired, now, [&](int seq, auto entry) {
      util::DebugTrace::Note("dap", only_expired ? "request-timeout seq" : "adapter-gone seq",
                             static_cast<long long>(seq));
      util::JsonValue envelope = dap_protocol::MakeResponse(
          0, 0, std::string{}, false, "debug adapter did not respond", util::JsonValue(nullptr));
      main_mailbox.PostWithoutWake([cb = std::move(entry.callback),
                                    envelope = std::move(envelope)]() mutable { cb(std::move(envelope)); });
    });
    if (failed > 0) {
      main_mailbox.PushWake();
    }
  }

  bool DispatchRequest(const std::string& command, util::JsonValue arguments,
                       std::function<void(util::JsonValue)> response_handler,
                       std::function<void()> on_send_failure) {
    if (test_stub_mode.load(std::memory_order_acquire)) {
      return DispatchRequestStub(command, std::move(arguments), std::move(response_handler),
                                 std::move(on_send_failure));
    }
    const int seq = RegisterPendingRequest(std::move(response_handler), RequestTimeoutFor(command));
    if (!SendMessageAfterInitialize(
            dap_protocol::MakeRequest(seq, command, std::move(arguments)))) {
      if (RemovePendingRequest(seq)) {
        on_send_failure();
      }
      return false;
    }
    return true;
  }

  bool DispatchRequestStub(const std::string& command, util::JsonValue arguments,
                           std::function<void(util::JsonValue)> response_handler,
                           std::function<void()> on_send_failure) {
    std::function<void(const std::string&, const util::JsonValue&, ResponseCallback)> handler;
    {
      std::lock_guard lock(mutex);
      handler = test_request_handler;
    }
    if (!handler) {
      on_send_failure();
      return false;
    }
    ResponseCallback bridge = [this, command,
                               response_handler = std::move(response_handler)](
                                  const dap_protocol::DapResponse& response) {
      util::JsonValue envelope = dap_protocol::MakeResponse(0, 0, command, response.success,
                                                            response.message, response.body);
      auto handler_copy = response_handler;
      main_mailbox.Post(
          [handler_copy = std::move(handler_copy), envelope = std::move(envelope)]() mutable {
            handler_copy(std::move(envelope));
          });
    };
    handler(command, arguments, std::move(bridge));
    return true;
  }

  void DoInitializeBlocking() {
    initializing.store(true, std::memory_order_release);

    using namespace util;
    JsonObject args;
    args["clientID"] = JsonValue("microide");
    args["clientName"] = JsonValue("microide");
    args["adapterID"] = JsonValue(adapter_id);
    args["locale"] = JsonValue("en-US");
    args["linesStartAt1"] = JsonValue(true);
    args["columnsStartAt1"] = JsonValue(true);
    args["pathFormat"] = JsonValue("path");
    args["supportsVariableType"] = JsonValue(true);
    args["supportsVariablePaging"] = JsonValue(true);
    args["supportsRunInTerminalRequest"] = JsonValue(false);
    args["supportsMemoryReferences"] = JsonValue(false);
    args["supportsProgressReporting"] = JsonValue(false);
    args["supportsInvalidatedEvent"] = JsonValue(false);

    const int init_seq = GetNextSeq();
    const auto req = dap_protocol::MakeRequest(init_seq, "initialize", JsonValue(std::move(args)));
    if (!SendMessageImmediate(req)) {
      SetLastError("failed to send initialize request to debug adapter");
      AbandonInitialize();
      return;
    }

    bool got_init = false;
    std::vector<util::JsonValue> early_messages;
    const auto init_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < init_deadline) {
      if (stop_init.load(std::memory_order_acquire)) {
        AbandonInitialize();
        return;
      }
      auto msg_opt = framer_.Next();
      if (!msg_opt) {
        auto chunk = proc.Read(4096, 500);
        if (!chunk) {
          break;
        }
        if (!chunk->empty()) {
          framer_.Append(*chunk);
        }
        if (framer_.BufferedBytes() > kMaxDapReadBufferBytes) {
          break;
        }
        continue;
      }
      const util::JsonValue& msg = *msg_opt;
      const std::string& type = msg["type"].AsString();
      int init_response_seq = 0;
      const bool is_init_response =
          type == "response" && DapResponseSeqInRange(msg["request_seq"], &init_response_seq) &&
          init_response_seq == init_seq;
      if (!is_init_response) {
        early_messages.push_back(std::move(*msg_opt));
        if (early_messages.size() > kMaxDapEarlyMessages) {
          SetLastError("debug adapter emitted too many events before initialize response");
          AbandonInitialize();
          return;
        }
        continue;
      }
      if (!msg["success"].AsBool(false)) {
        const util::JsonValue& message_val = msg["message"];
        const std::string message =
            message_val.IsString() ? message_val.AsString() : "initialize failed";
        SetLastError("debug adapter rejected initialize: " + message);
        AbandonInitialize();
        return;
      }
      {
        std::lock_guard lock(mutex);
        capabilities = dap_protocol::ParseCapabilities(msg["body"]);
      }
      got_init = true;
      break;
    }

    if (!got_init) {
      const std::optional<int> exit_code = proc.exit_code();
      if (exit_code.has_value()) {
        SetLastError("debug adapter exited before initialize response (exit code " +
                     std::to_string(*exit_code) + ")");
      } else {
        SetLastError("timed out waiting for initialize response from debug adapter");
      }
      AbandonInitialize();
      return;
    }

    {
      std::lock_guard lock(send_mutex);
      initialized.store(true, std::memory_order_release);
      for (QueuedMessage& message : deferred_messages) {
        outbound_messages.push_back(std::move(message));
      }
      deferred_messages.clear();
    }

    for (util::JsonValue& message : early_messages) {
      DispatchMessage(std::move(message));
    }
    early_messages.clear();

    OpenWakePipe();
    stop_io.store(false, std::memory_order_release);
    io_thread = std::thread([this]() {
      util::NameCurrentThread("dap-io");
      IoMain();
    });
    Wake();
    initializing.store(false, std::memory_order_release);
  }

  void DoShutdown() {
    TraceLifecycle("shutdown-begin");
    shutting_down.store(true, std::memory_order_release);
    stop_init.store(true);

    if (test_stub_mode.load(std::memory_order_acquire)) {
      ClearDeferredMessages();
      {
        std::lock_guard lock(mutex);
        test_request_handler = nullptr;
      }
      test_stub_mode.store(false, std::memory_order_release);
      MarkSessionEnded();
      main_mailbox.PushWake();
      return;
    }

    if (!initialized.load(std::memory_order_acquire)) {
      CancelBeforeInitialized();
      main_mailbox.PushWake();
      return;
    }

    if (init_thread.joinable()) {
      init_thread.join();
    }
    ClearDeferredMessages();

    using namespace util;
    int disconnect_seq = 0;
    {
      std::lock_guard lock(mutex);
      shutdown_response_received = false;
      shutdown_request_seq = next_seq++;
      disconnect_seq = shutdown_request_seq;
    }
    JsonObject disconnect_args;
    disconnect_args["restart"] = JsonValue(false);
    disconnect_args["terminateDebuggee"] =
        JsonValue(terminate_debuggee_on_disconnect.load(std::memory_order_acquire));
    const bool sent_disconnect = SendMessageImmediate(
        dap_protocol::MakeRequest(disconnect_seq, "disconnect", JsonValue(std::move(disconnect_args))),
        true, std::chrono::milliseconds(1000));
    TraceLifecycle("disconnect-request", sent_disconnect ? "sent" : "send-failed");
    if (sent_disconnect) {
      std::unique_lock lock(mutex);
      const bool got_response = shutdown_cv.wait_for(lock, std::chrono::milliseconds(750),
                                                     [this]() { return shutdown_response_received; });
      TraceLifecycle("disconnect-response", got_response ? "received" : "timeout");
    }
    if (proc.IsRunning()) {
      proc.CloseStdin();
    }
    StopProcessAfterGoodbye(std::chrono::milliseconds(3000));
    MarkSessionEnded();
    TraceDapLifecycle(adapter_id, -1, "shutdown-complete");
    main_mailbox.PushWake();
  }
};

}
