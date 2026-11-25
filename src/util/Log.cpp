#include "util/Log.h"

#include <cstdio>
#include <utility>

namespace microide::util {

namespace {

LogSink& InstalledSink() {
  static LogSink sink;
  return sink;
}

}

void SetLogSink(LogSink sink) { InstalledSink() = std::move(sink); }

void Log(std::string_view message) {
  if (const LogSink& sink = InstalledSink()) {
    sink(message);
    return;
  }
  std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
}

}
