#pragma once

#include <cstdint>

namespace microide::util {

// Plain keyboard-modifier bitmask. The kernel (the terminal model's mouse and key
// encoders) needs to know which modifiers were held, and must not name the windowing
// library to find out; the shell converts from SDL's own mask at its boundary
// (render::ToKeyModifiers). The bit values are ours, not SDL's, on purpose — a
// numeric coincidence is how a mask silently keeps working through a conversion that
// was never written.
using KeyModifiers = std::uint16_t;

inline constexpr KeyModifiers kKeyModNone = 0;
inline constexpr KeyModifiers kKeyModShift = 1u << 0;
inline constexpr KeyModifiers kKeyModCtrl = 1u << 1;
inline constexpr KeyModifiers kKeyModAlt = 1u << 2;
inline constexpr KeyModifiers kKeyModGui = 1u << 3;

}  // namespace microide::util
