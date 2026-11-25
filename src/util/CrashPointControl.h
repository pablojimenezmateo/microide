#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <signal.h>
#include <unistd.h>

#include "util/CoverageProfile.h"
#include "util/CrashPoint.h"
#include "util/Parse.h"
#include "util/ErrorOut.h"

namespace microide::util {

class CrashPointControl {
 public:
  struct SitePasses {
    std::string site;
    std::uint64_t passes = 0;
    std::vector<std::pair<std::string, std::uint64_t>> files;
  };

  struct Snapshot {
    bool installed = false;
    std::string armed;
    std::uint64_t armed_matches = 0;
    std::vector<SitePasses> sites;
  };

  static constexpr std::size_t kMaxFilesPerSite = 48;

  static bool InstallFromEnvironment() {
    const char* text = std::getenv("MICROIDE_QA_CRASH_POINTS");
    if (text == nullptr) {
      return false;
    }
    Install();
    const std::string_view value(text);
    if (!value.empty() && value != "count" && value != "1") {
      std::string error;
      (void)Arm(value, &error);
    }
    return true;
  }

  static void Install() {
    State& state = Shared();
    {
      const std::lock_guard lock(state.mutex);
      state.installed = true;
    }
    SetCrashPointHook(&OnCrashPoint);
  }

  static bool Installed() {
    State& state = Shared();
    const std::lock_guard lock(state.mutex);
    return state.installed;
  }

  static bool Arm(std::string_view rule, std::string* error) {
    Rule parsed;
    if (!Parse(rule, &parsed, error)) {
      return false;
    }
    State& state = Shared();
    const std::lock_guard lock(state.mutex);
    if (!state.installed) {
      return util::Fail(error, "crash points are not installed (MICROIDE_QA_CRASH_POINTS is unset)");
    }
    state.rule = std::move(parsed);
    state.armed = true;
    state.matches = 0;
    return true;
  }

  static void Disarm() {
    State& state = Shared();
    const std::lock_guard lock(state.mutex);
    state.armed = false;
  }

  static Snapshot Read() {
    State& state = Shared();
    const std::lock_guard lock(state.mutex);
    Snapshot snapshot;
    snapshot.installed = state.installed;
    if (state.armed) {
      snapshot.armed = state.rule.text;
      snapshot.armed_matches = state.matches;
    }
    for (const auto& [site, passes] : state.sites) {
      SitePasses entry;
      entry.site = site;
      entry.passes = passes.total;
      entry.files.assign(passes.files.begin(), passes.files.end());
      snapshot.sites.push_back(std::move(entry));
    }
    return snapshot;
  }

 private:
  enum class Action { Kill, Delay };

  struct Rule {
    std::string text;
    Action action = Action::Kill;
    std::int64_t delay_ms = 0;
    std::string site;
    std::string file;
    std::int64_t nth = 1;
  };

  struct Passes {
    std::uint64_t total = 0;
    std::map<std::string, std::uint64_t> files;
  };

  struct State {
    std::mutex mutex;
    bool installed = false;
    bool armed = false;
    Rule rule;
    std::uint64_t matches = 0;
    std::map<std::string, Passes> sites;
  };

  static State& Shared() {
    static State* const state = new State();
    return *state;
  }

  static bool Fail(std::string* error, std::string message) {
    return util::Fail(error, std::move(message));
  }

  static bool Parse(std::string_view text, Rule* rule, std::string* error) {
    rule->text = std::string(text);
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
      return Fail(error, "expected <action>:<site>[@<detail>][#<nth>]");
    }
    const std::string_view action = text.substr(0, colon);
    std::string_view rest = text.substr(colon + 1);
    if (action == "kill") {
      rule->action = Action::Kill;
    } else if (action.starts_with("delay=")) {
      const auto ms = ParseInt64(action.substr(6));
      if (!ms.has_value() || *ms < 0 || *ms > 60'000) {
        return Fail(error, "delay=<ms> needs 0..60000");
      }
      rule->action = Action::Delay;
      rule->delay_ms = *ms;
    } else {
      return Fail(error, "unknown action \"" + std::string(action) + "\" (kill, delay=<ms>)");
    }
    if (const std::size_t hash = rest.rfind('#'); hash != std::string_view::npos) {
      const auto nth = ParseInt64(rest.substr(hash + 1));
      if (!nth.has_value() || *nth < 0) {
        return Fail(error, "#<nth> needs a count");
      }
      rule->nth = *nth;
      rest = rest.substr(0, hash);
    }
    if (const std::size_t at = rest.find('@'); at != std::string_view::npos) {
      rule->file = std::string(rest.substr(at + 1));
      rest = rest.substr(0, at);
    }
    if (rest.empty()) {
      return Fail(error, "the rule names no site");
    }
    if (rule->action == Action::Kill && rule->nth < 1) {
      return Fail(error, "kill needs #<nth> of 1 or more");
    }
    rule->site = std::string(rest);
    return true;
  }

  static std::string FileNameOf(std::string_view detail) {
    const std::size_t slash = detail.rfind('/');
    std::string_view name = slash == std::string_view::npos ? detail : detail.substr(slash + 1);
    constexpr std::string_view marker = ".microide-staging.";
    if (const std::size_t at = name.find(marker); at != std::string_view::npos && name.starts_with(".")) {
      return std::string(name.substr(1, at - 1)) + " (staging)";
    }
    return std::string(name);
  }

  static void OnCrashPoint(const char* site, std::string_view detail) {
    Action action = Action::Kill;
    std::int64_t delay_ms = 0;
    bool fire = false;
    std::uint64_t pass = 0;
    {
      State& state = Shared();
      const std::lock_guard lock(state.mutex);
      Passes& passes = state.sites[site];
      ++passes.total;
      const std::string file = FileNameOf(detail);
      if (passes.files.contains(file) || passes.files.size() < kMaxFilesPerSite) {
        ++passes.files[file];
      }
      if (state.armed && (state.rule.site == "*" || state.rule.site == site) &&
          (state.rule.file.empty() || state.rule.file == file)) {
        pass = ++state.matches;
        if (state.rule.nth == 0 || static_cast<std::uint64_t>(state.rule.nth) == pass) {
          fire = true;
          action = state.rule.action;
          delay_ms = state.rule.delay_ms;
          if (state.rule.nth != 0) {
            state.armed = false;
          }
        }
      }
    }
    if (!fire) {
      return;
    }
    if (action == Action::Delay) {
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
      return;
    }
    const std::string line = "microide crash-point: killed at " + std::string(site) + " (" +
                             std::string(detail) + "), pass " + std::to_string(pass) + "\n";
    (void)!::write(STDERR_FILENO, line.data(), line.size());
    (void)FlushCoverageProfile();
    ::kill(::getpid(), SIGKILL);
  }
};

}
