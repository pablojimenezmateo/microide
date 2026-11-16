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

// A sanitizer build reports its own crashes, with a better stack than this one,
// and its runtime owns the fatal signals.
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

namespace microide::platform {

// What the process says about itself when it dies or stops answering, on stderr
// (where its log goes).
//
// **A crash.** The default action of SIGSEGV or SIGABRT is to die saying nothing,
// and an assertion's last word is the assertion's text, not who tripped it: the
// soak's `std::clamp` abort (707c77a79c1d, `Assertion '!(__hi < __lo)' failed`,
// exit -6) came with no stack, never replayed, and every crash of the unsanitized
// builds had the one signature "microide exited (-N)" -- different defects folded
// into one finding. The handler prints the crashing thread's return addresses
// (`microide(+0x1a2b3c)`, which `addr2line -e microide` turns into functions and
// lines: tools/qa/microide_qa.py symbolize_crash) and then lets the signal's
// default action happen, so the exit status and any core dump are what they were.
//
// **A stall.** The main loop is where latency is felt, and "the query took 6 s"
// says nothing about where the loop was. With MICROIDE_TRACE_STALL_MS=<n> a
// watchdog thread notices a loop turn that has run for n ms, interrupts the main
// thread for its stack, and prints it; the turn's end prints how long it took.
// Off unless asked: the sampling signal interrupts a blocking system call, which
// correct code tolerates and production need not be made to.
//
// Header-only so the entry point and the main loop's wait bracket get it without
// a new source in every curated target list.
namespace crash_diagnostics_detail {

inline constexpr int kMaxFrames = 64;
inline constexpr std::size_t kAlternateStackBytes = 64 * 1024;

// Everything a handler calls is async-signal-safe: write(2), syscalls, and
// backtrace() once its unwinder is loaded (Install loads it).
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
  // SA_RESETHAND has put the default action back: a fault in here ends the
  // process instead of recursing.
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
  // The default action, as if this handler had never been: the same exit status,
  // the same core dump.
  ::raise(signo);
}

// The stall watchdog's shared state. One per process, never freed: the handler
// and the watchdog thread may read it at any time until exit.
struct StallState {
  // When the loop turn in progress began (steady clock, ns); 0 while the main
  // thread sits in its blocking wait, which is idleness and not a stall.
  std::atomic<std::int64_t> busy_since_ns{0};
  // The turn the watchdog reported (its busy_since), so the main thread prints
  // the turn's end; 0 when none is owed.
  std::atomic<std::int64_t> reported_since_ns{0};
  std::int64_t threshold_ns = 0;
  pthread_t main_thread{};
  int sample_signal = 0;
  // The main thread's stack, written by its handler for the watchdog to print.
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
      // The handler runs as soon as the main thread is scheduled; on a starved
      // machine that is not at once.
      for (int waited = 0; waited < 400 && count < 0; ++waited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        count = state->frame_count.load(std::memory_order_acquire);
      }
    }
    // One write: other threads log to the same descriptor.
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

#endif  // MICROIDE_HAS_BACKTRACE

}  // namespace crash_diagnostics_detail

// Prints the crashing thread's backtrace on SIGSEGV, SIGABRT, SIGBUS, SIGFPE and
// SIGILL, then lets the signal kill the process as it would have. Call once,
// early in main. Nothing under a sanitizer, which reports these itself.
inline void InstallCrashBacktraceHandler() {
#if MICROIDE_HAS_BACKTRACE && !MICROIDE_SANITIZER_OWNS_SIGNALS
  namespace detail = crash_diagnostics_detail;
  // Loads the unwinder now: its first use allocates, which a handler must not.
  void* warm[1];
  (void)::backtrace(warm, 1);
  // A stack of its own for the handler: a stack overflow leaves the thread's
  // none to report from. For this (the main) thread; others report on theirs.
  // Process-lifetime: the kernel keeps the pointer.
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

// Starts the main-loop stall watchdog when MICROIDE_TRACE_STALL_MS names a
// threshold of at least 1 ms. Call once, on the main thread, before the loop.
// Returns whether it started.
inline bool StartStallWatchdogFromEnvironment() {
#if MICROIDE_HAS_BACKTRACE
  namespace detail = crash_diagnostics_detail;
  const char* text = std::getenv("MICROIDE_TRACE_STALL_MS");
  const auto threshold_ms = text != nullptr ? util::ParseInt64(text) : std::nullopt;
  if (!threshold_ms.has_value() || *threshold_ms < 1 ||
      detail::g_stall_state.load(std::memory_order_acquire) != nullptr) {
    return false;
  }
  void* warm[1];
  (void)::backtrace(warm, 1);
  auto* state = new detail::StallState();  // process-lifetime, see StallState
  state->threshold_ns = *threshold_ms * 1'000'000;
  state->main_thread = ::pthread_self();
  // A real-time signal nothing else in the process uses (glibc keeps the first
  // two for itself).
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

// The main loop's heartbeat: call at the top of every turn. One atomic load when
// the watchdog is off.
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

// Brackets the main loop's blocking wait for events: time spent in it is the
// loop being idle, not stalled.
class MainLoopIdleScope {
 public:
  MainLoopIdleScope() {
    // The turn that ends here may be one the watchdog reported.
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

}  // namespace microide::platform
