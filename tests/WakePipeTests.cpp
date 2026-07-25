// util::WakePipe tests.
//
// The self-pipe that lets any thread break a blocking poll() on the I/O thread,
// plus PollReadableOrWake — the stdio-transport poll shared by the LSP and DAP
// clients. That poll used to exist as two verbatim copies inside those clients,
// reachable only through a live subprocess, so its behaviour was never asserted
// directly; the copies had already drifted apart once and been re-synced by hand.

#include "TestSupport.h"

#include "util/WakePipe.h"

#include <chrono>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace microide::tests {
namespace {

#if defined(__unix__) || defined(__APPLE__)
// WakePipe::PollReadableOrWake is the stdio-transport poll shared by the LSP and
// DAP clients. It previously existed as two verbatim copies inside those clients,
// reachable only through a live subprocess, so its behaviour was never asserted
// directly — and the copies had already drifted once. Each property below is one
// the transports depend on.
void TestWakePipePollReadableOrWakeContract() {
  microide::util::WakePipe wake;
  wake.Open();
  Expect(wake.read_fd() >= 0, "wake pipe should open");

  int data_fds[2] = {-1, -1};
  Expect(::pipe(data_fds) == 0, "test data pipe should open");

  // 1. No data and no wake: the poll times out and reports "nothing to read", so
  //    the caller loops (drains outbound, sweeps timeouts) rather than blocking.
  Expect(!wake.PollReadableOrWake(data_fds[0], 10),
         "an idle watched fd must report not-readable so the io loop keeps sweeping");

  // 2. Data on the watched fd: readable.
  Expect(::write(data_fds[1], "x", 1) == 1, "test write should succeed");
  Expect(wake.PollReadableOrWake(data_fds[0], 10),
         "a watched fd with pending data must report readable");
  char scratch = 0;
  Expect(::read(data_fds[0], &scratch, 1) == 1, "test read should drain the byte");

  // 3. A Wake() must break the poll promptly even with no stdout data — this is
  //    what makes an outbound message get written without waiting out the
  //    timeout — and the wake byte must be CONSUMED, or the next poll would spin
  //    returning immediately forever.
  wake.Wake();
  const auto wake_start = std::chrono::steady_clock::now();
  Expect(!wake.PollReadableOrWake(data_fds[0], 5000),
         "a wake with no stdout data must report not-readable");
  const auto wake_elapsed = std::chrono::steady_clock::now() - wake_start;
  Expect(wake_elapsed < std::chrono::milliseconds(2000),
         "a wake must break the poll rather than waiting out the timeout");

  const auto second_start = std::chrono::steady_clock::now();
  Expect(!wake.PollReadableOrWake(data_fds[0], 50),
         "the poll after a consumed wake must report not-readable");
  Expect(std::chrono::steady_clock::now() - second_start >= std::chrono::milliseconds(20),
         "the wake byte must have been drained; a retained one would spin the io thread");

  // 4. A closed descriptor arrives as -1 (stdout_fd() returns -1 under lock once
  //    reaped). Returning true is deliberate: it hands control to the caller's
  //    read, which observes EOF and tears the session down. Returning false would
  //    hang the transport on a dead child.
  Expect(wake.PollReadableOrWake(-1, 10),
         "a closed (-1) watched fd must report readable so the caller's read sees EOF");

  // 5. EOF on the watched fd (peer closed) must also report readable via POLLHUP,
  //    for the same reason.
  ::close(data_fds[1]);
  Expect(wake.PollReadableOrWake(data_fds[0], 10),
         "a watched fd at EOF must report readable (POLLHUP), not time out");

  ::close(data_fds[0]);
  wake.Close();
  Expect(wake.read_fd() < 0, "closing the wake pipe should clear its read fd");

  // 6. With the wake pipe closed the helper must still poll the watched fd rather
  //    than dereferencing a -1 wake fd into the poll set.
  int more_fds[2] = {-1, -1};
  Expect(::pipe(more_fds) == 0, "second test pipe should open");
  Expect(::write(more_fds[1], "y", 1) == 1, "second test write should succeed");
  Expect(wake.PollReadableOrWake(more_fds[0], 10),
         "a closed wake pipe must not stop the watched fd from being polled");
  ::close(more_fds[0]);
  ::close(more_fds[1]);
}
#endif  // __unix__ || __APPLE__

}  // namespace

void RegisterWakePipeTests(std::vector<TestCase>& tests) {
#if defined(__unix__) || defined(__APPLE__)
  AddTest(tests, "WakePipe/PollReadableOrWakeContract", TestWakePipePollReadableOrWakeContract);
#else
  (void)tests;
#endif
}

}  // namespace microide::tests
