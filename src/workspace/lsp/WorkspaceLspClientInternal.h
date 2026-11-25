#pragma once

#include <array>

#include "workspace/PendingRequestTable.h"
#include "workspace/lsp/WorkspaceLspClient.h"
#include "workspace/lsp/LspFileWatchRegistry.h"
#include "workspace/lsp/LspProtocol.h"
#include "workspace/StdioClientQueue.h"
#include "workspace/StdioJsonRpcClientTransport.h"
#include "util/MainThreadMailbox.h"
#include "util/WakePipe.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "util/StartupTrace.h"
#include "workspace/lsp/LspClientTrace.h"
#include "workspace/JsonRpcMessageFraming.h"
#include "util/StringUtil.h"

namespace microide::workspace {

class AtomicAsciiSet {
 public:
  void Store(std::string_view characters) {
    std::uint64_t mask[2] = {0, 0};
    for (const char c : characters) {
      Add(c, mask);
    }
    Publish(mask);
  }
  void Store(const util::JsonValue& strings) {
    std::uint64_t mask[2] = {0, 0};
    if (strings.IsArray()) {
      for (const util::JsonValue& entry : strings.AsArray()) {
        if (entry.IsString() && entry.AsString().size() == 1) {
          Add(entry.AsString()[0], mask);
        }
      }
    }
    Publish(mask);
  }
  bool Contains(char c) const {
    const auto byte = static_cast<unsigned char>(c);
    return byte < 128 &&
           ((mask_[byte / 64].load(std::memory_order_acquire) >> (byte % 64)) & 1U) != 0;
  }

 private:
  static void Add(char c, std::uint64_t (&mask)[2]) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 128) {
      mask[byte / 64] |= std::uint64_t{1} << (byte % 64);
    }
  }
  void Publish(const std::uint64_t (&mask)[2]) {
    mask_[0].store(mask[0], std::memory_order_release);
    mask_[1].store(mask[1], std::memory_order_release);
  }
  std::array<std::atomic<std::uint64_t>, 2> mask_{};
};

struct LspClient::Impl : StdioJsonRpcClientTransport<LspClient::Impl> {
  Impl()
      : StdioJsonRpcClientTransport(StdioTransportConfig{
            .messages_sent = util::PerfCounterId::LspMessagesSent,
            .bytes_sent = util::PerfCounterId::LspBytesSent,
            .bytes_received = util::PerfCounterId::LspBytesReceived,
            .max_message_bytes = kMaxLspMessageBytes,
            .max_read_buffer_bytes = kMaxLspReadBufferBytes,
            .max_queued_messages = kMaxQueuedMessages,
            .max_queued_bytes = kMaxQueuedBytes,
            .peer_label = "lsp",
            .send_error = "failed to send message to language server",
        }) {}

  void TraceSend(const util::JsonValue&) const {}
  void TraceLifecycle(std::string_view phase, std::string_view detail = {}) {
    TraceLspLifecycle(language_id, proc.pid(), phase, detail);
  }
  void WakeMainThread() { main_mailbox.PushWake(); }

  std::mutex mutex;

  std::condition_variable shutdown_cv;
  bool shutdown_response_received = false;
  int shutdown_request_id = 0;

  PendingRequestTable<std::function<void(LspRequestOutcome, util::JsonValue)>> pending_requests;

  std::chrono::milliseconds request_timeout_ = kLspRequestTimeout;

  util::MainThreadMailbox main_mailbox;

