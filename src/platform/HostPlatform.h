#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace microide::platform {

void IgnoreBrokenPipeSignal();

void AllowParentToTraceWhenAskedByQa();

std::string UseUtf8CharacterLocale();

std::filesystem::path ProcessWorkingDirectory(int pid);

bool InterruptProcess(int pid);

}
