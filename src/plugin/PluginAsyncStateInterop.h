#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

#include "plugin/PluginHostRuntimeTypes.h"

namespace microide::plugin::async_state_interop {

void SetEventType(runtime_types::AsyncProcessState& state, std::uint32_t type);

void CancelCallbacks(runtime_types::AsyncProcessState& state);

std::vector<runtime_types::AsyncProcessCallback> TakePendingCallbacks(
    runtime_types::AsyncProcessState& state);

int PendingCount(runtime_types::AsyncProcessState& state);

void NotifyWorkerCompleted(runtime_types::AsyncProcessState& state);

bool DrainAndJoinWorkers(runtime_types::AsyncProcessState& state,
                         std::chrono::milliseconds deadline);

}  // namespace microide::plugin::async_state_interop
