#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

#include <pthread.h>
#include <unistd.h>

#include "util/Parse.h"

#if defined(__linux__) && defined(__has_include)
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#define MICROIDE_HAS_BACKTRACE 1
#endif
#endif
#ifndef MICROIDE_HAS_BACKTRACE
#define MICROIDE_HAS_BACKTRACE 0
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define MICROIDE_SANITIZER_OWNS_SIGNALS 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define MICROIDE_SANITIZER_OWNS_SIGNALS 1
#endif
#endif
#ifndef MICROIDE_SANITIZER_OWNS_SIGNALS
#define MICROIDE_SANITIZER_OWNS_SIGNALS 0
#endif

#if defined(__SANITIZE_THREAD__)
#define MICROIDE_THREAD_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define MICROIDE_THREAD_SANITIZER 1
#endif
#endif
#ifndef MICROIDE_THREAD_SANITIZER
#define MICROIDE_THREAD_SANITIZER 0
#endif

namespace microide::platform {

namespace crash_diagnostics_detail {

inline constexpr int kMaxFrames = 64;
inline constexpr std::size_t kAlternateStackBytes = 64 * 1024;

inline void WriteAll(const char* text, std::size_t size) {
  while (size > 0) {
    const ssize_t wrote = ::write(STDERR_FILENO, text, size);
    if (wrote < 0 && errno == EINTR) {
      continue;
    }
    if (wrote <= 0) {
      return;
    }
    text += wrote;
    size -= static_cast<std::size_t>(wrote);
  }
}

inline void WriteText(const char* text) { WriteAll(text, std::strlen(text)); }

inline void WriteNumber(long long value) {
  char digits[24];
  std::size_t at = sizeof(digits);
  const bool negative = value < 0;
  unsigned long long rest = negative ? 0ULL - static_cast<unsigned long long>(value)
                                     : static_cast<unsigned long long>(value);
  do {
    digits[--at] = static_cast<char>('0' + rest % 10);
    rest /= 10;
  } while (rest != 0 && at > 1);
  if (negative) {
    digits[--at] = '-';
  }
  WriteAll(digits + at, sizeof(digits) - at);
}

inline const char* SignalName(int signo) {
  switch (signo) {
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGBUS: return "SIGBUS";
    case SIGFPE: return "SIGFPE";
    case SIGILL: return "SIGILL";
    default: return "signal";
  }
}

#if MICROIDE_HAS_BACKTRACE

inline void WriteThreadIdentity() {
  WriteText("thread ");
  WriteNumber(static_cast<long long>(::syscall(SYS_gettid)));
  char name[17] = {};
  if (::prctl(PR_GET_NAME, name, 0, 0, 0) == 0 && name[0] != '\0') {
    WriteText(" (");
    WriteText(name);
    WriteText(")");
  }
}

inline void OnFatalSignal(int signo) {
  WriteText("microide crash: signal ");
  WriteNumber(signo);
  WriteText(" (");
  WriteText(SignalName(signo));
  WriteText(") on ");
  WriteThreadIdentity();
  WriteText("; backtrace:\n");
  void* frames[kMaxFrames];
  const int count = ::backtrace(frames, kMaxFrames);
  ::backtrace_symbols_fd(frames, count, STDERR_FILENO);
  WriteText("microide crash: end of backtrace\n");
  ::raise(signo);
}

struct StallState {
  std::atomic<std::int64_t> busy_since_ns{0};
  std::atomic<std::int64_t> reported_since_ns{0};
  std::int64_t threshold_ns = 0;
  pthread_t main_thread{};
  int sample_signal = 0;
  void* frames[kMaxFrames] = {};
  std::atomic<int> frame_count{-1};
};

inline std::atomic<StallState*> g_stall_state{nullptr};

inline std::int64_t SteadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline void OnSampleSignal(int) {
  const int saved_errno = errno;
  if (StallState* state = g_stall_state.load(std::memory_order_acquire); state != nullptr) {
    state->frame_count.store(::backtrace(state->frames, kMaxFrames), std::memory_order_release);
  }
  errno = saved_errno;
}

inline void WatchStalls(StallState* state) {
  (void)::prctl(PR_SET_NAME, "stall-watchdog", 0, 0, 0);
  const auto poll = std::chrono::nanoseconds(state->threshold_ns / 4 + 1);
  std::int64_t reported = 0;
  for (;;) {
    std::this_thread::sleep_for(poll);
    const std::int64_t since = state->busy_since_ns.load(std::memory_order_acquire);
    if (since == 0 || since == reported) {
      continue;
    }
    const std::int64_t busy = SteadyNowNs() - since;
    if (busy < state->threshold_ns) {
      continue;
    }
    reported = since;
    state->frame_count.store(-1, std::memory_order_release);
    int count = -1;
    if (::pthread_kill(state->main_thread, state->sample_signal) == 0) {
      for (int waited = 0; waited < 400 && count < 0; ++waited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        count = state->frame_count.load(std::memory_order_acquire);
      }
    }
    std::string block = "microide stall: the main loop has not turned for " +
                        std::to_string(busy / 1'000'000) + " ms (threshold " +
                        std::to_string(state->threshold_ns / 1'000'000) + " ms); ";
    if (count <= 0) {
      block += "the main thread did not answer the sampling signal (not scheduled, or signals blocked)\n";
    } else {
      block += "the main thread is at:\n";
      if (char** symbols = ::backtrace_symbols(state->frames, count); symbols != nullptr) {
        for (int index = 0; index < count; ++index) {
          block += symbols[index];
          block += '\n';
        }
        std::free(symbols);  // NOLINT(cppcoreguidelines-no-malloc): backtrace_symbols' contract
      }
      block += "microide stall: end of backtrace\n";
    }
    WriteAll(block.data(), block.size());
    state->reported_since_ns.store(since, std::memory_order_release);
  }
}

#endif

}

inline void InstallCrashBacktraceHandler() {
#if MICROIDE_HAS_BACKTRACE && !MICROIDE_SANITIZER_OWNS_SIGNALS
  namespace detail = crash_diagnostics_detail;
  void* warm[1];
  (void)::backtrace(warm, 1);
  static char* const alternate_stack = new char[detail::kAlternateStackBytes];
  stack_t alternate{};
  alternate.ss_sp = alternate_stack;
  alternate.ss_size = detail::kAlternateStackBytes;
  (void)::sigaltstack(&alternate, nullptr);
  struct sigaction action {};
  action.sa_handler = detail::OnFatalSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESETHAND | SA_NODEFER | SA_ONSTACK;
  for (const int signo : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL}) {
    (void)::sigaction(signo, &action, nullptr);
  }
#endif
}

