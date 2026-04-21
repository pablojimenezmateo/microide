#pragma once

#include "workspace/WorkspaceLspClient.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace microide::workspace {

// Debug adapter protocol manager — manages debugger connections.
// DAP is similar to LSP but for debugging: initialize, launch/attach, breakpoints, etc.
class DapManager {
 public:
  struct DebugConfiguration {
    std::string name;
    std::string type;  // e.g., "lldb", "python", "node"
    std::string request;  // "launch" or "attach"
    std::vector<std::string> command;
    std::string cwd;
    std::unordered_map<std::string, std::string> env;
  };

  DapManager();
  ~DapManager();
  DapManager(const DapManager&) = delete;
  DapManager& operator=(const DapManager&) = delete;

  // Register a debugger for a language or debug type.
  void RegisterDebugger(const std::string& type, const std::vector<std::string>& command);

  // Get or start debugger for type; returns nullptr if not registered.
  LspClient* GetDebugger(const std::string& type);

  // True if debugger is running for type.
  bool IsDebuggerRunning(const std::string& type) const;

  // Stop all debuggers.
  void ShutdownAll();

 private:
  struct DebuggerEntry {
    std::vector<std::string> command;
    std::unique_ptr<LspClient> client;
  };

  std::unordered_map<std::string, DebuggerEntry> debuggers_;
};

}  // namespace microide::workspace