  struct TestHandlers {
    std::function<void(std::string, DocumentSymbolCallback)> document_symbol;
    std::function<void(std::string, SemanticTokensCallback)> semantic_tokens;
    std::function<void(std::string, HoverCallback)> hover;
    std::function<void(std::string, bool, FormattingCallback)> formatting;
    std::function<void(std::string, CodeActionCallback)> code_action;
    std::function<void(std::string, std::string, RenameCallback)> rename;
    std::function<void(std::string, Position, CompletionCallback)> completion;
    std::function<void(std::string, Position, SignatureHelpCallback)> signature_help;
    std::function<void(std::string, Position, PrepareRenameCallback)> prepare_rename;
    std::function<void(std::string, WorkspaceSymbolCallback)> workspace_symbol;
    std::function<void(std::string, Position, LocationCallback)> references;
    std::function<void(std::string, Range, InlayHintCallback)> inlay_hint;
    std::function<void(std::string, Position, DocumentHighlightCallback)> document_highlight;
    std::function<void(std::string, CodeLensCallback)> code_lens;
    std::function<void(util::JsonValue, ResolveCodeLensCallback)> resolve_code_lens;
    std::function<void(std::string, std::vector<util::JsonValue>, ExecuteCommandCallback)>
        execute_command;
    std::function<void(std::string, Position, PrepareCallHierarchyCallback)>
        prepare_call_hierarchy;
    std::function<void(bool, util::JsonValue, CallHierarchyCallsCallback)> call_hierarchy_calls;
    void Reset() { *this = TestHandlers{}; }
  };
  TestHandlers test_handlers;

  std::vector<std::string> semantic_token_types;
  std::atomic<bool> supports_semantic_tokens{false};
  std::atomic<bool> supports_prepare_rename{false};
  std::atomic<bool> supports_inlay_hints{false};
  std::atomic<bool> supports_document_highlight{false};
  std::atomic<bool> supports_code_lens{false};
  std::atomic<bool> supports_code_lens_resolve{false};
  std::atomic<bool> supports_call_hierarchy{false};
  AtomicAsciiSet completion_triggers;
  AtomicAsciiSet signature_help_triggers;

  LspFileWatchRegistry file_watch_registry;
  std::atomic<bool> has_file_watchers{false};

  std::string position_encoding = "utf-16";

  OnPublishDiagnostics diagnostics_callback;
  std::function<bool(WorkspaceEdit)> apply_edit_handler;

  std::unordered_map<std::string, int> document_versions;
  std::unordered_map<std::string, int> retired_document_versions;
  int next_id = 1;
  std::string root_uri;
  std::filesystem::path root_path;
  std::string language_id;
  util::JsonValue initialization_options;
  util::JsonValue settings;
  std::string last_error;
  LspClient::ReadinessSnapshot readiness_snapshot;
  std::atomic<bool> supports_incremental_sync{false};

  void SetLastError(std::string message) {
    std::lock_guard lock(mutex);
    last_error = std::move(message);
    readiness_snapshot.state = LspClient::ReadinessSnapshot::State::Failed;
    readiness_snapshot.message = last_error;
    readiness_snapshot.indexed_count = 0;
  }

  int GetNextId() {
    std::lock_guard lock(mutex);
    return next_id++;
  }

  util::JsonValue MakeRequest(int id, const std::string& method, const util::JsonValue& params) {
    using namespace util;
    JsonObject req;
    req["jsonrpc"] = JsonValue("2.0");
    req["id"] = JsonValue(static_cast<std::int64_t>(id));
    req["method"] = JsonValue(method);
    req["params"] = params;
    return JsonValue(std::move(req));
  }

  util::JsonValue MakeNotification(const std::string& method, const util::JsonValue& params) {
    using namespace util;
    JsonObject msg;
    msg["jsonrpc"] = JsonValue("2.0");
    msg["method"] = JsonValue(method);
    msg["params"] = params;
    return JsonValue(std::move(msg));
  }

  util::JsonValue MakeResponse(const util::JsonValue& id) {
    using namespace util;
    JsonObject msg;
    msg["jsonrpc"] = JsonValue("2.0");
    msg["id"] = id;
    return JsonValue(std::move(msg));
  }

  void ResetProtocolState() {
    std::lock_guard lock(mutex);
    pending_requests.clear();
    main_mailbox.Clear();
    document_versions.clear();
    retired_document_versions.clear();
    shutdown_response_received = false;
    shutdown_request_id = 0;
    readiness_snapshot = LspClient::ReadinessSnapshot{};
    supports_incremental_sync.store(false, std::memory_order_release);
  }

