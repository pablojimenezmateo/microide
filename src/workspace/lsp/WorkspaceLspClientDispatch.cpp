#include "workspace/lsp/WorkspaceLspClientInternal.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/StartupTrace.h"
#include "workspace/lsp/LspProtocol.h"
#include "workspace/ProtocolNumeric.h"

namespace microide::workspace {

namespace {

bool ResponseIdInRange(const util::JsonValue& id_value, int* out) {
  if (!protocol_numeric::IsIntegralJsonNumber(id_value)) {
    return false;
  }
  const std::int64_t raw = id_value.AsInt();
  if (raw < static_cast<std::int64_t>(std::numeric_limits<int>::min()) ||
      raw > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  *out = static_cast<int>(raw);
  return true;
}

}

void LspClient::Impl::SendResponseResult(const util::JsonValue& id, util::JsonValue result) {
  using namespace util;
  JsonValue msg = MakeResponse(id);
  std::get<JsonObject>(msg.v)["result"] = std::move(result);
  SendMessageAfterInitialize(std::move(msg));
}

void LspClient::Impl::SendResponseError(const util::JsonValue& id, int code, std::string message) {
  using namespace util;
  JsonObject error;
  error["code"] = JsonValue(static_cast<std::int64_t>(code));
  error["message"] = JsonValue(std::move(message));
  JsonValue msg = MakeResponse(id);
  std::get<JsonObject>(msg.v)["error"] = JsonValue(std::move(error));
  SendMessageAfterInitialize(std::move(msg));
}

void LspClient::Impl::HandleServerRequest(const util::JsonValue& id, const std::string& method,
                                          const util::JsonValue& params) {
  using namespace util;
  if (method == "workspace/configuration") {
    constexpr std::size_t kMaxConfigurationItems = 256;
    constexpr std::size_t kMaxFullSettingsDuplications = 16;
    const JsonArray& items = params["items"].AsArray();
    const std::size_t item_count = std::min(items.size(), kMaxConfigurationItems);
    JsonArray result;
    result.reserve(item_count);
    std::size_t full_settings_returned = 0;
    for (std::size_t i = 0; i < item_count; ++i) {
      const std::string& section = items[i]["section"].AsString();
      JsonValue value;
      if (settings.IsObject()) {
        if (section.empty()) {
          if (full_settings_returned < kMaxFullSettingsDuplications) {
            value = settings;
            ++full_settings_returned;
          }
        } else if (settings.HasKey(section)) {
          value = settings[section];
        }
      }
      result.push_back(std::move(value));
    }
    SendResponseResult(id, JsonValue(std::move(result)));
    return;
  }
  if (method == "client/registerCapability") {
    const JsonValue& registrations = params["registrations"];
    if (registrations.IsArray()) {
      bool changed = false;
      {
        std::lock_guard lock(mutex);
        for (const JsonValue& registration : registrations.AsArray()) {
          if (registration["method"].AsString() != "workspace/didChangeWatchedFiles") {
            continue;
          }
          changed |= file_watch_registry.Register(registration, root_path);
        }
        if (changed) {
          has_file_watchers.store(!file_watch_registry.empty(), std::memory_order_release);
        }
      }
    }
    SendResponseResult(id, JsonValue(nullptr));
    return;
  }
  if (method == "client/unregisterCapability") {
    const JsonValue& unregistrations = params["unregisterations"].IsArray()
                                           ? params["unregisterations"]
                                           : params["unregistrations"];
    if (unregistrations.IsArray()) {
      std::lock_guard lock(mutex);
      for (const JsonValue& unregistration : unregistrations.AsArray()) {
        if (unregistration["method"].AsString() != "workspace/didChangeWatchedFiles") {
          continue;
        }
        file_watch_registry.Unregister(unregistration["id"].AsString());
      }
      has_file_watchers.store(!file_watch_registry.empty(), std::memory_order_release);
    }
    SendResponseResult(id, JsonValue(nullptr));
    return;
  }
  if (method == "window/workDoneProgress/create" || method == "window/showMessageRequest" ||
      method == "window/showDocument") {
    SendResponseResult(id, JsonValue(nullptr));
    return;
  }
  if (method == "workspace/applyEdit") {
    std::function<bool(WorkspaceEdit)> handler;
    {
      std::lock_guard lock(mutex);
      handler = apply_edit_handler;
    }
    if (!handler) {
      JsonObject obj;
      obj["applied"] = JsonValue(false);
      SendResponseResult(id, JsonValue(std::move(obj)));
      return;
    }
    WorkspaceEdit edit = lsp_protocol::ParseWorkspaceEdit(params["edit"]);
    util::JsonValue captured_id = id;
    main_mailbox.Post([this, captured_id = std::move(captured_id), edit = std::move(edit),
                       handler = std::move(handler)]() mutable {
      const bool applied = handler(std::move(edit));
      JsonObject obj;
      obj["applied"] = JsonValue(applied);
      SendResponseResult(captured_id, JsonValue(std::move(obj)));
    });
    return;
  }
  SendResponseError(id, -32601, "method not found: " + method);
}

void LspClient::Impl::DispatchMessage(util::JsonValue msg) {
  util::PerformanceTrace::Scope perf_scope("lsp::DispatchMessage");
  util::AddPerformanceCounter(util::PerfCounterId::LspMessagesReceived);
  const bool has_method = msg.HasKey("method") && msg["method"].IsString();
  const bool has_id = msg.HasKey("id") && !msg["id"].IsNull();
  if (shutting_down.load(std::memory_order_acquire)) {
    int id = 0;
    if (has_id && !has_method && (msg["id"].IsInt() || msg["id"].IsDouble()) &&
        ResponseIdInRange(msg["id"], &id)) {
      std::lock_guard lock(mutex);
      if (id == shutdown_request_id) {
        shutdown_response_received = true;
        shutdown_cv.notify_all();
      }
    }
    return;
  }
  if (has_method && has_id) {
    HandleServerRequest(msg["id"], msg["method"].AsString(), msg["params"]);
    return;
  }
  int id = 0;
  if (has_id && !has_method && (msg["id"].IsInt() || msg["id"].IsDouble()) &&
      ResponseIdInRange(msg["id"], &id)) {
    std::function<void(LspRequestOutcome, util::JsonValue)> cb;
    std::chrono::steady_clock::time_point sent_at;
    {
      std::lock_guard lock(mutex);
      if (auto entry = pending_requests.Take(id)) {
        cb = std::move(entry->callback);
        sent_at = entry->sent_at;
      }
    }
    if (cb && util::PerformanceTrace::SummaryEnabled()) {
      const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - sent_at)
                                    .count();
      util::PerformanceTrace::RecordSample("lsp::RequestRoundTrip", elapsed_ms);
    }
    if (cb) {
      util::JsonValue captured = std::move(msg);
      main_mailbox.Post([cb = std::move(cb), m = std::move(captured)]() mutable {
        util::StartupTrace::Scope scope("LspClient::DispatchResponse");
        cb(LspRequestOutcome::kOk, std::move(m));
      });
    }
  } else if (msg.HasKey("method")) {
    const std::string& method = msg["method"].AsString();
    if (method == "textDocument/publishDiagnostics") {
      util::StartupTrace::Scope scope("LspClient::DispatchDiagnostics");
      const auto& params = msg["params"];
      std::string uri = params["uri"].AsString();
      const util::JsonValue& version_val = params["version"];
      const std::optional<std::int64_t> version =
          (version_val.IsInt() || version_val.IsDouble())
              ? std::optional<std::int64_t>(version_val.AsInt()) : std::nullopt;
      std::vector<Diagnostic> diags = lsp_protocol::ParseDiagnostics(params["diagnostics"]);
      TraceLspLifecycle(language_id, proc.pid(), "publishDiagnostics", uri);
      OnPublishDiagnostics cb;
      {
        std::lock_guard lock(mutex);
        if (version.has_value()) {
          const auto it = document_versions.find(uri);
          if (it != document_versions.end() && *version < it->second) {
            return;
          }
        }
        cb = diagnostics_callback;
      }
      if (cb) {
        std::string coalesce_key = "diag:" + uri;
        main_mailbox.PostLatest(
            std::move(coalesce_key),
            [this, version, cb = std::move(cb), u = std::move(uri), ds = std::move(diags)]() mutable {
              if (version.has_value()) {
                std::lock_guard lock(mutex);
                const auto it = document_versions.find(u);
                if (it != document_versions.end() && *version < it->second) {
                  return;
                }
              }
              cb(std::move(u), std::move(ds));
            });
      }
    } else if (method == "$/progress") {
      SetProgressReadiness(msg["params"]["value"]);
    }
  }
}

void LspClient::Impl::FailPendingRequests(bool only_expired) {
  const auto now = std::chrono::steady_clock::now();
  const LspRequestOutcome outcome =
      only_expired ? LspRequestOutcome::kTimeout : LspRequestOutcome::kUnavailable;
  std::lock_guard lock(mutex);
  const std::size_t failed = pending_requests.Sweep(only_expired, now, [&](int, auto entry) {
    main_mailbox.PostWithoutWake([cb = std::move(entry.callback), outcome]() mutable {
      cb(outcome, util::JsonValue(util::JsonObject{}));
    });
  });
  if (failed > 0) {
    main_mailbox.PushWake();
  }
}

}