inline bool StartStallWatchdogFromEnvironment() {
#if MICROIDE_HAS_BACKTRACE && !MICROIDE_THREAD_SANITIZER
  namespace detail = crash_diagnostics_detail;
  const char* text = std::getenv("MICROIDE_TRACE_STALL_MS");
  const auto threshold_ms = text != nullptr ? util::ParseInt64(text) : std::nullopt;
  if (!threshold_ms.has_value() || *threshold_ms < 1 ||
      detail::g_stall_state.load(std::memory_order_acquire) != nullptr) {
    return false;
  }
  void* warm[1];
  (void)::backtrace(warm, 1);
  auto* state = new detail::StallState();
  state->threshold_ns = *threshold_ms * 1'000'000;
  state->main_thread = ::pthread_self();
  state->sample_signal = SIGRTMIN + 6;
  struct sigaction action {};
  action.sa_handler = detail::OnSampleSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESTART;
  if (::sigaction(state->sample_signal, &action, nullptr) != 0) {
    delete state;
    return false;
  }
  state->busy_since_ns.store(detail::SteadyNowNs(), std::memory_order_release);
  detail::g_stall_state.store(state, std::memory_order_release);
  std::thread(detail::WatchStalls, state).detach();
  return true;
#else
  return false;
#endif
}

inline void NoteMainLoopTurn() {
#if MICROIDE_HAS_BACKTRACE
  namespace detail = crash_diagnostics_detail;
  detail::StallState* state = detail::g_stall_state.load(std::memory_order_acquire);
  if (state == nullptr) {
    return;
  }
  const std::int64_t now = detail::SteadyNowNs();
  const std::int64_t began = state->busy_since_ns.exchange(now, std::memory_order_acq_rel);
  const std::int64_t reported = state->reported_since_ns.load(std::memory_order_acquire);
  if (reported != 0 && reported == began) {
    state->reported_since_ns.store(0, std::memory_order_release);
    const std::string line =
        "microide stall: ended after " + std::to_string((now - began) / 1'000'000) + " ms\n";
    detail::WriteAll(line.data(), line.size());
  }
#endif
}

class MainLoopIdleScope {
 public:
  MainLoopIdleScope() {
    NoteMainLoopTurn();
    Set(0);
  }
  ~MainLoopIdleScope() {
#if MICROIDE_HAS_BACKTRACE
    Set(crash_diagnostics_detail::SteadyNowNs());
#endif
  }
  MainLoopIdleScope(const MainLoopIdleScope&) = delete;
  MainLoopIdleScope& operator=(const MainLoopIdleScope&) = delete;

 private:
  static void Set([[maybe_unused]] std::int64_t busy_since_ns) {
#if MICROIDE_HAS_BACKTRACE
    namespace detail = crash_diagnostics_detail;
    if (detail::StallState* state = detail::g_stall_state.load(std::memory_order_acquire);
        state != nullptr) {
      state->busy_since_ns.store(busy_since_ns, std::memory_order_release);
    }
#endif
  }
};

}
