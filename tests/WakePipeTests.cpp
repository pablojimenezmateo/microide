#include "TestSupport.h"

#include "util/WakePipe.h"

#include <chrono>
#include <vector>

#include <unistd.h>

namespace microide::tests {
namespace {

void TestWakePipePollReadableOrWakeContract() {
  microide::util::WakePipe wake;
  wake.Open();
  Expect(wake.read_fd() >= 0, "wake pipe should open");

  int data_fds[2] = {-1, -1};
  Expect(::pipe(data_fds) == 0, "test data pipe should open");

  Expect(!wake.PollReadableOrWake(data_fds[0], 10),
         "an idle watched fd must report not-readable so the io loop keeps sweeping");

  Expect(::write(data_fds[1], "x", 1) == 1, "test write should succeed");
  Expect(wake.PollReadableOrWake(data_fds[0], 10),
         "a watched fd with pending data must report readable");
  char scratch = 0;
  Expect(::read(data_fds[0], &scratch, 1) == 1, "test read should drain the byte");

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

  Expect(wake.PollReadableOrWake(-1, 10),
         "a closed (-1) watched fd must report readable so the caller's read sees EOF");

  ::close(data_fds[1]);
  Expect(wake.PollReadableOrWake(data_fds[0], 10),
         "a watched fd at EOF must report readable (POLLHUP), not time out");

  ::close(data_fds[0]);
  wake.Close();
  Expect(wake.read_fd() < 0, "closing the wake pipe should clear its read fd");

  int more_fds[2] = {-1, -1};
  Expect(::pipe(more_fds) == 0, "second test pipe should open");
  Expect(::write(more_fds[1], "y", 1) == 1, "second test write should succeed");
  Expect(wake.PollReadableOrWake(more_fds[0], 10),
         "a closed wake pipe must not stop the watched fd from being polled");
  ::close(more_fds[0]);
  ::close(more_fds[1]);
}

}

void RegisterWakePipeTests(std::vector<TestCase>& tests) {
  AddTest(tests, "WakePipe/PollReadableOrWakeContract", TestWakePipePollReadableOrWakeContract);
}

}
