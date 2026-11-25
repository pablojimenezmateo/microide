#pragma once

#include <string_view>

namespace microide::platform {

void TraceTerminalLife(std::string_view event, int pid, bool with_stack);

}
