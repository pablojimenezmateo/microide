#include "perf/ScenarioProcessIsolation.h"

#include "perf/AllocationCounter.h"
#include "perf/ScenarioAggregateWire.h"

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#define MICROIDE_PERF_CAN_FORK 1

namespace microide::tests::perf {

namespace {

void AppendU64(std::string& out, std::uint64_t value) {
  char bytes[8];
  for (int i = 0; i < 8; ++i) {
    bytes[i] = static_cast<char>((value >> (8 * i)) & 0xFFu);
  }
  out.append(bytes, sizeof(bytes));
}

std::uint64_t ReadU64(std::string_view bytes) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8 && static_cast<std::size_t>(i) < bytes.size(); ++i) {
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[static_cast<std::size_t>(i)]))
             << (8 * i);
  }
  return value;
}

#if MICROIDE_PERF_CAN_FORK
bool WriteAll(int fd, std::string_view bytes) {
  while (!bytes.empty()) {
    const ssize_t written = ::write(fd, bytes.data(), bytes.size());
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    bytes.remove_prefix(static_cast<std::size_t>(written));
  }
  return true;
}

bool ReadAll(int fd, std::string* out) {
  char buffer[16384];
  while (true) {
    const ssize_t got = ::read(fd, buffer, sizeof(buffer));
    if (got == 0) {
      return true;
    }
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    out->append(buffer, static_cast<std::size_t>(got));
  }
}
#endif

}

bool ScenarioProcessIsolationAvailable() { return MICROIDE_PERF_CAN_FORK != 0; }

std::optional<Aggregate> RunScenarioInChildProcess(const Scenario& scenario,
                                                   const PerfHarness::RunOptions& options,
                                                   bool* selected, std::string* error) {
  const auto fail = [&](std::string message) -> std::optional<Aggregate> {
    if (error != nullptr) {
      *error = std::move(message);
    }
    return std::nullopt;
  };
  if (selected != nullptr) {
    *selected = true;
  }

#if !MICROIDE_PERF_CAN_FORK
  (void)scenario;
  (void)options;
  return fail("per-scenario process isolation is not available on this platform");
#else
  int fds[2] = {-1, -1};
  if (::pipe2(fds, O_CLOEXEC) != 0) {
    return fail(std::string("pipe2 failed: ") + std::strerror(errno));
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    const std::string message = std::string("fork failed: ") + std::strerror(errno);
    ::close(fds[0]);
    ::close(fds[1]);
    return fail(message);
  }

  if (pid == 0) {
    ::close(fds[0]);
    std::string header;
    std::string payload;
    const std::optional<Aggregate> aggregate = PerfHarness::RunScenario(scenario, options);
    if (aggregate.has_value()) {
      AppendU64(header, 1);
      payload = EncodeScenarioAggregate(*aggregate);
    } else {
      const std::string message = PerfHarness::LastError();
      AppendU64(header, message.empty() ? 2 : 0);
      AppendU64(payload, message.size());
      payload.append(message);
    }
    Allocations::DumpTracedAllocationSites();
    const bool wrote = WriteAll(fds[1], header) && WriteAll(fds[1], payload);
    ::close(fds[1]);
    ::_exit(wrote ? 0 : 1);
  }

  ::close(fds[1]);
  std::string bytes;
  const bool read_ok = ReadAll(fds[0], &bytes);
  ::close(fds[0]);

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return fail(std::string("waitpid failed: ") + std::strerror(errno));
    }
  }

  if (!read_ok) {
    return fail("failed to read the scenario child's report");
  }
  if (WIFSIGNALED(status)) {
    return fail("scenario child died on signal " + std::to_string(WTERMSIG(status)));
  }
  if (bytes.size() < 8) {
    return fail("scenario child produced no report (exit status " +
                std::to_string(WEXITSTATUS(status)) + ")");
  }

  const std::uint64_t kind = ReadU64(std::string_view(bytes).substr(0, 8));
  const std::string_view body = std::string_view(bytes).substr(8);
  if (kind == 1) {
    std::optional<Aggregate> decoded = DecodeScenarioAggregate(body);
    if (!decoded.has_value()) {
      return fail("scenario child's report was truncated or malformed");
    }
    return decoded;
  }
  std::string text;
  if (body.size() >= 8) {
    const std::uint64_t length = ReadU64(body.substr(0, 8));
    if (length <= body.size() - 8) {
      text.assign(body.substr(8, static_cast<std::size_t>(length)));
    }
  }
  if (kind == 2) {
    if (selected != nullptr) {
      *selected = false;
    }
    if (error != nullptr) {
      error->clear();
    }
    return std::nullopt;
  }
  return fail(text.empty() ? std::string("scenario failed in child process") : text);
#endif
}

}
