#pragma once

#include <chrono>
#include <cstddef>
#include <string_view>

namespace microide::workspace {

inline constexpr std::chrono::milliseconds kLspRequestTimeout{30000};

inline constexpr std::size_t kMaxQueuedMessages = 50000;

inline constexpr std::size_t kMaxQueuedBytes = 512u * 1024 * 1024;

inline constexpr std::size_t kMaxLspMessageBytes = 64ull * 1024 * 1024;
inline constexpr std::size_t kMaxLspReadBufferBytes = kMaxLspMessageBytes + (1ull * 1024 * 1024);

void TraceLspLifecycle(std::string_view language_id, int pid, std::string_view phase,
                       std::string_view detail = {});

}
