#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

#if MICROIDE_HAS_LUA_PLUGINS
#include <lua.hpp>
#endif

namespace microide::plugin {

class LuaPanicError : public std::runtime_error {
 public:
  explicit LuaPanicError(const std::string& message) : std::runtime_error(message) {}
};

class LuaRuntime {
 public:
  static std::unique_ptr<LuaRuntime> Create(std::string* error_message);
  ~LuaRuntime();

  LuaRuntime(const LuaRuntime&) = delete;
  LuaRuntime& operator=(const LuaRuntime&) = delete;

#if MICROIDE_HAS_LUA_PLUGINS
  lua_State* state() const { return state_; }

  bool PCall(int nargs, int nresults, std::string* error_message) const;

  bool PCallNested(int nargs, int nresults, std::string* error_message) const;

  void set_call_budget(std::chrono::steady_clock::duration budget) { call_budget_ = budget; }

  static bool CallBudgetSpent(lua_State* state);
  static constexpr const char* kTimeBudgetMessage =
      "plugin call exceeded its time budget (possible infinite loop)";
#endif

 private:
  LuaRuntime() = default;

#if MICROIDE_HAS_LUA_PLUGINS
  static void TimeoutHook(lua_State* state, lua_Debug* ar);

  static void* BoundedAlloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize);

  struct MemoryBudget {
    std::size_t used = 0;
    std::size_t limit = 0;
  };
  MemoryBudget memory_budget_;

  lua_State* state_ = nullptr;
  std::chrono::steady_clock::duration call_budget_ = std::chrono::milliseconds(750);
  mutable std::chrono::steady_clock::time_point deadline_{};
#endif
};

}
