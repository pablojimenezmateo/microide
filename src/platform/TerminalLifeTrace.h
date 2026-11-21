#pragma once

#include <string_view>

namespace microide::platform {

// A terminal's shell and pty in the app log, with MICROIDE_TRACE_TERMINAL_LIFE (on
// in QA runs): `start` with the shell's pid and the code that started it, and
// `stop` when the backend is stopped (its destructor stops it). The leak oracle
// found 70 interactive shells, their reader threads and their ptys alive under
// five open panes, and nothing said which code made the backends nobody stopped
// (soak 2d33d0bc377e, 5853a23b19a7, 83e5786c4590). Raw frames, as the damage
// trace prints them; tools/qa symbolizes those of the children still alive into
// the evidence (children-origin.txt). Free when the flag is off.
void TraceTerminalLife(std::string_view event, int pid, bool with_stack);

}  // namespace microide::platform
