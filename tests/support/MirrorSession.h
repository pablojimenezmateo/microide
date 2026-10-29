#pragma once

// A host tree served by a real `microide-server serve-stdio`, and a mirror of it
// in a different directory: the fixture every MirrorSyncEngine test drives.

#include "TestSupport.h"

#include "project/remote/MirrorStore.h"
#include "project/remote/MirrorSyncEngine.h"
#include "project/remote/RemoteConnection.h"
#include "project/remote/RemoteServerClient.h"
#include "project/remote/RemoteWorkspace.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace microide::tests {

namespace remote = project::remote;

struct MirrorSession {
  TemporaryDirectory temp;
  std::filesystem::path host = temp.path() / "host";
  std::filesystem::path mirror_dir = temp.path() / "mirror";
  std::shared_ptr<remote::RemoteServerClient> client;
  std::shared_ptr<remote::RemoteConnection> connection;
  std::unique_ptr<remote::RemoteWorkspace> workspace;
  std::unique_ptr<remote::MirrorStore> store;
  std::unique_ptr<remote::MirrorSyncEngine> engine;
  std::vector<std::string> materialized;

  MirrorSession() { std::filesystem::create_directories(host); }

  // A new server process for the host tree: its manifest ids start again at 1.
  // With `pid_file`, the server's pid is written there (to stop it from a test).
  std::shared_ptr<remote::RemoteServerClient> StartServer(const std::filesystem::path& pid_file = {}) {
    auto started = remote::RemoteServerClient::Create();
    std::string error;
    const std::vector<std::string> argv =
        pid_file.empty() ? std::vector<std::string>{MICROIDE_SERVER_BINARY, "serve-stdio"}
                         : std::vector<std::string>{"sh", "-c",
                                                    "echo $$ > '" + pid_file.string() + "'; exec '" +
                                                        std::string(MICROIDE_SERVER_BINARY) + "' serve-stdio"};
    Expect(started->ConnectCommand(argv,
                                   remote::HelloRequest{.release = "test", .root = host.string()},
                                   &error),
           "connects: " + error);
    return started;
  }

  void Connect(remote::MirrorSyncEngine::Options options = {}) {
    engine.reset();
    client = StartServer();
    std::string error;
    connection = std::make_shared<remote::RemoteConnection>(client);
    workspace = std::make_unique<remote::RemoteWorkspace>(connection);
    store = std::make_unique<remote::MirrorStore>(mirror_dir);
    Expect(store->Open(&error), "the mirror opens: " + error);
    engine = std::make_unique<remote::MirrorSyncEngine>(
        *workspace, *store, options,
        remote::MirrorSyncEngine::Callbacks{
            .changed = {},
            .materialized =
                [this](const std::vector<std::string>& paths) {
                  materialized.insert(materialized.end(), paths.begin(), paths.end());
                },
        });
  }

  void Sync() {
    engine->RequestSync();
    engine->Flush();
  }

  std::filesystem::path Tree(std::string_view path) const {
    return mirror_dir / "tree" / std::string(path);
  }
};

inline bool WaitFor(const std::function<bool()>& done, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (done()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return done();
}

}  // namespace microide::tests
