#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "util/PerformanceCounters.h"
#include "util/ThreadName.h"
#include "util/WakePipe.h"
#include "workspace/HostPathTranslator.h"
#include "workspace/JsonRpcMessageFraming.h"
#include "workspace/StdioClientQueue.h"
#include "platform/AsyncSubprocess.h"

namespace microide::workspace {

struct StdioTransportConfig {
  util::PerfCounterId messages_sent;
  util::PerfCounterId bytes_sent;
  util::PerfCounterId bytes_received;
  std::size_t max_message_bytes = kDefaultMaxJsonRpcMessageBytes;
  std::size_t max_read_buffer_bytes = kDefaultMaxJsonRpcMessageBytes + (1ull * 1024 * 1024);
  std::size_t max_queued_messages = 50000;
  std::size_t max_queued_bytes = 512u * 1024 * 1024;
  const char* peer_label = "?";
  const char* send_error = "failed to send message to peer";
};

template <typename Derived>
class StdioJsonRpcClientTransport {
 public:
  using QueuedMessage = StdioQueuedMessage;

  explicit StdioJsonRpcClientTransport(const StdioTransportConfig& config)
      : transport_config_(config),
        framer_{.max_message_bytes = config.max_message_bytes},
        max_queued_bytes_(config.max_queued_bytes) {}

  platform::AsyncSubprocess proc;
  std::mutex send_mutex;
  std::mutex write_mutex;

  StdioTransportConfig transport_config_;

  JsonRpcMessageFramer framer_;
  std::thread io_thread;
  std::atomic<bool> stop_io{false};
  util::WakePipe wake_pipe_;
  int cached_stdout_fd_ = -1;
  HostPathTranslator path_translator_;

  std::atomic<bool> shutting_down{false};
  std::atomic<bool> process_shutdown_started{false};
  std::atomic<bool> initialized{false};
  std::atomic<bool> test_stub_mode{false};

  std::thread init_thread;
  std::atomic<bool> initializing{false};
  std::atomic<bool> stop_init{false};
  std::thread shutdown_thread;
  std::atomic<bool> shutdown_started{false};
  std::atomic<bool> shutdown_complete{false};

  std::deque<QueuedMessage> deferred_messages;
  std::deque<QueuedMessage> outbound_messages;
  std::size_t outbound_queued_bytes_ = 0;
  std::size_t max_queued_bytes_ = 0;
  bool outbound_overflow_logged_ = false;

  void OpenWakePipe() { wake_pipe_.Open(); }
  void CloseWakePipe() { wake_pipe_.Close(); }
  void Wake() { wake_pipe_.Wake(); }

  std::string SerializeMessage(const util::JsonValue& msg) const {
    if (path_translator_.active()) {
      util::JsonValue translated = msg;
      path_translator_.ToHost(translated);
      return SerializeTranslated(translated);
    }
    return SerializeTranslated(msg);
  }

  void SetPathTranslator(HostPathTranslator translator) { path_translator_ = translator; }

 private:
  std::string SerializeTranslated(const util::JsonValue& msg) const {
    derived().TraceSend(msg);
    const std::string json = util::SerializeJson(msg);
    std::string framed;
    framed.reserve(32 + json.size());
    framed += "Content-Length: ";
    framed += std::to_string(json.size());
    framed += "\r\n\r\n";
    framed += json;
    return framed;
  }

 public:

  void DrainOutbound() {
    std::deque<QueuedMessage> batch;
    {
      std::lock_guard lock(send_mutex);
      batch.swap(outbound_messages);
    }
    if (batch.empty()) {
      return;
    }
    {
      std::size_t drained_bytes = 0;
      for (const QueuedMessage& queued : batch) {
        drained_bytes += queued.approx_bytes;
      }
      std::lock_guard lock(send_mutex);
      outbound_queued_bytes_ -= std::min(outbound_queued_bytes_, drained_bytes);
    }
    std::lock_guard wlock(write_mutex);
    for (QueuedMessage& queued : batch) {
      util::AddPerformanceCounter(transport_config_.messages_sent);
      util::AddPerformanceCounter(transport_config_.bytes_sent, queued.approx_bytes);
      if (!proc.Write(std::move(queued).TakeSerialized())) {
        derived().SetLastError(transport_config_.send_error);
        stop_io.store(true, std::memory_order_release);
        return;
      }
    }
  }

