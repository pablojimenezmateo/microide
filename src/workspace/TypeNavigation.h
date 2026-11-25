#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace microide::workspace {

struct TypeNavigationState {
  std::string prefix;
  std::uint64_t last_key_ns = 0;
};

inline constexpr std::uint64_t kTypeNavigationResetNs = 1'000'000'000;

template <typename LabelOf>
std::optional<std::size_t> TypeNavigate(TypeNavigationState& state,
                                        char typed,
                                        std::uint64_t now_ns,
                                        std::size_t current,
                                        std::size_t count,
                                        LabelOf&& label) {
  if (count == 0) {
    return std::nullopt;
  }
  if (now_ns - state.last_key_ns > kTypeNavigationResetNs || now_ns < state.last_key_ns) {
    state.prefix.clear();
  }
  state.last_key_ns = now_ns;
  const char lowered = static_cast<char>(std::tolower(static_cast<unsigned char>(typed)));
  state.prefix.push_back(lowered);
  const bool repeated =
      state.prefix.find_first_not_of(state.prefix.front()) == std::string::npos;
  const std::string_view wanted =
      repeated ? std::string_view(state.prefix).substr(0, 1) : std::string_view(state.prefix);
  const auto matches = [&](std::size_t index) {
    const std::string_view name = label(index);
    if (name.size() < wanted.size()) {
      return false;
    }
    for (std::size_t i = 0; i < wanted.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(name[i])) != static_cast<unsigned char>(wanted[i])) {
        return false;
      }
    }
    return true;
  };
  const std::size_t start = (repeated && state.prefix.size() > 1) || state.prefix.size() == 1
                                ? current + 1
                                : current;
  for (std::size_t step = 0; step < count; ++step) {
    const std::size_t index = (start + step) % count;
    if (matches(index)) {
      return index;
    }
  }
  return std::nullopt;
}

}
