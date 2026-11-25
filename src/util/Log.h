#pragma once

#include <functional>
#include <string_view>

namespace microide::util {

using LogSink = std::function<void(std::string_view)>;

void SetLogSink(LogSink sink);

void Log(std::string_view message);

}
