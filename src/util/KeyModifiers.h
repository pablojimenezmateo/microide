#pragma once

#include <cstdint>

namespace microide::util {

using KeyModifiers = std::uint16_t;

inline constexpr KeyModifiers kKeyModNone = 0;
inline constexpr KeyModifiers kKeyModShift = 1u << 0;
inline constexpr KeyModifiers kKeyModCtrl = 1u << 1;
inline constexpr KeyModifiers kKeyModAlt = 1u << 2;
inline constexpr KeyModifiers kKeyModGui = 1u << 3;

}
