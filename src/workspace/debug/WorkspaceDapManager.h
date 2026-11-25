#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "platform/SubprocessSandbox.h"
#include "workspace/debug/DebugSession.h"
#include "workspace/debug/LaunchConfig.h"

namespace microide::workspace {

struct DapSessionInfo {
  int id = 0;
  std::string name;
  DebugSession::State state = DebugSession::State::Inactive;
  bool attention = false;
};

struct PrunedSession {
  int id = 0;
  bool failed = false;
  std::string console_label;
  std::string error;
};

class DapManager {
 public:
  DapManager();
  ~DapManager();
  DapManager(const DapManager&) = delete;
  DapManager& operator=(const DapManager&) = delete;

  void SetWakeChannel(Uint32 event_type);

  void RegisterAdapter(const std::string& type, const platform::ProcessLauncher& launcher,
                       const std::vector<std::string>& command,
                       const platform::SubprocessSandbox& sandbox = {});
  void RetainAdaptersIn(const std::unordered_set<std::string>& types);

  bool HasAdapter(const std::string& type) const;
  std::vector<std::string> AdapterTypes() const;

  struct AdapterInfo {
    std::string type;
    std::vector<std::string> command;
  };
  std::vector<AdapterInfo> AdapterDetails() const;

  int StartSession(const LaunchConfig& config,
                   std::function<DebugSession::Callbacks(int id)> make_callbacks,
                   const std::string& cwd = {});
  int StartSession(const LaunchConfig& config, DebugSession::Callbacks callbacks,
                   const std::string& cwd = {});
  int ReplaceActiveSession(const LaunchConfig& config,
                           std::function<DebugSession::Callbacks(int id)> make_callbacks,
                           const std::string& cwd = {});
  int ReplaceActiveSession(const LaunchConfig& config, DebugSession::Callbacks callbacks,
                           const std::string& cwd = {});

  DebugSession* ActiveSession();
  const DebugSession* ActiveSession() const;
  int ActiveSessionId() const { return active_session_id_; }
  void SetActiveSession(int id);
  DebugSession* SessionById(int id);
  std::size_t SessionCount() const { return sessions_.size(); }
  void SetSessionAttention(int id, bool attention);
  std::vector<DapSessionInfo> Sessions() const;
  std::string SessionLabel(int id) const;

  void StopActiveSession();
  bool ReapExitedSessions();
  std::vector<PrunedSession> PruneTerminated();

  const std::string& LastError() const { return last_error_; }

  void DrainCallbacks();

  void BeginShutdownAll();
  void ShutdownAll();

 private:
  struct AdapterEntry {
    const platform::ProcessLauncher* launcher = &platform::LocalProcessLauncher();
    std::vector<std::string> command;
    platform::SubprocessSandbox sandbox;
  };
  struct SessionEntry {
    int id = 0;
    bool attention = false;
    std::unique_ptr<DebugSession> session;
  };

  std::size_t ActiveIndex() const;
  void ClearActiveEntry();

  Uint32 wake_event_type_ = 0;
  std::unordered_map<std::string, AdapterEntry> adapters_;
  std::vector<SessionEntry> sessions_;
  int active_session_id_ = 0;
  int next_session_id_ = 1;
  std::string last_error_;
};

}
