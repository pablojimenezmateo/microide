#include "perf/PerfHarness.h"

#include "platform/Subprocess.h"
#include "platform/UnixSocket.h"
#include "project/remote/RemoteConnection.h"
#include "project/remote/RemotePathMap.h"
#include "project/remote/RemoteProcessLauncher.h"
#include "project/remote/RemoteServerClient.h"
#include "project/remote/RemoteServerPaths.h"
#include "project/remote/RemoteWorkspace.h"
#include "support/DelayRelay.h"
#include "terminal/TerminalSession.h"

#include <stdlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace microide::tests::perf {
namespace {

namespace remote = project::remote;
using Clock = std::chrono::steady_clock;
using terminal::TerminalSession;

constexpr std::chrono::milliseconds kRtt{60};
constexpr std::uint64_t kLinkBytesPerSecond = 12'500'000;
constexpr std::uint64_t kBulkBytes = 50ull * 1024 * 1024;

double Ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

bool Poll(const std::function<bool()>& done, std::chrono::milliseconds timeout) {
  const auto deadline = Clock::now() + timeout;
  while (!done()) {
    if (Clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  return true;
}

class HostServer {
 public:
  static HostServer& Get() { return *current_; }
  HostServer(const HostServer&) = delete;
  HostServer& operator=(const HostServer&) = delete;
  ~HostServer() {
    current_ = nullptr;
    if (ok_) {
      (void)platform::RunSubprocess({MICROIDE_SERVER_BINARY, "stop", "--socket-dir", socket_dir_.string()},
                                    platform::SubprocessOptions{.timeout_ms = 10000});
    }
    std::error_code ec;
    if (!base_.empty()) {
      std::filesystem::remove_all(base_, ec);
    }
  }
  const std::filesystem::path& root() const { return root_; }
  const std::filesystem::path& socket_dir() const { return socket_dir_; }
  bool ok() const { return ok_; }

  HostServer() {
    current_ = this;
    std::string pattern = (std::filesystem::temp_directory_path() / "mip-perf.XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (::mkdtemp(buffer.data()) == nullptr) {
      return;
    }
    base_ = buffer.data();
    socket_dir_ = base_ / "s";
    root_ = base_ / "root";
    std::filesystem::create_directories(root_);
    std::ofstream out(root_ / "bulk.bin", std::ios::binary);
    std::mt19937_64 random(42);
    std::vector<std::uint64_t> block(1 << 16);
    for (std::uint64_t written = 0; written < kBulkBytes; written += block.size() * 8) {
      for (auto& word : block) {
        word = random();
      }
      out.write(reinterpret_cast<const char*>(block.data()),
                static_cast<std::streamsize>(block.size() * 8));
    }
    out.close();
    ok_ = platform::RunSubprocess({MICROIDE_SERVER_BINARY, "start", "--socket-dir", socket_dir_.string()},
                                  platform::SubprocessOptions{.timeout_ms = 20000})
              .success();
  }

 private:
  static inline HostServer* current_ = nullptr;
  std::filesystem::path base_;
  std::filesystem::path socket_dir_;
  std::filesystem::path root_;
  bool ok_ = false;
};

struct Link {
  std::unique_ptr<DelayRelay> relay;
  std::shared_ptr<remote::RemoteServerClient> client;
};

Link Connect(std::uint64_t bytes_per_second = kLinkBytesPerSecond) {
  HostServer& server = HostServer::Get();
  Link link;
  link.relay = std::make_unique<DelayRelay>(
      platform::ConnectUnixSocket(remote::ServerSocketPath(server.socket_dir())), kRtt,
      bytes_per_second);
  link.client = remote::RemoteServerClient::Create();
  const int fd = link.relay->TakeClientFd();
  std::string error;
  if (!link.client->ConnectFds(fd, fd,
                               remote::HelloRequest{.release = "perf",
                                                    .root = server.root().string()},
                               &error)) {
    throw std::runtime_error("remote perf: the client did not connect: " + error);
  }
  return link;
}

std::string CursorLineText(const TerminalSession& session) {
  const auto lines = session.SnapshotLines();
  std::string text;
  for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
    std::string line;
    for (const auto& cell : it->cells) {
      line += cell.DisplayText().empty() ? std::string(" ") : std::string(cell.DisplayText());
    }
    while (!line.empty() && line.back() == ' ') {
      line.pop_back();
    }
    if (!line.empty()) {
      return line;
    }
  }
  return text;
}

struct HostTerminal {
  std::shared_ptr<remote::RemoteConnection> connection;
  std::unique_ptr<remote::RemoteProcessLauncher> launcher;
  TerminalSession session;

  explicit HostTerminal(const std::shared_ptr<remote::RemoteConnection>& conn)
      : connection(conn),
        launcher(std::make_unique<remote::RemoteProcessLauncher>(
            conn, remote::RemotePathMap(HostServer::Get().root(), HostServer::Get().root()),
            remote::RemoteProcessLauncher::Options{.host_paths_readable_locally = true})) {
    session.SetPredictionMode(terminal::TerminalPredictionOverlay::Mode::Always);
    if (!session.Start(*launcher, HostServer::Get().root(), {}, "sh")) {
      throw std::runtime_error("remote perf: the host terminal did not start");
    }
    session.SendBytes("PS1='rea''dy> '\n");
    if (!Poll([&] { return CursorLineText(session) == "ready>"; }, std::chrono::seconds(10))) {
      throw std::runtime_error("remote perf: no prompt: " + CursorLineText(session));
    }
  }
  ~HostTerminal() { session.Stop(); }

  void Type(char32_t c) {
    TerminalSession::KeyPress press;
    press.key = TerminalSession::KeyPress::Key::Char;
    press.codepoint = c;
    (void)session.SendKeyPress(press);
  }
  double TypeAndConfirm(char32_t c, const std::string& expected) {
    const auto start = Clock::now();
    Type(c);
    if (!Poll([&] { return session.PendingPredictions() == 0 && CursorLineText(session) == expected; },
              std::chrono::seconds(10))) {
      throw std::runtime_error("remote perf: never confirmed; the line reads '" +
                               CursorLineText(session) + "', expected '" + expected + "'");
    }
    return Ms(Clock::now() - start);
  }
  void ClearLine() {
    session.SendBytes("\x15");
    Poll([&] { return CursorLineText(session) == "ready>"; }, std::chrono::seconds(10));
  }
};

double Percentile(std::vector<double> samples, double p) {
  std::sort(samples.begin(), samples.end());
  const std::size_t index = std::min(samples.size() - 1,
                                     static_cast<std::size_t>(p * static_cast<double>(samples.size())));
  return samples[index];
}

void Require(bool ok, const std::string& what) {
  if (!ok) {
    throw std::runtime_error(what);
  }
}

bool ServerUp(ScenarioContext& context) {
  if (!HostServer::Get().ok()) {
    context.SkipScenario("microide-server did not start");
    return false;
  }
  return true;
}

void RunRemoteFirstPrompt(ScenarioContext& context) {
  const HostServer server;
  if (!ServerUp(context)) {
    return;
  }
  const double ms = context.Measure("remote.first_prompt", [&] {
    // perf-measure-waits-on-clock: connect-to-prompt latency over the simulated link is the measurement
    Link link = Connect();
    HostTerminal terminal(std::make_shared<remote::RemoteConnection>(link.client));
  });
  std::fprintf(stderr, "[perf][remote_first_prompt] %.1fms (%.1f RTT)\n", ms, ms / Ms(kRtt));
}

void RunRemoteEchoUnderBulk(ScenarioContext& context) {
  const HostServer server;
  if (!ServerUp(context)) {
    return;
  }
  Link link = Connect();
  auto connection = std::make_shared<remote::RemoteConnection>(link.client);
  HostTerminal terminal(connection);
  remote::RemoteWorkspace workspace(connection);
  const std::filesystem::path staging = HostServer::Get().root().parent_path() / "staging.bin";
  std::vector<double> idle;
  {
    std::string line = "ready> ";
    for (int i = 0; i < 10; ++i) {
      line += 'x';
      idle.push_back(terminal.TypeAndConfirm(U'x', line));
    }
    terminal.ClearLine();
  }
  std::vector<double> echoes;
  context.Measure("remote.echo_under_bulk", [&] {
    // perf-measure-waits-on-clock: each wait is one keystroke-to-echo sample under a busy bulk lane
    std::atomic_bool fetched{false};
    const auto fetch_started = Clock::now();
    workspace.FetchObjectToFile("bulk.bin", staging, remote::Lane::Bulk,
                                [&](remote::RemoteWorkspace::FetchedObject, std::string) {
                                  fetched = true;
                                });
    std::this_thread::sleep_for(kRtt * 2);
    std::string line = "ready> ";
    int typed = 0;
    while (!fetched && typed < 40) {
      line += 'x';
      echoes.push_back(terminal.TypeAndConfirm(U'x', line));
      ++typed;
      if (typed % 10 == 0) {
        terminal.ClearLine();
        line = "ready> ";
      }
    }
    Poll([&] { return fetched.load(); }, std::chrono::seconds(60));
    const double transfer_ms = Ms(Clock::now() - fetch_started);
    std::fprintf(stderr, "[perf][remote_echo_under_bulk] 50 MiB in %.0f ms = %.1f Mbit/s (link %.0f)\n",
                 transfer_ms, static_cast<double>(kBulkBytes) * 8 / transfer_ms / 1000.0,
                 static_cast<double>(kLinkBytesPerSecond) * 8 / 1e6);
    terminal.ClearLine();
  });
  if (std::getenv("MICROIDE_PERF_REMOTE_SAMPLES") != nullptr) {
    for (const double e : echoes) {
      std::fprintf(stderr, "%.1f ", e);
    }
    std::fprintf(stderr, "\n");
  }
  Require(echoes.size() >= 8, "remote_echo_under_bulk: too few samples taken during the transfer");
  const double chunk_ms = 1000.0 * static_cast<double>(64 * 1024) / static_cast<double>(kLinkBytesPerSecond);
  constexpr double kMeasurementToleranceMs = 2.0;
  const double budget = Ms(kRtt) + 5.0 + chunk_ms + kMeasurementToleranceMs;
  const double p50 = Percentile(echoes, 0.5);
  const double p95 = Percentile(echoes, 0.95);
  std::fprintf(stderr,
               "[perf][remote_echo_under_bulk] idle p50=%.1fms | under bulk samples=%zu p50=%.1fms "
               "p95=%.1fms budget p50<=%.1fms p95<=%.1fms\n",
               Percentile(idle, 0.5), echoes.size(), p50, p95, budget, budget + 16.0);
  Require(p50 <= budget, "remote_echo_under_bulk: p50 echo " + std::to_string(p50) +
                             " ms exceeds 1 RTT + 5 ms + one bulk chunk");
  Require(p95 <= budget + 16.0, "remote_echo_under_bulk: p95 echo " + std::to_string(p95) +
                                    " ms exceeds 1 RTT + 5 ms + one bulk chunk + one frame");
}

void RunRemoteContradictedPrediction(ScenarioContext& context) {
  const HostServer server;
  if (!ServerUp(context)) {
    return;
  }
  Link link = Connect();
  HostTerminal terminal(std::make_shared<remote::RemoteConnection>(link.client));
  terminal.session.SendBytes("stty -echo\n");
  std::this_thread::sleep_for(kRtt * 3);
  std::vector<double> gone;
  context.Measure("remote.contradicted_prediction", [&] {
    // perf-measure-waits-on-clock: how long a contradicted prediction stays on screen is the measurement
    gone.push_back(terminal.TypeAndConfirm(U'z', "ready>"));
  });
  terminal.ClearLine();
  const double worst = *std::max_element(gone.begin(), gone.end());
  const double budget = Ms(kRtt) + 50.0 + 16.0;
  std::fprintf(stderr, "[perf][remote_contradicted_prediction] worst=%.1fms budget=%.1fms\n", worst,
               budget);
  Require(Percentile(gone, 0.5) <= budget,
          "remote_contradicted_prediction: a contradicted prediction outlived 1 RTT + 50 ms");
}

void RunRemoteReattachThree(ScenarioContext& context) {
  const HostServer server;
  if (!ServerUp(context)) {
    return;
  }
  Link link = Connect();
  auto connection = std::make_shared<remote::RemoteConnection>(link.client);
  std::vector<std::unique_ptr<HostTerminal>> terminals;
  for (int i = 0; i < 3; ++i) {
    terminals.push_back(std::make_unique<HostTerminal>(connection));
  }
  std::vector<double> samples;
  context.Measure("remote.reattach_three", [&] {
    // perf-measure-waits-on-clock: hello to all three terminals mirrored again is the measurement
    link.client->peer().Fail("perf: link dropped");
    Poll([&] {
      return std::all_of(terminals.begin(), terminals.end(),
                         [](const auto& t) { return !t->session.running(); });
    }, std::chrono::seconds(5));
    const auto start = Clock::now();
    Link next = Connect();
    connection->Replace(next.client);
    Require(Poll([&] {
              return std::all_of(terminals.begin(), terminals.end(),
                                 [](const auto& t) { return t->session.running(); });
            }, std::chrono::seconds(10)),
            "remote_reattach_three: the terminals did not come back");
    samples.push_back(Ms(Clock::now() - start));
    link = std::move(next);
  });
  const double p50 = Percentile(samples, 0.5);
  std::fprintf(stderr, "[perf][remote_reattach_three] p50=%.1fms budget=%.1fms\n", p50,
               Ms(kRtt) * 2 + 20.0);
  Require(p50 <= Ms(kRtt) * 2 + 20.0, "remote_reattach_three: reattach took more than 2 RTT");
}

void RunRemoteDeadLink(ScenarioContext& context) {
  const HostServer server;
  if (!ServerUp(context)) {
    return;
  }
  std::vector<double> samples;
  context.Measure("remote.dead_link", [&] {
    // perf-measure-waits-on-clock: time for a silent link to be declared dead is the measurement
    Link link = Connect();
    const auto start = Clock::now();
    link.relay->Blackhole();
    Require(Poll([&] { return !link.client->connected(); }, std::chrono::seconds(15)),
            "remote_dead_link: a silent link was never declared dead");
    samples.push_back(Ms(Clock::now() - start));
  });
  const double worst = *std::max_element(samples.begin(), samples.end());
  std::fprintf(stderr, "[perf][remote_dead_link] worst=%.0fms budget=8000ms\n", worst);
  Require(worst <= 8000.0, "remote_dead_link: a dead link took more than 6 s + one ping interval");
}

void RunRemoteStalledServerInput(ScenarioContext& context) {
  const HostServer server;
  if (!ServerUp(context)) {
    return;
  }
  Link link = Connect();
  HostTerminal terminal(std::make_shared<remote::RemoteConnection>(link.client));
  double worst = 0.0;
  context.Measure("remote.stalled_server_input", [&] {
    link.relay->SetServerStalled(true);
    for (int i = 0; i < 50; ++i) {
      const auto start = Clock::now();
      terminal.Type(U'k');
      (void)terminal.session.SnapshotLines();
      worst = std::max(worst, Ms(Clock::now() - start));
    }
    link.relay->SetServerStalled(false);
  });
  terminal.ClearLine();
  std::fprintf(stderr, "[perf][remote_stalled_server_input] worst=%.2fms budget=16ms\n", worst);
  Require(worst < 16.0, "remote_stalled_server_input: a keystroke blocked on a stalled server");
}

const ScenarioRegistration g_remote_first_prompt({Scenario{
    .name = "remote_first_prompt",
    .smoke = false,
    .baseline_gated = false,
    .run_by_default = false,
    .run = RunRemoteFirstPrompt,
}});
const ScenarioRegistration g_remote_echo_under_bulk({Scenario{
    .name = "remote_echo_under_bulk",
    .smoke = false,
    .baseline_gated = false,
    .run_by_default = false,
    .run = RunRemoteEchoUnderBulk,
}});
const ScenarioRegistration g_remote_contradicted_prediction({Scenario{
    .name = "remote_contradicted_prediction",
    .smoke = false,
    .baseline_gated = false,
    .run_by_default = false,
    .run = RunRemoteContradictedPrediction,
}});
const ScenarioRegistration g_remote_reattach_three({Scenario{
    .name = "remote_reattach_three",
    .smoke = false,
    .baseline_gated = false,
    .run_by_default = false,
    .run = RunRemoteReattachThree,
}});
const ScenarioRegistration g_remote_dead_link({Scenario{
    .name = "remote_dead_link",
    .smoke = false,
    .baseline_gated = false,
    .run_by_default = false,
    .run = RunRemoteDeadLink,
}});
const ScenarioRegistration g_remote_stalled_server_input({Scenario{
    .name = "remote_stalled_server_input",
    .smoke = false,
    .baseline_gated = false,
    .run_by_default = false,
    .run = RunRemoteStalledServerInput,
}});

}
}

