#pragma once

#include <cstddef>

namespace microide::platform {

// The control wire's request-line ceiling, shared by the server that ENFORCES it
// and the client that must not frame past it.
//
// These were two constants of the same name in two files, the client's comment
// claiming to mirror the server's -- at 16x the value. The server sheds a
// connection whose unterminated line passes its ceiling, so the client happily
// framed and sent request lines the server would answer by dropping the socket,
// with nothing on either side saying why. One name, one value, one place.
//
// A single control request (a command or query line) is a few hundred bytes at
// most; the ceiling exists so a hostile local peer streaming bytes with no
// newline cannot grow an unframed read buffer without limit.
inline constexpr std::size_t kMaxControlRequestLineBytes = 1u << 20;  // 1 MiB

}  // namespace microide::platform
