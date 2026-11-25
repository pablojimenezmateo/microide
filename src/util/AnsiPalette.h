#pragma once

#include "util/Rgba8.h"

namespace microide::util {

Rgba8 BasicAnsiColor(int index, bool bright);

Rgba8 Ansi256Color(int index);

}