  static int ExtractIndexedCount(std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (!util::IsAsciiDigit(static_cast<unsigned char>(text[i]))) {
        continue;
      }
      std::int64_t value = 0;
      std::size_t cursor = i;
      while (cursor < text.size() &&
             util::IsAsciiDigit(static_cast<unsigned char>(text[cursor]))) {
        value = value * 10 + (text[cursor] - '0');
        if (value > std::numeric_limits<int>::max()) {
          value = std::numeric_limits<int>::max();
        }
        ++cursor;
      }
      return static_cast<int>(value);
    }
    return 0;
  }

  void SetProgressReadiness(const util::JsonValue& value);

  void SendResponseResult(const util::JsonValue& id, util::JsonValue result);
  void SendResponseError(const util::JsonValue& id, int code, std::string message);

  void HandleServerRequest(const util::JsonValue& id, const std::string& method,
                           const util::JsonValue& params);

  void DispatchMessage(util::JsonValue msg);

  int RegisterPendingRequest(std::function<void(LspRequestOutcome, util::JsonValue)> cb) {
    std::lock_guard lock(mutex);
    const int id = next_id++;
    pending_requests.Add(id, std::move(cb), request_timeout_);
    return id;
  }

  bool RemovePendingRequest(int id) {
    std::lock_guard lock(mutex);
    return pending_requests.Remove(id);
  }

  void FailPendingRequests(bool only_expired);

  template <typename Handler, typename Callback, typename... Args>
  bool DispatchTestStub(const Handler& handler_member, Callback& callback, Args&&... args) {
    std::lock_guard lock(mutex);
    if (!test_stub_mode.load(std::memory_order_acquire)) {
      return false;
    }
    Handler handler = handler_member;
    main_mailbox.PostWithoutWake(
        [handler = std::move(handler), cb = std::move(callback),
         ... args = std::forward<Args>(args)]() mutable {
          if (handler) {
            handler(std::move(args)..., std::move(cb));
          } else {
            cb(std::nullopt);
          }
        });
    return true;
  }

  void DispatchRequest(const std::string& method, util::JsonValue params,
                       std::function<void(LspRequestOutcome, util::JsonValue)> response_handler,
                       std::function<void()> on_send_failure) {
    const int id = RegisterPendingRequest(std::move(response_handler));
    if (!SendMessageAfterInitialize(MakeRequest(id, method, std::move(params)))) {
      if (RemovePendingRequest(id)) {
        on_send_failure();
      }
    }
  }

  template <typename Callback, typename Parser>
  void DispatchResultRequest(const std::string& method, util::JsonValue params, Callback callback,
                             Parser parse_result) {
    if (!callback) {
      return;
    }
    using OptT = std::decay_t<std::invoke_result_t<Parser&, util::JsonValue&>>;
    using ResultT = LspResult<typename OptT::value_type>;
    Callback failure = callback;
    DispatchRequest(
        method, std::move(params),
        [cb = std::move(callback), parse_result = std::move(parse_result)](
            LspRequestOutcome transport, util::JsonValue resp) mutable {
          if (transport != LspRequestOutcome::kOk) {
            cb(ResultT(transport, std::nullopt));
            return;
          }
          if (util::JsonValue* error = resp.MutableAt("error"); error != nullptr && !error->IsNull()) {
            cb(ResultT(LspRequestOutcome::kProtocolError, std::nullopt));
            return;
          }
          util::JsonValue* result = resp.MutableAt("result");
          if (result == nullptr) {
            cb(ResultT(LspRequestOutcome::kProtocolError, std::nullopt));
            return;
          }
          OptT parsed = parse_result(*result);
          const LspRequestOutcome outcome =
              parsed.has_value() ? LspRequestOutcome::kOk : LspRequestOutcome::kEmpty;
          cb(ResultT(outcome, std::move(parsed)));
        },
        [failure = std::move(failure)]() mutable {
          failure(ResultT(LspRequestOutcome::kUnavailable, std::nullopt));
        });
  }

  void DoInitializeBlocking();
  void ApplyServerCapabilities(const util::JsonValue& server_caps);

  void DoShutdown();
};

}