  bool SendMessageImmediate(const util::JsonValue& msg, bool allow_during_shutdown = false,
                            std::chrono::milliseconds lock_timeout = std::chrono::milliseconds::zero()) {
    if (!allow_during_shutdown && shutting_down.load(std::memory_order_acquire)) {
      return false;
    }
    const std::string serialized = SerializeMessage(msg);
    util::AddPerformanceCounter(transport_config_.messages_sent);
    util::AddPerformanceCounter(transport_config_.bytes_sent, serialized.size());
    if (lock_timeout > std::chrono::milliseconds::zero()) {
      std::unique_lock<std::mutex> wlock(write_mutex, std::defer_lock);
      const auto deadline = std::chrono::steady_clock::now() + lock_timeout;
      while (!wlock.try_lock()) {
        if (std::chrono::steady_clock::now() >= deadline) {
          return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return proc.Write(serialized);
    }
    std::lock_guard wlock(write_mutex);
    return proc.Write(serialized);
  }

  bool SendMessageAfterInitialize(util::JsonValue msg) {
    return SendMessageBuilderAfterInitialize(
        [this, m = std::move(msg)]() { return SerializeMessage(m); });
  }

  bool SendMessageBuilderAfterInitialize(std::function<std::string()> build_serialized,
                                         std::size_t approx_bytes = 0) {
    if (shutting_down.load(std::memory_order_acquire)) {
      return false;
    }
    std::lock_guard lock(send_mutex);
    if (shutting_down.load(std::memory_order_acquire)) {
      return false;
    }
    if (deferred_messages.size() + outbound_messages.size() >= transport_config_.max_queued_messages ||
        outbound_queued_bytes_ + approx_bytes > max_queued_bytes_) {
      if (!outbound_overflow_logged_) {
        outbound_overflow_logged_ = true;
        std::fprintf(stderr,
                     "[%s] outbound queue exceeded %zu messages / %zu bytes; peer appears "
                     "wedged, refusing further messages\n",
                     transport_config_.peer_label, transport_config_.max_queued_messages,
                     max_queued_bytes_);
      }
      return false;
    }
    if (!initialized.load(std::memory_order_acquire)) {
      outbound_queued_bytes_ += approx_bytes;
      deferred_messages.push_back(QueuedMessage{
          .serialized = {},
          .build_serialized = std::move(build_serialized),
          .approx_bytes = approx_bytes,
      });
      return true;
    }
    if (test_stub_mode.load(std::memory_order_acquire)) {
      return true;
    }
    if (stop_io.load(std::memory_order_acquire) || !proc.IsRunning()) {
      return false;
    }
    outbound_queued_bytes_ += approx_bytes;
    outbound_messages.push_back(QueuedMessage{
        .serialized = {},
        .build_serialized = std::move(build_serialized),
        .approx_bytes = approx_bytes,
    });
    Wake();
    return true;
  }

  void ClearDeferredMessages() {
    std::lock_guard lock(send_mutex);
    deferred_messages.clear();
    outbound_messages.clear();
    outbound_queued_bytes_ = 0;
  }

  void ShutdownProcessOnce(int timeout_ms = 3000) {
    bool expected = false;
    if (!process_shutdown_started.compare_exchange_strong(expected, true,
                                                          std::memory_order_acq_rel)) {
      return;
    }
    proc.Shutdown(timeout_ms);
  }

  void LaunchInitializeThread() {
    shutdown_started.store(false, std::memory_order_release);
    shutdown_complete.store(false, std::memory_order_release);
    shutting_down.store(false, std::memory_order_release);
    process_shutdown_started.store(false, std::memory_order_release);
    stop_init.store(false);
    init_thread = std::thread([this]() {
      util::NameCurrentThread("rpc-initialize");
      derived().DoInitializeBlocking();
    });
  }

  void AbandonInitialize() {
    ShutdownProcessOnce();
    derived().FailPendingRequests(false);
    ClearDeferredMessages();
    initializing.store(false, std::memory_order_release);
  }

  void BeginShutdown() {
    bool expected = false;
    if (!shutdown_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      return;
    }
    shutdown_complete.store(false, std::memory_order_release);
    shutdown_thread = std::thread([this]() {
      util::NameCurrentThread("rpc-shutdown");
      derived().DoShutdown();
    });
  }

  void WaitForShutdown() {
    BeginShutdown();
    if (shutdown_thread.joinable()) {
      shutdown_thread.join();
    }
  }

  void CancelBeforeInitialized() {
    derived().TraceLifecycle("preinit-cancel");
    stop_io.store(true, std::memory_order_release);
    Wake();
    ShutdownProcessOnce(0);
    if (init_thread.joinable()) {
      init_thread.join();
    }
    if (io_thread.joinable()) {
      stop_io.store(true, std::memory_order_release);
      Wake();
      io_thread.join();
    }
    ClearDeferredMessages();
    CloseWakePipe();
    MarkSessionEnded();
  }

  void StopProcessAfterGoodbye(std::chrono::milliseconds grace) {
    const auto deadline = std::chrono::steady_clock::now() + grace;
    while (proc.IsRunning() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    derived().TraceLifecycle("graceful-wait", proc.IsRunning() ? "still-running" : "exited");
    stop_io.store(true, std::memory_order_release);
    Wake();
    if (proc.IsRunning()) {
      derived().TraceLifecycle("forced-shutdown");
      ShutdownProcessOnce(1000);
    }
    if (io_thread.joinable()) {
      io_thread.join();
    }
    CloseWakePipe();
  }

  void MarkSessionEnded() {
    derived().ResetProtocolState();
    initialized.store(false, std::memory_order_release);
    initializing.store(false, std::memory_order_release);
    shutting_down.store(false, std::memory_order_release);
    shutdown_complete.store(true, std::memory_order_release);
  }

  bool PeerRunning() {
    return test_stub_mode.load(std::memory_order_acquire) || proc.IsRunning();
  }

  bool IsShutdownComplete() const {
    return shutdown_started.load(std::memory_order_acquire) &&
           shutdown_complete.load(std::memory_order_acquire);
  }

  bool WaitStdoutReadable(int timeout_ms) {
    return wake_pipe_.PollReadableOrWake(proc.stdout_fd(), timeout_ms);
  }

  void ParseBufferedMessages() {
    while (true) {
      const std::size_t buffered_before = framer_.BufferedBytes();
      auto msg_opt = framer_.Next();
      if (msg_opt) {
        path_translator_.FromHost(*msg_opt);
        derived().DispatchMessage(std::move(*msg_opt));
        continue;
      }
      if (framer_.BufferedBytes() == buffered_before) {
        break;
      }
    }
  }

  void IoMain() {
    cached_stdout_fd_ = proc.stdout_fd();
    const int read_timeout = cached_stdout_fd_ >= 0 ? 0 : 50;
    while (!stop_io.load(std::memory_order_acquire)) {
      ParseBufferedMessages();
      DrainOutbound();
      if (!shutting_down.load(std::memory_order_acquire)) {
        derived().FailPendingRequests(true);
      }
      if (stop_io.load(std::memory_order_acquire)) {
        break;
      }
      if (!WaitStdoutReadable(1000)) {
        continue;
      }
      auto chunk = proc.Read(4096, read_timeout);
      if (!chunk) {
        break;
      }
      if (!chunk->empty()) {
        util::AddPerformanceCounter(transport_config_.bytes_received, chunk->size());
        framer_.Append(*chunk);
      }
      if (framer_.BufferedBytes() > transport_config_.max_read_buffer_bytes) {
        break;
      }
    }
    ParseBufferedMessages();
    DrainOutbound();
    if (!shutting_down.load(std::memory_order_acquire)) {
      derived().FailPendingRequests(false);
      derived().WakeMainThread();
    }
    stop_io.store(true, std::memory_order_release);
  }

 private:
  Derived& derived() { return static_cast<Derived&>(*this); }
  const Derived& derived() const { return static_cast<const Derived&>(*this); }
};

}
