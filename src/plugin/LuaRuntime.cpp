#include "plugin/LuaRuntime.h"

#if MICROIDE_HAS_LUA_PLUGINS
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <string>

#include "plugin/LuaBudgetedPatterns.h"
#include "plugin/LuaError.h"
#include "util/PerformanceCounters.h"
#include "util/PerformanceTrace.h"
#include "util/ErrorOut.h"
#endif

namespace microide::plugin {

#if MICROIDE_HAS_LUA_PLUGINS
namespace {

constexpr int kHookInstructionBatch = 100000;

constexpr std::size_t kLuaMemoryLimitBytes = 256ull * 1024 * 1024;

[[noreturn]] int LuaPanic(lua_State* state) {
  const char* message = lua_tostring(state, -1);
  throw LuaPanicError(message != nullptr ? message : "unprotected Lua error");
}

}
#endif

std::unique_ptr<LuaRuntime> LuaRuntime::Create(std::string* error_message) {
#if MICROIDE_HAS_LUA_PLUGINS
  auto runtime = std::unique_ptr<LuaRuntime>(new LuaRuntime());
  runtime->memory_budget_.limit = kLuaMemoryLimitBytes;
  runtime->state_ = lua_newstate(&LuaRuntime::BoundedAlloc, &runtime->memory_budget_);
  if (runtime->state_ == nullptr) {
    util::SetError(error_message, "failed to create Lua state");
    return nullptr;
  }
  lua_atpanic(runtime->state_, &LuaPanic);

  luaL_requiref(runtime->state_, "_G", luaopen_base, 1);
  lua_pop(runtime->state_, 1);
  luaL_requiref(runtime->state_, LUA_TABLIBNAME, luaopen_table, 1);
  lua_pop(runtime->state_, 1);
  luaL_requiref(runtime->state_, LUA_STRLIBNAME, luaopen_string, 1);
  lua_pop(runtime->state_, 1);
  InstallBudgetedPatternFunctions(runtime->state_);
  luaL_requiref(runtime->state_, LUA_MATHLIBNAME, luaopen_math, 1);
  lua_pop(runtime->state_, 1);
  luaL_requiref(runtime->state_, LUA_UTF8LIBNAME, luaopen_utf8, 1);
  lua_pop(runtime->state_, 1);
  luaL_requiref(runtime->state_, LUA_LOADLIBNAME, luaopen_package, 1);
  lua_pop(runtime->state_, 1);

  lua_pushnil(runtime->state_);
  lua_setglobal(runtime->state_, "loadfile");
  lua_pushnil(runtime->state_);
  lua_setglobal(runtime->state_, "dofile");

  *static_cast<LuaRuntime**>(lua_getextraspace(runtime->state_)) = runtime.get();
  return runtime;
#else
  (void)error_message;
  return nullptr;
#endif
}

LuaRuntime::~LuaRuntime() {
#if MICROIDE_HAS_LUA_PLUGINS
  if (state_ != nullptr) {
    lua_close(state_);
    state_ = nullptr;
  }
#endif
}

#if MICROIDE_HAS_LUA_PLUGINS
void* LuaRuntime::BoundedAlloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
  auto* budget = static_cast<LuaRuntime::MemoryBudget*>(ud);
  const std::size_t old_size = (ptr == nullptr) ? 0 : osize;

  if (nsize == 0) {
    std::free(ptr);
    budget->used = (budget->used >= old_size) ? budget->used - old_size : 0;
    return nullptr;
  }

  if (nsize > old_size) {
    const std::size_t growth = nsize - old_size;
    if (budget->used + growth > budget->limit) {
      return nullptr;
    }
  }

  void* new_ptr = std::realloc(ptr, nsize);
  if (new_ptr == nullptr) {
    return nullptr;
  }
  budget->used = budget->used - old_size + nsize;
  return new_ptr;
}

bool LuaRuntime::CallBudgetSpent(lua_State* state) {
  const LuaRuntime* runtime = *static_cast<LuaRuntime**>(lua_getextraspace(state));
  return runtime != nullptr && std::chrono::steady_clock::now() > runtime->deadline_;
}

void LuaRuntime::TimeoutHook(lua_State* state, lua_Debug* ) {
  if (!CallBudgetSpent(state)) {
    return;
  }
  lua_error_util::PushMessage(state, kTimeBudgetMessage);
  lua_error(state);
}

namespace {

bool ReportPCallStatus(lua_State* state, int status, std::string* error_message) {
  if (status == LUA_OK) {
    util::ClearError(error_message);
    return true;
  }
  util::AddPerformanceCounter(util::PerfCounterId::PluginLuaCallbackErrors);
  if (error_message != nullptr) {
    const char* raw = lua_tostring(state, -1);
    *error_message = raw != nullptr ? std::string(raw) : std::string("unknown Lua error");
  }
  lua_pop(state, 1);
  return false;
}

}

bool LuaRuntime::PCall(int nargs, int nresults, std::string* error_message) const {
  if (state_ == nullptr) {
    return util::Fail(error_message, "Lua runtime unavailable");
  }

  util::PerformanceTrace::Scope perf_scope("plugin::LuaPCall");
  util::AddPerformanceCounter(util::PerfCounterId::PluginLuaCallbackDispatches);

  deadline_ = std::chrono::steady_clock::now() + call_budget_;
  lua_sethook(state_, &LuaRuntime::TimeoutHook, LUA_MASKCOUNT, kHookInstructionBatch);
  const int status = lua_pcall(state_, nargs, nresults, 0);
  lua_sethook(state_, nullptr, 0, 0);

  return ReportPCallStatus(state_, status, error_message);
}

bool LuaRuntime::PCallNested(int nargs, int nresults, std::string* error_message) const {
  if (state_ == nullptr) {
    return util::Fail(error_message, "Lua runtime unavailable");
  }

  util::PerformanceTrace::Scope perf_scope("plugin::LuaPCallNested");
  util::AddPerformanceCounter(util::PerfCounterId::PluginLuaCallbackDispatches);

  const std::chrono::steady_clock::time_point outer_deadline = deadline_;
  const lua_Hook outer_hook = lua_gethook(state_);
  const int outer_mask = lua_gethookmask(state_);
  const int outer_count = lua_gethookcount(state_);

  deadline_ = std::chrono::steady_clock::now() + call_budget_;
  lua_sethook(state_, &LuaRuntime::TimeoutHook, LUA_MASKCOUNT, kHookInstructionBatch);
  const int status = lua_pcall(state_, nargs, nresults, 0);
  lua_sethook(state_, outer_hook, outer_mask, outer_count);
  deadline_ = outer_deadline;

  return ReportPCallStatus(state_, status, error_message);
}
#endif

}
