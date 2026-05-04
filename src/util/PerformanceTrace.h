#pragma once

#include <chrono>
#include <string>
#include <string_view>

namespace microide::util {

class PerformanceTrace {
 public:
  static bool Enabled();
  static bool FlagEnabled(const char* env_name);
  static double MinimumDurationMs();

  class Scope {
   public:
    explicit Scope(std::string_view label);
    ~Scope();

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    std::string label_;
    std::chrono::steady_clock::time_point start_{};
    int depth_ = 0;
    bool enabled_ = false;
  };
};

}  // namespace microide::util
