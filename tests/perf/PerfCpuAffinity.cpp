#include "perf/PerfCpuAffinity.h"

#include "util/Parse.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

namespace microide::tests::perf {
namespace {

#if defined(__linux__)

std::optional<std::uint64_t> ReadCpuMaxFrequencyKhz(int cpu) {
  const std::filesystem::path path = std::filesystem::path("/sys/devices/system/cpu") /
                                     ("cpu" + std::to_string(cpu)) / "cpufreq" /
                                     "cpuinfo_max_freq";
  std::ifstream in(path);
  if (!in) {
    return std::nullopt;
  }
  std::string text;
  if (!std::getline(in, text)) {
    return std::nullopt;
  }
  const auto parsed = util::ParseSize(text);
  if (!parsed.has_value() || *parsed == 0) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(*parsed);
}

// "0-3,12-15" -> {0,1,2,3,12,13,14,15}. Returns empty on anything malformed so a
// typo cannot silently pin to a single core.
std::vector<int> ParseCpuList(const std::string& text) {
  std::vector<int> cpus;
  std::size_t at = 0;
  while (at <= text.size()) {
    const std::size_t comma = text.find(',', at);
    const std::string token =
        text.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
    if (token.empty()) {
      return {};
    }
    const std::size_t dash = token.find('-');
    if (dash == std::string::npos) {
      const auto value = util::ParseInt(token);
      if (!value.has_value() || *value < 0) {
        return {};
      }
      cpus.push_back(*value);
    } else {
      const auto low = util::ParseInt(token.substr(0, dash));
      const auto high = util::ParseInt(token.substr(dash + 1));
      if (!low.has_value() || !high.has_value() || *low < 0 || *high < *low) {
        return {};
      }
      for (int cpu = *low; cpu <= *high; ++cpu) {
        cpus.push_back(cpu);
      }
    }
    if (comma == std::string::npos) {
      break;
    }
    at = comma + 1;
  }
  return cpus;
}

std::string FormatCpuList(const std::vector<int>& cpus) {
  std::string text;
  for (std::size_t i = 0; i < cpus.size();) {
    std::size_t run_end = i;
    while (run_end + 1 < cpus.size() && cpus[run_end + 1] == cpus[run_end] + 1) {
      ++run_end;
    }
    if (!text.empty()) {
      text += ',';
    }
    text += std::to_string(cpus[i]);
    if (run_end > i) {
      text += '-';
      text += std::to_string(cpus[run_end]);
    }
    i = run_end + 1;
  }
  return text;
}

bool PinTo(const std::vector<int>& cpus) {
  if (cpus.empty()) {
    return false;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  for (const int cpu : cpus) {
    if (cpu >= 0 && cpu < CPU_SETSIZE) {
      CPU_SET(cpu, &set);
    }
  }
  return sched_setaffinity(0, sizeof(set), &set) == 0;
}

#endif  // defined(__linux__)

}  // namespace

CpuAffinityPlan ApplyPerfCpuAffinity(const std::string& request) {
  if (request == "off") {
    return CpuAffinityPlan{.description = "off", .pinned = false};
  }

#if !defined(__linux__)
  return CpuAffinityPlan{.description = "unsupported platform", .pinned = false};
#else
  if (request != "auto") {
    const std::vector<int> cpus = ParseCpuList(request);
    if (cpus.empty()) {
      return CpuAffinityPlan{.description = "invalid cpu list: " + request, .pinned = false};
    }
    if (!PinTo(cpus)) {
      return CpuAffinityPlan{.description = "sched_setaffinity failed for " + request,
                             .pinned = false};
    }
    return CpuAffinityPlan{.description = FormatCpuList(cpus) + " (explicit)", .pinned = true};
  }

  // Group every online CPU by its advertised maximum frequency. A machine with a
  // single group is homogeneous and needs no pinning; pinning it anyway would
  // just shrink the pool the background executors run on.
  cpu_set_t online;
  CPU_ZERO(&online);
  if (sched_getaffinity(0, sizeof(online), &online) != 0) {
    return CpuAffinityPlan{.description = "sched_getaffinity failed", .pinned = false};
  }
  std::map<std::uint64_t, std::vector<int>> by_frequency;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (!CPU_ISSET(cpu, &online)) {
      continue;
    }
    const auto khz = ReadCpuMaxFrequencyKhz(cpu);
    if (!khz.has_value()) {
      // No cpufreq data (a VM, a kernel without the driver): treat the machine as
      // homogeneous rather than guessing.
      return CpuAffinityPlan{.description = "no cpufreq data (no pinning)", .pinned = false};
    }
    by_frequency[*khz].push_back(cpu);
  }
  if (by_frequency.size() <= 1) {
    return CpuAffinityPlan{.description = "homogeneous (no pinning needed)", .pinned = false};
  }
  const auto& fastest = *by_frequency.rbegin();
  if (!PinTo(fastest.second)) {
    return CpuAffinityPlan{.description = "sched_setaffinity failed", .pinned = false};
  }
  return CpuAffinityPlan{
      .description = FormatCpuList(fastest.second) + " (" +
                     std::to_string(fastest.second.size()) + " cpus @ " +
                     std::to_string(fastest.first / 1000) + " MHz)",
      .pinned = true};
#endif
}

}  // namespace microide::tests::perf
