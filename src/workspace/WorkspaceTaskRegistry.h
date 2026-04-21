#pragma once

#include <string>
#include <vector>

namespace microide::workspace {

// Runnable task: declared by plugin, executed via subprocess or Lua.
struct TaskSpec {
  std::string id;
  std::string plugin_id;
  std::string label;
  std::string group;          // e.g., "build" or "test"
  std::vector<std::string> command;  // subprocess command
  std::string cwd;            // working directory
  bool run_in_shell = false;  // if true, wrap in sh -c
};

// Registry for runnable tasks.
class TaskRegistry {
 public:
  TaskRegistry();
  ~TaskRegistry();

  void Register(const TaskSpec& spec);
  const std::vector<TaskSpec>& Specs() const { return specs_; }

  // Find task by id.
  const TaskSpec* FindTask(const std::string& id) const;

 private:
  std::vector<TaskSpec> specs_;
};

}  // namespace microide::workspace
